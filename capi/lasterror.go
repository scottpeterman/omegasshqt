// capi/lasterror.go
//
// Detail text for the last failing call, keyed by the OS thread that made it.
//
// This was a single global string, and it worked as long as one thread used
// the library. It does not survive a Qt application: a worker dialing a host
// and a worker unlocking a vault share the variable, and whichever fails
// second overwrites the message the first is about to read. The failure is
// silent and the symptom is a plausible, wrong error in a dialog.
//
// Keyed on the C thread rather than a goroutine because that is the identity
// the caller has. cgo pins the calling thread for the duration of an exported
// call, so a C++ caller that checks a return code and then reads the text is
// guaranteed its own -- the two calls happen on the same thread, and no other
// thread can write that key.
//
// The other half of the fix is clearing on success. Reading the text after an
// OK return used to hand back whatever failed earlier in the process, which
// reads as an error attached to a call that worked.
package main

/*
#cgo !windows LDFLAGS: -lpthread
#include <stdint.h>
#ifdef _WIN32
#include <windows.h>
static unsigned long long omegassh_thread_id(void) {
    return (unsigned long long)GetCurrentThreadId();
}
#else
#include <pthread.h>
static unsigned long long omegassh_thread_id(void) {
    return (unsigned long long)(uintptr_t)pthread_self();
}
#endif
*/
import "C"

import (
	"fmt"
	"sync"
)

// errDetail maps thread id -> most recent message on that thread. Entries are
// small and bounded by the number of threads that have ever called in, which
// for a GUI is a handful.
var errDetail sync.Map

// setErr records why the current call failed. Both surfaces use it.
func setErr(format string, a ...interface{}) {
	errDetail.Store(uint64(C.omegassh_thread_id()), fmt.Sprintf(format, a...))
}

// clearErr drops the message for this thread, so a later successful call does
// not leave an older failure readable.
func clearErr() {
	errDetail.Delete(uint64(C.omegassh_thread_id()))
}

//export omegassh_last_error
func omegassh_last_error() *C.char {
	if v, ok := errDetail.Load(uint64(C.omegassh_thread_id())); ok {
		return C.CString(v.(string))
	}
	return C.CString("")
}
