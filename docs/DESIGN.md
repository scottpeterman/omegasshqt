# omegassh — design

## Scope

omegassh does one thing: it turns a network device into a byte stream, and
takes bytes back. It is the transport half of a terminal. anytermqt is the
other half — it turns bytes into pixels.

Neither knows the other exists. That is deliberate: it is what let anytermqt
ship, and the same discipline applies here.

**In scope:** SSH transport, authentication, host-key policy, algorithm
negotiation for old gear, jump hosts, pty allocation, window resizing.

**Out of scope:** terminal emulation, session management, credential storage,
discovery, telnet, serial. Those belong to whatever app is using this.

## Layering

```
  app (nterm, Pathfinder, anything else)
      |
  qt/          OmegaSshSession  -- QObject, QSocketNotifier, signals
      |
  include/     omegassh.h       -- stable C ABI
      |
  capi/        cgo shim         -- marshalling and handle bookkeeping only
      |
  sshcore/     the actual work  -- plain Go, importable directly
```

The rule for the layers: **anything that could have a protocol bug in it
belongs in `sshcore`.** The cgo shim converts JSON to a struct, hands out
integer handles, and copies bytes. It has no logic worth testing, because
everything worth testing is one layer down where it is ordinary Go.

That rule is why `session.go` and `buffer.go` live in `sshcore` rather than
in `capi`. A Go caller — Pathfinder, most obviously — gets the same pty
handling, resize behavior and exit detection as the C++ one, from the same
code, without going near cgo.

## `sshcore` is exported, not `internal`

This is the point at which Pathfinder stops owning its own SSH backend and
imports `github.com/scottpeterman/omegassh/sshcore` instead.

One place to fix the known_hosts bug. One place to add an algorithm when some
2009 switch needs it. The alternative — two copies drifting apart, one of
which is the one you happen to be debugging — is how this kind of thing
usually goes wrong.

## No Go→C callbacks

The obvious design is to register a C function pointer and call it from Go
when bytes arrive. It is also wrong for a GUI.

That callback fires on a Go-created OS thread. In Qt, touching a QObject from
a thread it does not belong to is undefined behavior, so every delivery would
need `QMetaObject::invokeMethod` with a queued connection. Forgetting once
gives you a crash that reproduces one time in four hundred, on someone else's
machine.

Instead each session owns a **notifier**: a readable handle that becomes
ready when output is buffered or the shell has exited. The caller watches it
with `QSocketNotifier`, drains it, and calls `omegassh_read` until empty.
Every byte lands on the caller's thread, and the design makes the wrong thing
impossible rather than merely discouraged.

The notifier is a wakeup, not a channel. Treat readiness as "look again",
never as a byte count, and expect spurious wakeups.

### Platform primitives

| Platform | Primitive | Watched with |
|---|---|---|
| Linux, macOS | `pipe2` pair, both ends non-blocking | `QSocketNotifier` on the fd |
| Windows | loopback TCP socket pair | `QSocketNotifier` on the SOCKET |

**`os.Pipe` will not do.** The Go netpoller adopts an `os.File` read end, and
`File.Fd()` then hands back a descriptor forced into blocking mode. A C++
drain loop deadlocks on it the first time the pipe is empty. `capi` uses raw
descriptors from `syscall.Pipe2`, which the netpoller never sees.

The same reasoning drives the Windows implementation: the sockets are made
with raw winsock calls and never wrapped in a `net.Conn`, because a `net.Conn`
is netpoller-owned and handing its SOCKET to C++ invites the same class of
intermittent failure. `accept`, `ioctlsocket` and `send` come from `ws2_32`
directly, since `x/sys/windows` stubs them out with `EWINDOWS`.

## The Qt surface mirrors `qtpyte::PtySession` name for name

`OmegaSshSession` exposes `dataReceived`, `write`, `resize`, `terminate`,
`finished(int)`, `running()` and `error()` — the same names anytermqt's
`PtySession` uses, not the names Qt convention would suggest. Swapping a local
shell for an SSH session is a type change and nothing else, and
`omegassh::attach` is `qtpyte::attach` with the transport swapped.

The naming trap worth recording, since the first draft fell into it: in
anytermqt, **`dataReady` is the widget's outbound signal** — keystrokes headed
for the transport. The transport's inbound signal is `dataReceived`. Calling
this class's output `dataReady` read perfectly well in isolation and wired up
exactly backwards next to the widget.

