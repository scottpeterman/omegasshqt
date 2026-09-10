// tests/win/minarchive/min.go
//
// The smallest possible c-archive: one exported function, no imports beyond
// cgo itself, no package init anywhere in the graph.
//
// This exists to split one question into two. vault_smoke prints "handle" and
// hangs on the first call into the archive, and a cgo call blocks until the Go
// runtime AND every package init() in the binary have finished. So the hang is
// one of:
//
//	A. the Go runtime does not come up when it is a library in an MSVC
//	   process rather than the owner of main()
//	B. the runtime is fine and something in omegassh's init graph blocks
//	C. the archive is damaged -- scripts/pdatafix reorders .pdata, and the
//	   real build is the first thing to EXECUTE from a patched archive
//
// This module has nothing that could block in init and is small enough that
// its .pdata is normally already sorted, so pdatafix reports "nothing to do"
// and C is off the table too. What is left is A alone.
//
//	RUNS  -> A and C are out. The runtime starts fine as a library, so the
//	         problem is in omegassh's own init graph, and min2.go bisects it.
//	HANGS -> A. Nothing about omegassh or pdatafix is involved; Go's runtime
//	         is not starting in a foreign process on this machine.
package main

import "C"

//export min_answer
func min_answer() C.int {
	return 42
}

func main() {}
