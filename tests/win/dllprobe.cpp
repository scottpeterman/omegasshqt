// tests/win/dllprobe.cpp
//
// Does Go's runtime start at all inside a process it does not own?
//
// vault_smoke prints "handle" and then hangs on omegassh_vault_open -- the
// first call into the archive. Two things could produce that, and they need
// separating before either can be fixed:
//
//   1. the archive is damaged. scripts/pdatafix reorders .pdata to satisfy
//      MSVC, and this is the first time anything has EXECUTED from a patched
//      archive. Linking clean proves the object parses, not that its unwind
//      tables are right.
//
//   2. the archive is fine and Go's runtime does not come up when it is
//      initialized as a library rather than owning main(). Nothing in
//      PathfinderSSH exercises that: there the Go runtime owns the process.
//
// This tells them apart. -buildmode=c-shared hands the whole link to Go's own
// linker, so no .pdata ever reaches link.exe and pdatafix never runs. The DLL
// is loaded at run time with LoadLibrary, so there is no import library to
// generate and nothing to configure.
//
//   RUNS   -> the Go side is fine as a library; suspicion falls on the
//             c-archive path, which means pdatafix or the MSVC link.
//   HANGS  -> the archive was never the problem. Go's runtime is not starting
//             in a foreign process, and pdatafix is exonerated.
//
// Build and run: tests\win\dllprobe.bat
//
// Deliberately not part of the CMake build. It is a bisection tool for one
// question, and a target that outlives the question it answers becomes a
// target nobody dares delete.

#include <windows.h>

#include <cstdio>

typedef long long (*vault_open_fn)(const char *);
typedef char *(*last_error_fn)(void);

int main(int argc, char **argv) {
    const char *dll = (argc > 1) ? argv[1] : "build\\omegassh.dll";
    const char *path = (argc > 2) ? argv[2] : "build\\dllprobe-vault.json";

    std::printf("loading %s\n", dll);
    std::fflush(stdout);

    HMODULE h = LoadLibraryA(dll);
    if (!h) {
        std::printf("LoadLibrary failed: %lu\n", GetLastError());
        return 1;
    }
    std::printf("loaded. Go's runtime starts during DLL init, so if this is\n");
    std::printf("the last line you see, it never came up.\n");
    std::fflush(stdout);

    auto vault_open = (vault_open_fn)GetProcAddress(h, "omegassh_vault_open");
    if (!vault_open) {
        std::printf("omegassh_vault_open not exported: %lu\n", GetLastError());
        return 1;
    }
    std::printf("resolved omegassh_vault_open, calling it now\n");
    std::fflush(stdout);

    DeleteFileA(path);
    long long v = vault_open(path);

    std::printf("returned %lld\n", v);
    if (v < 0) {
        auto last_error = (last_error_fn)GetProcAddress(h, "omegassh_last_error");
        if (last_error) {
            char *msg = last_error();
            std::printf("  error: %s\n", msg ? msg : "(none)");
        }
        // A negative return is a fine outcome here. The question is whether
        // the call RETURNED, not whether it succeeded.
    }
    std::printf("\nthe Go runtime came up and a call completed\n");
    return 0;
}