`omegassh::attach` lives in `integration/`, not in the `omegassh_qt` library.
That library links QtCore alone; pulling in a widget would cost every headless
consumer a dependency on QtWidgets.

## Exit status

`finished(int)` reports the same normalization `PtySession` documents for a
local child: the program's own status, or 128 + signal number where a signal
ended it. SSH carries a signal *name* rather than a number (the numbers differ
between systems), so `exit.go` maps the RFC 4254 names back.

This produced the one genuine race in the design, worth writing down because
it is not obvious: **EOF on stdout is not the end of the session.** The exit
status arrives on the channel afterwards. The first implementation flipped
`alive` to 0 on EOF, so every clean exit reported -1 — the status simply had
not been read yet. `alive` now stays 1 until the status is reaped, and the
reap sends one final wake.

The reap also has to happen *after* the buffer drains, never alongside it:
x/crypto closes the session inside `Wait`, and anything still in the stdout
pipe is discarded. Reaping concurrently costs the user the last screenful of
whatever they just ran.

## Handles, not pointers

The C API deals in `long long` session handles resolved through a registry,
not in opaque pointers. cgo forbids passing a Go pointer to C and keeping it,
and a handle also gives free protection against use-after-close: a stale
handle resolves to nothing and every call returns an error instead of
dereferencing freed memory.

## Host-key policy is a first-class enum

Three values, chosen explicitly:

- **strict** — known_hosts only. An unknown host is refused. The default.
- **tofu** — accept and pin on first contact. A **mismatch** against an
  already-pinned key still fails, always.
- **insecure** — no verification.

`insecure` exists because lab gear gets rebuilt and re-keyed constantly, and
a client that cannot be told to stop caring is a client people work around —
usually by turning verification off everywhere, permanently. Making it an
explicit per-connection opt-in is safer than making it unavailable.

What is *not* negotiable: a key mismatch on a pinned host fails closed under
every policy except `insecure`. TOFU covers first contact and nothing else.

TOFU auto-accepts at the C layer, because a fingerprint prompt cannot cross
the C boundary without reintroducing the callback-on-a-foreign-thread problem
this design exists to avoid. A UI that wants to show a fingerprint dialog
should import `sshcore` directly and supply its own `HostKeyPrompt`.

## Algorithm policy is shared across dial paths

Both hops — bastion and target — go through `algorithmPolicy()` and
`hostKeyAlgos()`. Never inline lists.

This was learned the hard way. When the two dial paths carried different
host-key preference orders, the same server would offer an ed25519 key on one
path and an ecdsa key on the other, and known_hosts read the difference as a
**mismatch** for a box whose keys had never changed.

Legacy algorithms (`group1-sha1`, CBC ciphers, `hmac-sha1`/`md5`, `ssh-rsa`,
`ssh-dss`) are appended after the modern set rather than replacing it, and
only when asked for. A modern server negotiates normally with the legacy tail
present; an old one finds something it recognizes further down the list.

## known_hosts tolerates malformed lines

`x/crypto/ssh/knownhosts` parses the entire file when the callback is built
and fails on the first line it cannot read. The blast radius is the whole
file: one junk line and every connection fails, with an error naming a line
number rather than a host — including hosts you were not dialing. OpenSSH
skips what it cannot parse.

`knownhostsfilter.go` parses line by line, keeps what is valid, and reports
what it dropped. Two things it deliberately does not do:

- **It never drops a line silently.** A skipped entry means a host that was
  pinned no longer is, and the operator has to know that.
- **It never repairs the source file.** The staged copy is a temporary; the
  operator's known_hosts is theirs.

Tolerance for garbage, none for a mismatch.

## Open questions



**Build joint.** CMake currently drives `go build`, which makes the Go
toolchain a hard dependency for every C++ consumer. The alternative is
publishing prebuilt archives per platform on GitHub releases and having CMake
fetch them: more release machinery, far friendlier for anyone else.

## Not yet built

- Python bindings (Shiboken6, mirroring anytermqt's `bindings/`)
- Agent auth on Windows — the OpenSSH agent is a named pipe and needs
  `go-winio`, deliberately deferred to keep dependencies at `x/crypto`
- Exec channels (run a command, capture output, no pty) — Pathfinder's
  capture and crawl paths will want these
- Keepalive and reconnect policy
