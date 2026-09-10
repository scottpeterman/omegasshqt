// capi/serial.go
// Serial port enumeration.
//
// Ports cross as data, not as a handle -- the same pattern as the vault
// metadata surface. That is what lets the quick-connect form render whatever
// the shim returns instead of Qt enumerating ports itself, which would mean
// QSerialPort in the build and in windeployqt for the sake of a list of
// strings.
//
// Note for the macOS link line: the detailed enumerator is cgo on darwin
// (CoreFoundation and IOKit); the port I/O itself is pure Go on all three
// platforms. Pathfinder ships this and it works, but Pathfinder is a Go
// binary so cgo's LDFLAGS resolve during Go's own link. Here the archive is
// linked by CMake, so those frameworks have to reach the final C++ link line
// explicitly -- see the APPLE branch in the top-level CMakeLists.txt.
package main

/*
#include <stdlib.h>
*/
import "C"

import (
	"encoding/json"

	"github.com/scottpeterman/omegassh/serialx"
)

// portJSON is the wire form. Named fields rather than a bare string array so
// a form can show "usbserial-FTDI (0403:6001)" next to the device name, which
// is the difference between picking the right adapter and guessing.
type portJSON struct {
	Name         string `json:"name"`
	IsUSB        bool   `json:"is_usb"`
	VID          string `json:"vid,omitempty"`
	PID          string `json:"pid,omitempty"`
	SerialNumber string `json:"serial_number,omitempty"`
}

// Returns the available serial ports as a JSON array, or NULL on failure with
// the reason in omegassh_last_error. The caller frees a non-NULL result with
// omegassh_free.
//
//	[{"name":"/dev/ttyUSB0","is_usb":true,"vid":"0403","pid":"6001",
//	  "serial_number":"AB0KM1XY"}]
//
// An empty array is a valid answer and means no ports, not a failure.
//
// If the detailed enumeration fails -- a permissions problem reading the USB
// metadata, a platform quirk -- this falls back to the bare port list rather
// than returning nothing. A form showing device names with no vendor strings
// is still usable; an empty form is not.
//
//export omegassh_serial_ports
func omegassh_serial_ports() *C.char {
	var out []portJSON

	detailed, err := serialx.ListDetailed()
	if err == nil {
		out = make([]portJSON, 0, len(detailed))
		for _, p := range detailed {
			out = append(out, portJSON{
				Name:         p.Name,
				IsUSB:        p.IsUSB,
				VID:          p.VID,
				PID:          p.PID,
				SerialNumber: p.SerialNumber,
			})
		}
	} else {
		names, listErr := serialx.List()
		if listErr != nil {
			setErr("enumerate serial ports: %v (detailed enumeration also failed: %v)",
				listErr, err)
			return nil
		}
		out = make([]portJSON, 0, len(names))
		for _, n := range names {
			out = append(out, portJSON{Name: n})
		}
	}

	blob, jerr := json.Marshal(out)
	if jerr != nil {
		setErr("encode serial ports: %v", jerr)
		return nil
	}
	return C.CString(string(blob))
}
