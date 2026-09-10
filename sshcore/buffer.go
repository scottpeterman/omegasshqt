// sshcore/buffer.go
// Output buffering between a reader goroutine and a foreign event loop.
//
// The cgo shim cannot hand bytes upward the moment they arrive: a Go
// goroutine calling into C++ lands on a thread the GUI toolkit knows nothing
// about. So output accumulates here and the consumer drains it on its own
// thread, woken by whatever primitive its platform provides.
//
// Kept in sshcore rather than in the shim so it is ordinary Go with ordinary
// tests. It has no idea what "wake" actually does.
package sshcore

import (
	"io"
	"sync"
)

// OutputBuffer collects bytes from a producer and hands them to a consumer
// running on a different thread. Safe for concurrent use.
type OutputBuffer struct {
	mu     sync.Mutex
	buf    []byte
	eof    bool
	err    error
	closed bool

	// wake is called after bytes are appended and after EOF. It must not
	// block and must not call back into the buffer.
	wake func()

	// wakeMu serializes a wake against Close, so Close can promise that no
	// callback is running when it returns rather than only that none will
	// start. Held around the callback itself, which is why wake must not
	// block -- see notify.
	wakeMu sync.Mutex
}

// NewOutputBuffer returns a buffer that calls wake whenever the consumer has
// something new to look at. wake may be nil for polling consumers.
func NewOutputBuffer(wake func()) *OutputBuffer {
	return &OutputBuffer{wake: wake}
}

// Pump copies r into the buffer until it ends, then records EOF and wakes
// the consumer one last time. Intended to run in its own goroutine.
func (b *OutputBuffer) Pump(r io.Reader) {
	chunk := make([]byte, 32*1024)
	for {
		n, err := r.Read(chunk)
		if n > 0 {
			b.mu.Lock()
			if b.closed {
				b.mu.Unlock()
				return
			}
			b.buf = append(b.buf, chunk[:n]...)
			b.mu.Unlock()
			b.notify()
		}
		if err != nil {
			b.mu.Lock()
			b.eof = true
			if err != io.EOF {
				b.err = err
			}
			b.mu.Unlock()
			b.notify()
			return
		}
	}
}

// notify calls the wake callback, if there is one and the buffer is still open.
//
// The check and the call are both under wakeMu, and Close takes the same lock.
// Reading closed under b.mu, releasing it, and only then calling wake() leaves
// a window: Close can run in between, and the wake lands afterwards -- poking
// a descriptor the consumer has already torn down, which is exactly what
// Close exists to prevent. It is a narrow window and it does fire; widening it
// artificially turns TestOutputBufferCloseStopsWakes from intermittent into
// deterministic.
//
// wakeMu is separate from b.mu so a caller-supplied callback never runs under
// the lock guarding the byte slice -- a wake must not be able to block Drain.
// Holding it across the call is safe because the notifier's write end is
// non-blocking: wake() cannot stall on a full pipe, so Close cannot be held up
// by one.
func (b *OutputBuffer) notify() {
	b.wakeMu.Lock()
	defer b.wakeMu.Unlock()

	b.mu.Lock()
	wake := b.wake
	closed := b.closed
	b.mu.Unlock()

	if wake != nil && !closed {
		wake()
	}
}

// Drain moves up to len(dst) buffered bytes into dst and returns the count.
// Zero means nothing is pending; it never blocks and never signals EOF, so
// check Done separately.
func (b *OutputBuffer) Drain(dst []byte) int {
	b.mu.Lock()
	defer b.mu.Unlock()
	if len(b.buf) == 0 || len(dst) == 0 {
		return 0
	}
	n := copy(dst, b.buf)
	// Reslice rather than reallocate; the tail is reused on the next append.
	b.buf = b.buf[n:]
	if len(b.buf) == 0 {
		b.buf = b.buf[:0]
	}
	return n
}

// Pending reports how many bytes are waiting to be drained.
func (b *OutputBuffer) Pending() int {
	b.mu.Lock()
	defer b.mu.Unlock()
	return len(b.buf)
}

// Done reports whether the producer has finished. Bytes may still be
// buffered after Done returns true — drain until empty before acting on it.
func (b *OutputBuffer) Done() bool {
	b.mu.Lock()
	defer b.mu.Unlock()
	return b.eof
}

// Err returns the producer's terminating error, if it was not a clean EOF.
func (b *OutputBuffer) Err() error {
	b.mu.Lock()
	defer b.mu.Unlock()
	return b.err
}

// Close stops further wakes and drops anything still buffered. Idempotent.
//
// Taking wakeMu makes the guarantee in the name real: on return, no wake is
// running and none will start. Without it, Close only promised that no wake
// would BEGIN afterwards, which is not the same thing and not what the shim
// needs -- it closes the buffer and then the notifier, and a wake in flight
// across that gap writes to a closed descriptor.
func (b *OutputBuffer) Close() {
	b.wakeMu.Lock()
	defer b.wakeMu.Unlock()

	b.mu.Lock()
	b.closed = true
	b.wake = nil
	b.buf = nil
	b.mu.Unlock()
}
