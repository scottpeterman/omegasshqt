# Per-session attributes

What a saved session can now say for itself, where those values come from when
it says nothing, and why the shape is what it is.

## The constraint that used to apply, and no longer does

Everything about the session store was built to a rule: the same
`~/.nterm/sessions.db`, opened by both Omega and nterm-qt, with neither taught
the other's serialiser. That rule is why serial and telnet were quick-connect
only — a saved session had nowhere to record a transport, and adding a column
would have meant a schema the other application did not know.

nterm-qt is retired. The schema is Omega's, and the columns below are what that
buys.

The rule was never wrong; it was a scoring harness for a port, and it did its
job. What is kept from it: the base DDL is still byte-identical to
`ntermqt/manager/models.py`, so a database created fresh is created the way the
Python created one, and `extras` is still carried as opaque column text. The
attribute columns are added by `migrate()` on open rather than written into
`kSchema`, so a real nterm-qt database gains them on first open with every row
intact.

## Three layers, one function

A value comes from the SESSION if it has one, else from the APPLICATION
SETTINGS, else from the struct's own default. That resolution lives in
`app/effectiveconfig.h` and nowhere else.

Before it, `openTab()` applied term type, paste threshold, scrollback, font
size and anti-idle to the tab one call at a time, and `openSession()` built the
transport config separately. Adding overrides that way would have meant two
places to remember for every new field, and the failure is silent: a field
wired into one path and missed in the other looks correct until it is reached
from the other menu. Quick connect resolves through the same function — its
dialog chose every transport field explicitly, so it inherits nothing there,
but the tab settings are the same globals resolved the same way.

## Absent is not a value

Every override column is nullable, and NULL means *inherit*, which is a
different thing from any value the field could hold — including `false`.

A bool stored as 0-or-1 with a default cannot express the difference. The
consequence is not theoretical: a session saved once would pin whatever the
global happened to be that day and quietly stop following it, and nothing about
the session would look wrong afterwards. `std::optional` in the struct, NULL in
the column, and three widget idioms in the editor that each keep absence
distinct from every value:

| | |
|---|---|
| text | empty line edit, inherited value as placeholder |
| number | spin box one below its minimum, with a special value text |
| flag | three-item combo — inherit, on, off |

A tristate `QCheckBox` was the obvious answer for the last one and is the wrong
one: partially-checked reads as "some of the things below are on", not "this
follows the global".

`tests/compat/session_editor_probe.cpp` asserts the whole of this from the
other end: a session that overrides nothing, opened in the form and saved
again, comes back with every column still NULL.

## What a session can override

**Transport** — `ssh`, `telnet` or `serial`, stored as text so the column reads
as itself in a `sqlite3` shell. Defaulted to `ssh`, because every row that
predates it was one. This is the column that stopped telnet and serial being
saved session types.

**Terminal** — term type, scrollback, multi-line paste threshold, paste rate.
Paste rate is in baud, and `0` is a real value meaning unpaced, which is the
clearest case for why absence has to be its own state.

**SSH** — host key policy (`strict` / `tofu` / `insecure`), legacy algorithms,
and a username that overrides the one on the credential. Host key policy per
session is the point rather than a convenience: a lab bench and a production
edge do not want the same answer, and reimaged gear is a real reason to turn
verification off in one place without turning it off everywhere.

**Jump host** — host, port, username, credential. nterm-qt had no concept of a
bastion at all, so this is new ground rather than parity. The jump credential
resolves independently of the session's own, which is how a key for the hop and
a password for the target arrive in one dial.

**Anti-idle** — enabled, interval, keystroke, custom bytes, split across four
columns rather than a JSON blob so a query can find every session with it
switched on. It resolves FIELD BY FIELD: a session that only switches it on
inherits the global's interval and keystroke. Copying the whole struct on any
one override would silently reset the other three.

**Serial** — port, baud, data bits, parity, stop bits.

**Telnet** — CR to CR LF.

## What the editor drops, on purpose

Credentials, host key policy and a jump host are SSH's alone: telnet has no
authentication step — a login prompt on it is ordinary session data arriving
after the socket is up, and the library refuses credentials outright — and
serial has no network identity. Saving a session as either therefore clears
those columns rather than leaving values behind that nothing will read.

This is destructive, and it is visible: the rows grey out the moment the
transport row changes.

## Things deliberately not done

**Nothing went into `extras`.** The blob is still opaque and still carries the
TerminalTelemetry vendor, device type and model keys. An earlier design put the
attributes in there as a namespaced sub-object with tri-state semantics, which
existed only to smuggle fields past a schema that could not be changed. Once it
could, columns were simpler in every direction — a query can find them, a
`sqlite3` shell can read them, and NULL already means what the tri-state was
emulating.

**`sessions/sessionio.cpp` now merges `extras` instead of replacing it.** The
TerminalTelemetry import rebuilt the blob from the three keys the format
defines, so a Ctrl+I would have dropped anything else a session carried.

**Font size has no per-session column.** It is in the settings file and reads
like a candidate, but a per-tab font is a display preference rather than a
property of the device, and nothing has wanted it.

## Still open

Quick connect has no "Save as session…" button, so the only route to a serial
or telnet session is New Session and switching the transport row. The dialog
already produces a full config; what it lacks is somewhere to put it.
