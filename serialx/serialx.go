// serialx/serialx.go
// Serial transport for the interactive terminal.
//
// Carried over from the tetherssh serialx package, which had already been
// settled against real adapters on Linux, macOS and Windows. The transport
// logic below — enumeration, mode mapping, the lock discipline that keeps a
// blocked Read from deadlocking Close — is unchanged from that version and
// deliberately so. Four things were added to satisfy transport.Transport:
//
//   - Resize takes a transport.Size. Still a no-op: a serial console has no
//     window-change concept. It returns nil rather than an error on purpose,
//     so the widget above can call Resize on every layout pass without
//     knowing which transport it is driving.
//   - Done/Err. SSH learns of its own death from a separate Wait; a serial
//     port has no such channel, and the only evidence the far end went away
//     is an error out of Read. So Read and Write record that error and close
//     done. Callers still get the error back unchanged.
//   - IsConnected now follows the same state. Previously a mid-session unplug
//     left it reporting true until something called Close, because nothing
//     cleared the flag on a read failure.
//   - openPort exists so the connect/read/close/unplug sequence can be tested
//     without hardware. It is the only seam; everything else is the shipped
//     path.
//
// A Backend is single-use, matching the SSH session: once closed it stays
// closed, and reconnecting means building a new one. Reconnect policy belongs
// above the transport, not inside it.
package serialx

import (
	"errors"
	"fmt"
	"sync"
	"time"

	"go.bug.st/serial"
	"go.bug.st/serial/enumerator"

	"github.com/scottpeterman/omegassh/transport"
)

// ErrNotConnected is returned by Read/Write before Connect (or after Close).
var ErrNotConnected = errors.New("serial: not connected")

// ErrClosed is returned by Connect once the Backend has been closed. A
// Backend is single-use; build a new one to reconnect.
var ErrClosed = errors.New("serial: backend is closed")

// openPort is the seam the tests replace. In every shipped build it is
// serial.Open and nothing else.
var openPort = serial.Open

// Config describes a serial connection. Parity and StopBits are strings so
// callers (YAML session files, CLI flags, the session-editor UI) don't have to
// import go.bug.st/serial just to name a setting.
type Config struct {
	Port     string // OS port name: COM3, /dev/ttyUSB0, /dev/cu.usbserial-XXXX
	Baud     int    // default 9600
	DataBits int    // 5-8, default 8
	Parity   string // none|odd|even|mark|space, default none
	StopBits string // 1|1.5|2, default 1

	// ReadTimeout controls Read() blocking. Zero or negative blocks until a
	// byte arrives (recommended for an interactive console). A positive value
	// makes Read return after the interval, possibly with zero bytes.
	ReadTimeout time.Duration
}

func (c Config) withDefaults() Config {
	if c.Baud == 0 {
		c.Baud = 9600
	}
	if c.DataBits == 0 {
		c.DataBits = 8
	}
	if c.Parity == "" {
		c.Parity = "none"
	}
	if c.StopBits == "" {
		c.StopBits = "1"
	}
	return c
}

// Summary is a short human-readable form, e.g. "9600 8N1".
func (c Config) Summary() string {
	c = c.withDefaults()
	p := "N"
	switch c.Parity {
	case "odd":
		p = "O"
	case "even":
		p = "E"
	case "mark":
		p = "M"
	case "space":
		p = "S"
	}
	return fmt.Sprintf("%d %d%s%s", c.Baud, c.DataBits, p, c.StopBits)
}

func (c Config) mode() (*serial.Mode, error) {
	c = c.withDefaults()

	var parity serial.Parity
	switch c.Parity {
	case "none":
		parity = serial.NoParity
	case "odd":
		parity = serial.OddParity
	case "even":
		parity = serial.EvenParity
	case "mark":
		parity = serial.MarkParity
	case "space":
		parity = serial.SpaceParity
	default:
		return nil, fmt.Errorf("serial: unknown parity %q", c.Parity)
	}

	var stop serial.StopBits
	switch c.StopBits {
	case "1":
		stop = serial.OneStopBit
	case "1.5":
		stop = serial.OnePointFiveStopBits
	case "2":
		stop = serial.TwoStopBits
	default:
		return nil, fmt.Errorf("serial: unknown stop bits %q", c.StopBits)
	}

	if c.DataBits < 5 || c.DataBits > 8 {
		return nil, fmt.Errorf("serial: data bits out of range: %d", c.DataBits)
	}

	return &serial.Mode{
		BaudRate: c.Baud,
		DataBits: c.DataBits,
		Parity:   parity,
		StopBits: stop,
	}, nil
}

// Validate reports whether this configuration describes a mode a port could
// be opened in, without opening one. Connect makes the same checks on its way
// through; this exists so a caller that has moved Connect onto its own
// goroutine can still refuse "parity: maybe" as the configuration error it is,
// in the same answer that would have refused a missing device name.
func (c Config) Validate() error {
	_, err := c.mode()
	return err
}

// Backend is a serial connection satisfying transport.Transport.
type Backend struct {
	cfg Config

	mu   sync.Mutex
	port serial.Port
	open bool
	shut bool // Close was called; Connect is refused from here on

	done     chan struct{}
	doneOnce sync.Once
	err      error
}

