// tests/win/minarchive/min2.go
//
// The same archive with omegassh's dependency graph pulled in, and nothing
// else changed. Blank imports, so no code from them is called -- only their
// package init() functions run. That is the whole point: if min.go runs and
// this one hangs, the block is in an init(), and removing imports from this
// list one at a time names which.
//
// Built as a separate archive by min.bat, not alongside min.go.
package main

import (
	"C"

	_ "github.com/scottpeterman/omegassh/serialx"
	_ "github.com/scottpeterman/omegassh/sshcore"
	_ "github.com/scottpeterman/omegassh/telnetx"
	_ "github.com/scottpeterman/omegassh/transport"
	_ "github.com/scottpeterman/omegassh/vault"
	_ "github.com/scottpeterman/omegassh/vault/keyring"
	_ "github.com/scottpeterman/omegassh/vault/sshsecrets"
)

//export min_answer
func min_answer() C.int {
	return 42
}

func main() {}
