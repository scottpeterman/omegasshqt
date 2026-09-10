// internal/privatefile/privatefile_windows.go
//go:build windows

// The Windows half. Everything here exists because Windows has no mode bits
// to set, so the equivalent of chmod 0600 is: build a DACL with exactly one
// allow-ACE for the calling user, and mark it PROTECTED so the parent
// directory's inheritable ACEs do not get merged back in.
//
// PROTECTED is the part that is easy to leave out and the part that matters.
// Without it the ACE is added to whatever the directory hands down, which
// commonly includes Users on a machine-wide path -- the file then has a
// perfectly good owner ACE and is still world-readable.
//
// golang.org/x/sys/windows is already a direct dependency of this module
// (capi/notify_windows.go), so none of this adds one.
package privatefile

import (
	"fmt"
	"os"
	"unsafe"

	"golang.org/x/sys/windows"
)

func harden(path string, dir bool) error {
	if _, err := os.Stat(path); err != nil {
		return fmt.Errorf("restrict %s: %w", path, err)
	}

	sid, err := currentUserSID()
	if err != nil {
		return fmt.Errorf("restrict %s: %w", path, err)
	}

	// A directory's ACE is marked inheritable so files created in it later
	// start private. A file's is not: an inheritable ACE on a leaf object is
	// meaningless and only confuses anyone reading the ACL afterwards.
	inheritance := uint32(windows.NO_INHERITANCE)
	if dir {
		inheritance = windows.SUB_CONTAINERS_AND_OBJECTS_INHERIT
	}

	dacl, err := windows.ACLFromEntries([]windows.EXPLICIT_ACCESS{{
		AccessPermissions: windows.GENERIC_ALL,
		AccessMode:        windows.GRANT_ACCESS,
		Inheritance:       inheritance,
		Trustee: windows.TRUSTEE{
			TrusteeForm:  windows.TRUSTEE_IS_SID,
			TrusteeType:  windows.TRUSTEE_IS_USER,
			TrusteeValue: windows.TrusteeValueFromSID(sid),
		},
	}}, nil)
	if err != nil {
		return fmt.Errorf("restrict %s: build DACL: %w", path, err)
	}

	err = windows.SetNamedSecurityInfo(
		path,
		windows.SE_FILE_OBJECT,
		windows.DACL_SECURITY_INFORMATION|windows.PROTECTED_DACL_SECURITY_INFORMATION,
		nil, nil, dacl, nil,
	)
	if err != nil {
		return fmt.Errorf("restrict %s: set DACL: %w", path, err)
	}
	return nil
}

func isPrivate(path string) (bool, error) {
	sd, err := windows.GetNamedSecurityInfo(path, windows.SE_FILE_OBJECT,
		windows.DACL_SECURITY_INFORMATION)
	if err != nil {
		return false, fmt.Errorf("read DACL of %s: %w", path, err)
	}
	dacl, _, err := sd.DACL()
	if err != nil {
		return false, fmt.Errorf("read DACL of %s: %w", path, err)
	}
	// A present-but-NULL DACL is not an empty one. It grants everyone full
	// access, which is the least private a file can be.
	if dacl == nil {
		return false, nil
	}

	allowed, err := allowedTrustees()
	if err != nil {
		return false, err
	}

	for i := uint32(0); i < uint32(dacl.AceCount); i++ {
		var ace *windows.ACCESS_ALLOWED_ACE
		if err := windows.GetAce(dacl, i, &ace); err != nil {
			return false, fmt.Errorf("read ACE %d of %s: %w", i, path, err)
		}
		// Deny ACEs only ever subtract, so they cannot make a file less
		// private. Anything else granting access has to be on the list.
		if ace.Header.AceType != windows.ACCESS_ALLOWED_ACE_TYPE {
			continue
		}
		sid := (*windows.SID)(unsafe.Pointer(&ace.SidStart))
		if !anyEquals(allowed, sid) {
			return false, nil
		}
	}
	return true, nil
}

// currentUserSID is the SID of the account this process runs as. The SID in a
// Tokenuser points into a buffer owned by that call, so it is copied out
// before the buffer goes out of scope.
func currentUserSID() (*windows.SID, error) {
	user, err := windows.GetCurrentProcessToken().GetTokenUser()
	if err != nil {
		return nil, fmt.Errorf("look up current user: %w", err)
	}
	sid, err := user.User.Sid.Copy()
	if err != nil {
		return nil, fmt.Errorf("copy current user SID: %w", err)
	}
	return sid, nil
}

// allowedTrustees are the SIDs whose access does not count as a leak: the
// caller, LocalSystem, and BUILTIN\Administrators. See IsPrivate for why the
// last two are on the list.
func allowedTrustees() ([]*windows.SID, error) {
	self, err := currentUserSID()
	if err != nil {
		return nil, err
	}
	out := []*windows.SID{self}
	for _, wk := range []windows.WELL_KNOWN_SID_TYPE{
		windows.WinLocalSystemSid,
		windows.WinBuiltinAdministratorsSid,
	} {
		sid, err := windows.CreateWellKnownSid(wk)
		if err != nil {
			return nil, fmt.Errorf("look up well-known SID %d: %w", wk, err)
		}
		out = append(out, sid)
	}
	return out, nil
}

func anyEquals(sids []*windows.SID, want *windows.SID) bool {
	for _, s := range sids {
		if s.Equals(want) {
			return true
		}
	}
	return false
}