// New builds a Backend from cfg, applying defaults. It does not open the port;
// call Connect.
func New(cfg Config) *Backend {
	return &Backend{
		cfg:  cfg.withDefaults(),
		done: make(chan struct{}),
	}
}

// Config returns the effective (defaulted) configuration.
func (b *Backend) Config() Config { return b.cfg }

// Connect opens the serial port. Unlike SSH there is no auth, host-key, or
// keepalive step -- opening the port is the whole handshake.
func (b *Backend) Connect() error {
	b.mu.Lock()
	if b.shut {
		b.mu.Unlock()
		return ErrClosed
	}
	if b.open {
		b.mu.Unlock()
		return errors.New("serial: already connected")
	}
	b.mu.Unlock()

	mode, err := b.cfg.mode()
	if err != nil {
		return err
	}

	p, err := openPort(b.cfg.Port, mode)
	if err != nil {
		return fmt.Errorf("open %s: %w", b.cfg.Port, err)
	}

	// Set the read mode explicitly so behavior is deterministic across OSes
	// instead of relying on the library default.
	to := b.cfg.ReadTimeout
	if to <= 0 {
		to = serial.NoTimeout // block until a byte is available
	}
	if err := p.SetReadTimeout(to); err != nil {
		_ = p.Close()
		return fmt.Errorf("set read timeout: %w", err)
	}

	b.mu.Lock()
	b.port = p
	b.open = true
	b.mu.Unlock()
	return nil
}

// Read reads from the port. The port pointer is fetched under the lock and the
// blocking Read is then called without holding it, so a blocked Read never
// deadlocks Close.
//
// An error here is how a serial session dies -- there is nothing else to
// watch -- so it is also what closes Done.
func (b *Backend) Read(p []byte) (int, error) {
	b.mu.Lock()
	port := b.port
	b.mu.Unlock()
	if port == nil {
		return 0, ErrNotConnected
	}
	n, err := port.Read(p)
	if err != nil {
		b.finish(err)
	}
	return n, err
}

// Write writes to the port. go.bug.st/serial permits a Write concurrent with a
// Read on the same port, which is exactly how the read/write loops use it.
func (b *Backend) Write(p []byte) (int, error) {
	b.mu.Lock()
	port := b.port
	b.mu.Unlock()
	if port == nil {
		return 0, ErrNotConnected
	}
	n, err := port.Write(p)
	if err != nil {
		b.finish(err)
	}
	return n, err
}

// Resize is a no-op: a serial console has no window-change concept. The widget
// still measures and lays out rows/cols locally; nothing is signaled
// downstream. It returns nil rather than an error so callers do not have to
// branch on which transport they hold.
func (b *Backend) Resize(transport.Size) error { return nil }

// Done is closed when the port goes away, whether by unplug, read failure, or
// a local Close.
func (b *Backend) Done() <-chan struct{} { return b.done }

// Err reports why the session ended; meaningful only once Done is closed. A
// local Close reports nil, matching the SSH side: an operator disconnecting is
// not a failure and should not surface as one.
func (b *Backend) Err() error {
	b.mu.Lock()
	defer b.mu.Unlock()
	return b.err
}

// Close closes the port and unblocks any in-flight Read.
func (b *Backend) Close() error {
	b.mu.Lock()
	port := b.port
	b.port = nil
	b.shut = true
	b.mu.Unlock()

	// Recorded before the port is torn down, so the read error that Close
	// itself provokes cannot overwrite it with a spurious failure.
	b.finish(nil)

	if port == nil {
		return nil
	}
	return port.Close()
}

// IsConnected reports whether the port is currently usable. It goes false on a
// read or write failure, not just on Close -- an unplugged adapter is not a
// connection, however the flag was last set.
func (b *Backend) IsConnected() bool {
	b.mu.Lock()
	defer b.mu.Unlock()
	return b.open
}

// finish records the terminating error, clears the connected flag and closes
// done, at most once.
func (b *Backend) finish(err error) {
	b.doneOnce.Do(func() {
		b.mu.Lock()
		b.err = err
		b.open = false
		b.mu.Unlock()
		close(b.done)
	})
}

// PortInfo is the subset of enumerator metadata that is stable across the
// library's supported platforms.
type PortInfo struct {
	Name         string
	IsUSB        bool
	VID          string
	PID          string
	SerialNumber string
}

// List returns bare port names (serial.GetPortsList). Pure Go on every
// supported platform.
func List() ([]string, error) {
	return serial.GetPortsList()
}

// ListDetailed returns ports with USB metadata where the OS provides it.
//
// Note for build planning: the enumerator's darwin implementation is cgo
// (CoreFoundation + IOKit). Linux and Windows are pure Go. This is the only
// function in the package that carries that constraint.
func ListDetailed() ([]PortInfo, error) {
	ports, err := enumerator.GetDetailedPortsList()
	if err != nil {
		return nil, err
	}
	out := make([]PortInfo, 0, len(ports))
	for _, p := range ports {
		out = append(out, PortInfo{
			Name:         p.Name,
			IsUSB:        p.IsUSB,
			VID:          p.VID,
			PID:          p.PID,
			SerialNumber: p.SerialNumber,
		})
	}
	return out, nil
}

// compile-time check that the serial backend is a usable Transport.
var _ transport.Transport = (*Backend)(nil)
