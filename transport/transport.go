// transport/transport.go
// Package transport is the contract every omegassh session satisfies,
// whatever is underneath it: SSH, telnet, serial.
//
// It exists because telnetx and serialx arrived from Pathfinder coupled to
// that project's internal/term package by exactly three symbols -- Size,
// Size.Valid and Transport. Rather than carry an import of another
// application's internals, those three are redeclared here with the same
// shapes, so the two packages compile unmodified apart from the import line
// and their existing suites remain the evidence that nothing changed.
//
// Above the byte-level Transport sits Session (session.go), which is the part
// that could not be lifted from any backend: the state machine and event
// contract the UI renders. Both live in this package so the layering is one
// import rather than two.
//
// Nothing here imports telnetx or serialx. The dependency runs one way: those
// packages implement Transport, this package wraps whatever it is handed.
package transport

import (
	"fmt"
	"io"
	"strings"
)

// Size is a terminal window geometry in character cells.
//
// Kept as a plain value with exported fields because it crosses the cgo
// boundary as two ints and appears in saved session config; a caller naming a
// window size should not have to import a transport package to do it.
type Size struct {
	Cols int
	Rows int
}

// Valid reports whether both dimensions are positive. A zero Size is what a
// widget reports before its first layout pass, and pushing that downstream
// tells a device its window is 0x0.
func (s Size) Valid() bool { return s.Cols > 0 && s.Rows > 0 }

// String renders a size as "132x40".
func (s Size) String() string { return fmt.Sprintf("%dx%d", s.Cols, s.Rows) }

// Transport is a byte-level connection to a device.
//
// Read and Write are the whole data path: the same OutputBuffer.Pump drives
// every implementation, so there is one read loop, one buffer, one notify and
// one logging tap for all of them.
//
// Done, Err and IsConnected are the liveness story. A transport that dies on
// its own -- a dropped socket, an unplugged adapter -- closes Done and records
// why in Err. A local Close also closes Done but records nil: an operator
// disconnecting is not a failure and must not surface as one.
type Transport interface {
	io.Reader
	io.Writer

	// Connect performs whatever handshake the transport has. A Transport is
	// single-use: once closed it stays closed, and reconnecting means
	// building a new one. Reconnect policy belongs above this interface.
	Connect() error

	// Resize reports a new window geometry. Implementations that have no
	// window concept return nil rather than an error, so a caller can resize
	// on every layout pass without branching on what it holds.
	Resize(Size) error

	// Done is closed once the session has ended, for any reason.
	Done() <-chan struct{}

	// Err reports why the session ended. Meaningful only once Done is
	// closed; nil for a local Close.
	Err() error

	// IsConnected reports whether the transport is currently usable. It goes
	// false on a read or write failure, not only on Close.
	IsConnected() bool

	// Close ends the session and unblocks any in-flight Read.
	Close() error
}

// Kind names which transport a session is running over. It crosses the cgo
// boundary as the lowercase string form, so a caller selects a transport
// without any knowledge of this enum's numbering.
type Kind int

const (
	KindSSH Kind = iota
	KindTelnet
	KindSerial
)

// String is the wire form: "ssh", "telnet", "serial".
func (k Kind) String() string {
	switch k {
	case KindSSH:
		return "ssh"
	case KindTelnet:
		return "telnet"
	case KindSerial:
		return "serial"
	}
	return fmt.Sprintf("kind(%d)", int(k))
}

// ParseKind maps the wire form to a Kind. An empty string is SSH, so a caller
// that predates the transport selector keeps working unchanged.
func ParseKind(s string) (Kind, error) {
	switch strings.ToLower(strings.TrimSpace(s)) {
	case "", "ssh":
		return KindSSH, nil
	case "telnet":
		return KindTelnet, nil
	case "serial":
		return KindSerial, nil
	}
	return KindSSH, fmt.Errorf("unknown transport %q (want ssh, telnet or serial)", s)
}

// State is a session's position in its lifecycle.
//
// The six values are nterm-qt's, not a superset invented here, so the
// connection overlay displays what the transport publishes rather than
// mapping one vocabulary onto another.
//
// Not every transport reaches every state, and none of them fake one to look
// uniform:
//
//   - StateAuthenticating is SSH only. Telnet's login prompt, where there is
//     one, is ordinary session data arriving after the socket is up; the
//     protocol has no authentication step to be in.
//   - StateConnecting is SSH and telnet. Opening a serial port is the whole
//     handshake and it either succeeds or fails, so serial goes from
//     disconnected straight to connected.
//   - StateReconnecting is published by the reconnect policy above the
//     transport, which does not exist yet. No transport enters it on its own.
type State int

const (
	StateDisconnected State = iota
	StateConnecting
	StateAuthenticating
	StateConnected
	StateReconnecting
	StateFailed
)

// String is the wire form, and what the overlay renders.
func (s State) String() string {
	switch s {
	case StateDisconnected:
		return "disconnected"
	case StateConnecting:
		return "connecting"
	case StateAuthenticating:
		return "authenticating"
	case StateConnected:
		return "connected"
	case StateReconnecting:
		return "reconnecting"
	case StateFailed:
		return "failed"
	}
	return fmt.Sprintf("state(%d)", int(s))
}

// EventKind is the category of a session event.
//
// EventData is declared for a Go consumer that would rather be pushed bytes
// than poll a buffer. The cgo shim does not use it: bytes cross that boundary
// through OutputBuffer precisely so no Go-created thread ever calls into Qt.
//
// EventInteractionRequired is declared and not yet emitted. The dial is
// asynchronous as of Phase 2b, so it now CAN park in StateAuthenticating and
// publish a question; what is still missing is the answer travelling back,
// which for the cgo shim means one more call alongside omegassh_write.
// Declaring the vocabulary now fixed it early; it still does not claim the
// machine exists.
type EventKind int

const (
	EventStateChanged EventKind = iota
	EventData
	EventError
	EventInteractionRequired
)

// String is the wire form.
func (e EventKind) String() string {
	switch e {
	case EventStateChanged:
		return "state_changed"
	case EventData:
		return "data"
	case EventError:
		return "error"
	case EventInteractionRequired:
		return "interaction_required"
	}
	return fmt.Sprintf("event(%d)", int(e))
}

// Interaction is a challenge the far end put to the operator: a
// keyboard-interactive prompt, an MFA code, a security-key touch.
//
// Echo false means the answer is a secret and must not be shown as it is
// typed. It is the same flag sshcore's Config.AuthPrompt carries.
type Interaction struct {
	Question string `json:"question"`
	Echo     bool   `json:"echo"`
}

// Event is one thing a session has to say for itself. Exactly one of the
// optional fields is meaningful, chosen by Kind.
type Event struct {
	Kind  EventKind    `json:"kind"`
	State State        `json:"state,omitempty"`
	Error string       `json:"error,omitempty"`
	Data  []byte       `json:"-"`
	Ask   *Interaction `json:"ask,omitempty"`
}
