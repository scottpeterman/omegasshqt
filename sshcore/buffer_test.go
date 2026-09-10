// sshcore/buffer_test.go
package sshcore

import (
	"bytes"
	"errors"
	"io"
	"runtime"
	"sync"
	"sync/atomic"
	"testing"
	"time"
)

func TestOutputBufferPumpAndDrain(t *testing.T) {
	var wakes int32
	b := NewOutputBuffer(func() { atomic.AddInt32(&wakes, 1) })

	done := make(chan struct{})
	go func() {
		b.Pump(bytes.NewReader([]byte("eng-leaf-1#")))
		close(done)
	}()
	<-done

	if !b.Done() {
		t.Fatal("Done should be true after the reader ends")
	}
	if b.Err() != nil {
		t.Fatalf("clean EOF should not surface an error, got %v", b.Err())
	}
	if got := b.Pending(); got != 11 {
		t.Fatalf("Pending = %d, want 11", got)
	}
	if got := atomic.LoadInt32(&wakes); got < 1 {
		t.Fatalf("expected at least one wake, got %d", got)
	}

	dst := make([]byte, 64)
	n := b.Drain(dst)
	if string(dst[:n]) != "eng-leaf-1#" {
		t.Fatalf("Drain = %q", dst[:n])
	}
	if n2 := b.Drain(dst); n2 != 0 {
		t.Fatalf("second Drain = %d, want 0", n2)
	}
}

func TestOutputBufferPartialDrainPreservesOrder(t *testing.T) {
	b := NewOutputBuffer(nil)
	go b.Pump(bytes.NewReader([]byte("abcdefghij")))

	waitPending(t, b, 10)

	var got []byte
	small := make([]byte, 3)
	for {
		n := b.Drain(small)
		if n == 0 {
			break
		}
		got = append(got, small[:n]...)
	}
	if string(got) != "abcdefghij" {
		t.Fatalf("reassembled = %q, want abcdefghij", got)
	}
}

// A short read must not be mistaken for EOF: the pty hands over whatever has
// arrived, so a full-screen redraw routinely lands in several chunks.
func TestOutputBufferAccumulatesAcrossReads(t *testing.T) {
	pr, pw := io.Pipe()
	b := NewOutputBuffer(nil)
	go b.Pump(pr)

	for _, chunk := range []string{"\x1b[2J", "\x1b[H", "usa-spine-1#"} {
		if _, err := pw.Write([]byte(chunk)); err != nil {
			t.Fatal(err)
		}
	}
	waitPending(t, b, 4+3+12)
	if b.Done() {
		t.Fatal("Done should be false while the writer is open")
	}
	pw.Close()

	deadline := time.Now().Add(2 * time.Second)
	for !b.Done() && time.Now().Before(deadline) {
		time.Sleep(5 * time.Millisecond)
	}
	if !b.Done() {
		t.Fatal("Done should be true after the writer closes")
	}

	dst := make([]byte, 128)
	n := b.Drain(dst)
	if string(dst[:n]) != "\x1b[2J\x1b[Husa-spine-1#" {
		t.Fatalf("Drain = %q", dst[:n])
	}
}

func TestOutputBufferSurfacesReadError(t *testing.T) {
	want := errors.New("connection reset")
	b := NewOutputBuffer(nil)
	b.Pump(errReader{err: want})

	if !b.Done() {
		t.Fatal("Done should be true after a failed read")
	}
	if !errors.Is(b.Err(), want) {
		t.Fatalf("Err = %v, want %v", b.Err(), want)
	}
}

// Close must stop wakes: the shim closes the buffer before the notifier, and
// a late wake would poke a descriptor that is about to be closed.
func TestOutputBufferCloseStopsWakes(t *testing.T) {
	var wakes int32
	b := NewOutputBuffer(func() { atomic.AddInt32(&wakes, 1) })

	pr, pw := io.Pipe()
	go b.Pump(pr)
	pw.Write([]byte("first"))
	waitPending(t, b, 5)

	b.Close()
	before := atomic.LoadInt32(&wakes)

	pw.Write([]byte("second"))
	pw.Close()
	time.Sleep(50 * time.Millisecond)

	if got := atomic.LoadInt32(&wakes); got != before {
		t.Fatalf("wakes after Close: %d, want %d", got, before)
	}
	if got := b.Pending(); got != 0 {
		t.Fatalf("Close should drop buffered bytes, Pending = %d", got)
	}
	b.Close() // idempotent
}

func TestOutputBufferConcurrentDrain(t *testing.T) {
	b := NewOutputBuffer(nil)
	pr, pw := io.Pipe()
	go b.Pump(pr)

	var wg sync.WaitGroup
	var total int64
	var timedOut int32
	for i := 0; i < 4; i++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			dst := make([]byte, 16)
			deadline := time.Now().Add(2 * time.Second)
			for time.Now().Before(deadline) {
				n := b.Drain(dst)
				atomic.AddInt64(&total, int64(n))
				if n != 0 {
					continue
				}
				if !b.Done() {
					// Nothing pending and not finished: yield rather
					// than spin. Four goroutines in a tight Drain loop
					// starve the pump and the writer of CPU on a small
					// machine, and the symptom is this test hitting its
					// deadline with the buffer half full. A real
					// consumer blocks on its wake primitive here, which
					// is the same thing at a coarser grain.
					runtime.Gosched()
					continue
				}
				// Done only says Pump has finished. A chunk can have
				// landed between the Drain above and this check --
				// appended, and then eof set -- so returning here is
				// what left bytes behind and made this test flaky.
				//
				// Draining to empty now IS terminal: eof is set under
				// the same mutex as every append, so once Done reports
				// true there is no producer left to race with.
				for {
					n := b.Drain(dst)
					if n == 0 {
						return
					}
					atomic.AddInt64(&total, int64(n))
				}
			}
			// Distinct from a short count: a goroutine that runs out of
			// clock has not proved anything about the buffer, and
			// reporting it as a byte mismatch sends the next reader to
			// the wrong place.
			atomic.StoreInt32(&timedOut, 1)
		}()
	}
	for i := 0; i < 200; i++ {
		pw.Write([]byte("0123456789"))
	}
	pw.Close()
	wg.Wait()

	if atomic.LoadInt32(&timedOut) != 0 {
		t.Fatal("a drain goroutine hit its deadline before the buffer reported Done")
	}
	if total != 2000 {
		t.Fatalf("drained %d bytes across goroutines, want 2000", total)
	}
}

type errReader struct{ err error }

func (e errReader) Read([]byte) (int, error) { return 0, e.err }

func waitPending(t *testing.T, b *OutputBuffer, want int) {
	t.Helper()
	deadline := time.Now().Add(2 * time.Second)
	for time.Now().Before(deadline) {
		if b.Pending() >= want {
			return
		}
		time.Sleep(5 * time.Millisecond)
	}
	t.Fatalf("timed out waiting for %d pending bytes, have %d", want, b.Pending())
}
