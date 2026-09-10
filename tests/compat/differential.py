#!/usr/bin/env python3
"""tests/compat/differential.py

Runs the C++ store and nterm-qt's Python store against copies of the same
database and compares the results.

Each case below corresponds to a compatibility claim in Phase 1 of the
roadmap. The claims are about agreeing with a specific implementation, so the
only honest test is to run that implementation.

Two shapes of comparison:

  reader   -- one database, both implementations read it, outputs must match.
              Catches disagreements about ordering and about what a row says.

  writer   -- two copies, each implementation performs the same mutation, then
              ONE dumper reads both. Catches disagreements about what a
              mutation does to the file, without the dumper being a variable.

    NTERMQT_SRC=/path/to/nterm-qt ./differential.py --probe build/compat_probe
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
ORACLE = HERE / "oracle.py"

PASS, FAIL = "pass", "FAIL"
results: list[tuple[str, str, str]] = []


def run(cmd: list[str]) -> str:
    p = subprocess.run(cmd, capture_output=True, text=True)
    if p.returncode != 0:
        raise RuntimeError(f"{' '.join(cmd)}\n{p.stderr.strip()}")
    return p.stdout


def oracle(*args: str) -> str:
    return run([sys.executable, str(ORACLE), *args])


def check(name: str, got, want, note: str = "") -> None:
    if got == want:
        results.append((PASS, name, note))
        return
    detail = note
    if isinstance(got, str) and isinstance(want, str):
        g, w = got.splitlines(), want.splitlines()
        for i in range(max(len(g), len(w))):
            a = g[i] if i < len(g) else "<missing>"
            b = w[i] if i < len(w) else "<missing>"
            if a != b:
                detail = f"line {i + 1}: cpp={a!r} py={b!r}"
                break
    results.append((FAIL, name, detail))


def fixture(tmp: Path, name: str) -> Path:
    """A fresh fixture database built by the Python store."""
    db = tmp / f"{name}.db"
    oracle("build-fixture", str(db))
    return db


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--probe", required=True, help="path to compat_probe")
    ap.add_argument("--against", metavar="DB",
                    help="read-only: compare both readers against an existing "
                         "database instead of running the fixture suite")
    args = ap.parse_args()
    probe = str(Path(args.probe).resolve())

    def cpp(*a: str) -> str:
        return run([probe, *a])

    if args.against:
        return compare_existing(args.against, cpp)

    src = os.environ.get("NTERMQT_SRC")
    if not src:
        return print("NTERMQT_SRC is not set") or 2

    # Record which checkout answered. Two package layouts exist in the wild
    # (nterm/ and ntermqt/), and a run that passes against one says nothing
    # about the other unless the output says which it was.
    resolved = next(
        (p for p in (Path(src) / "ntermqt" / "manager" / "models.py",
                     Path(src) / "nterm" / "manager" / "models.py")
         if p.exists()), None)
    if resolved is None:
        return print(f"no session store found under {src}") or 2
    print(f"oracle: {resolved}\n")

    with tempfile.TemporaryDirectory() as td:
        tmp = Path(td)

        # -- reader: the same rows, read two ways --------------------------
        db = fixture(tmp, "read")
        check("reader agrees on raw rows",
              cpp("dump-raw", str(db)), oracle("dump-raw", str(db)))

        # -- reader: ordering, including the position tie ------------------
        # The fixture deliberately gives two sibling sessions and two sibling
        # folders position 0, so ORDER BY position, name has to break the tie
        # on name. Sorting by position alone passes every other case.
        check("reader agrees on tree order",
              cpp("dump-ordered", str(db)), oracle("dump-ordered", str(db)),
              "includes tied positions")

        # -- writer: delete_folder, the foreign-key trap -------------------
        # 'edge' (id 2) has a subfolder 'deep' (id 4) and two sessions.
        #
        # The hazard is writing the delete the way the DDL reads. With foreign
        # keys ON, a bare DELETE cascades 'deep' out of existence; with them
        # OFF -- how nterm-qt actually runs -- the same bare DELETE leaves
        # 'deep' pointing at a folder that no longer exists. The Python does
        # neither: it reparents to root. Both mutants are caught here.
        #
        # Note that the pragma alone is not the bug. Enabling foreign keys
        # while still reparenting by hand passes this test, because the manual
        # UPDATE has already cleared the reference before the DELETE runs. It
        # is the combination that loses data, which is why the guard is a
        # behavioural comparison rather than a check for the pragma.
        a, b = tmp / "del_py.db", tmp / "del_cpp.db"
        shutil.copy(fixture(tmp, "del"), a)
        shutil.copy(a, b)
        oracle("delete-folder", str(a), "2")
        cpp("delete-folder", str(b), "2")
        check("delete-folder leaves identical rows",
              oracle("dump-raw", str(b)), oracle("dump-raw", str(a)),
              "subfolder reparented, not cascaded")

        # -- writer: position assignment on insert -------------------------
        a, b = tmp / "pos_py.db", tmp / "pos_cpp.db"
        shutil.copy(fixture(tmp, "pos"), a)
        shutil.copy(a, b)
        for i in range(3):
            oracle("add-session", str(a), f"new{i}", f"new{i}.lab.example",
                   "2", "{}")
            cpp("add-session", str(b), f"new{i}", f"new{i}.lab.example",
                "2", "{}")
        check("insert positions agree",
              oracle("dump-raw", str(b)), oracle("dump-raw", str(a)),
              "MAX(position) + 1 per folder")

        # -- writer: root inserts and the IS NULL compare ------------------
        # WHERE folder_id = NULL matches nothing, so a C++ store using `=`
        # would restart root positions at 0 every time. Only shows up at root.
        a, b = tmp / "root_py.db", tmp / "root_cpp.db"
        shutil.copy(fixture(tmp, "root"), a)
        shutil.copy(a, b)
        for i in range(3):
            oracle("add-session", str(a), f"r{i}", f"r{i}.lab.example", "-", "{}")
            cpp("add-session", str(b), f"r{i}", f"r{i}.lab.example", "-", "{}")
        check("root insert positions agree",
              oracle("dump-raw", str(b)), oracle("dump-raw", str(a)),
              "WHERE folder_id IS ?, not = ?")

        # -- extras: survives a Python rewrite -----------------------------
        # The C++ writes a blob with keys nothing knows, the Python opens and
        # saves every session (re-serialising through json.dumps), and the C++
        # must still find every key. Byte equality is not the bar and cannot
        # be -- Python's separators and key order are its own.
        db = fixture(tmp, "extras")
        weird = {"z_last": 1, "a_first": [1, 2, {"deep": True}],
                 "unicode": "æøå — ✓", "empty": {}, "null": None,
                 "num": 1.5}
        sid = cpp("add-session", str(db), "extras-probe", "probe.lab.example",
                  "-", json.dumps(weird)).strip()
        before = json.loads(_extras_of(str(db), sid))
        oracle("resave-all", str(db))
        after = json.loads(_extras_of(str(db), sid))
        check("extras keys survive a Python rewrite", after, before,
              "semantic equality, not bytes")

        # And the C++ round trip on its own must be lossless byte for byte,
        # since it never parses the blob.
        db = fixture(tmp, "extras2")
        sid = cpp("add-session", str(db), "opaque", "probe.lab.example", "-",
                  json.dumps(weird)).strip()
        raw_before = _extras_of(str(db), sid)
        cpp("resave-all", str(db))
        check("extras bytes survive a C++ rewrite",
              _extras_of(str(db), sid), raw_before, "opaque, never parsed")

        # -- timestamps: same format from both -----------------------------
        db = fixture(tmp, "ts")
        oracle("record-connect", str(db), "1")
        cpp("record-connect", str(db), "2")
        rows = {r.split("\t")[0]: r.split("\t")
                for r in oracle("timestamps", str(db)).strip().splitlines()}
        py_ts, cpp_ts = rows["1"][2], rows["2"][2]
        check("timestamp formats agree",
              _shape(cpp_ts), _shape(py_ts),
              f"py={py_ts!r} cpp={cpp_ts!r}")

        # -- move-folder cycle rejection -----------------------------------
        a, b = tmp / "cyc_py.db", tmp / "cyc_cpp.db"
        shutil.copy(fixture(tmp, "cyc"), a)
        shutil.copy(a, b)
        # folder 1 is 'lab', folder 2 is 'edge' beneath it: moving lab under
        # edge would detach the subtree from the root.
        py_out = oracle("move-folder", str(a), "1", "2").strip()
        cpp_out = cpp("move-folder", str(b), "1", "2").strip()
        check("cycle rejected by both",
              cpp_out.startswith("rejected"), py_out.startswith("rejected"),
              f"py={py_out!r} cpp={cpp_out!r}")
        check("rejected move left the file alone",
              oracle("dump-raw", str(b)), oracle("dump-raw", str(a)))

        # -- creation order: whoever gets there first writes the schema ----
        #
        # A new Omega user on a clean machine has no ~/.nterm/sessions.db, so
        # the C++ creates it and nterm-qt adopts it. Its _ensure_db then runs
        # CREATE TABLE IF NOT EXISTS over a file it did not make -- meaning if
        # the two DDLs ever drift, nothing errors: the second application just
        # keeps using whatever the first one wrote, and the difference shows
        # up later as a missing default or a column that is not there.
        #
        # Both orders are tested because the failure is asymmetric: a column
        # the C++ omits is invisible until the Python tries to write it.
        cpp_first, py_first = tmp / "order_cpp.db", tmp / "order_py.db"
        cpp("create", str(cpp_first))
        oracle("create", str(py_first))
        check("the two DDLs match",
              _ddl(cpp("schema", str(cpp_first))),
              _ddl(oracle("schema", str(py_first))),
              "whitespace normalised, everything else exact")

        # C++ creates, Python then writes and reads through its own store.
        oracle("add-session", str(cpp_first), "adopted", "adopted.lab.example",
               "-", '{"written_by": "python"}')
        oracle("delete-folder", str(cpp_first), "1")  # no such folder: a no-op
        check("Python can write a store the C++ created",
              cpp("dump-raw", str(cpp_first)), oracle("dump-raw", str(cpp_first)))

        # And the reverse.
        cpp("add-session", str(py_first), "adopted", "adopted.lab.example",
            "-", '{"written_by": "cpp"}')
        check("the C++ can write a store Python created",
              cpp("dump-raw", str(py_first)), oracle("dump-raw", str(py_first)))

        # The schemas must still match after both have been written to.
        check("DDL unchanged after both have written",
              _ddl(cpp("schema", str(cpp_first))),
              _ddl(oracle("schema", str(py_first))))

        # -- move-session between folders ----------------------------------
        a, b = tmp / "mv_py.db", tmp / "mv_cpp.db"
        shutil.copy(fixture(tmp, "mv"), a)
        shutil.copy(a, b)
        oracle("move-session", str(a), "1", "3")
        cpp("move-session", str(b), "1", "3")
        check("move-session agrees",
              oracle("dump-raw", str(b)), oracle("dump-raw", str(a)))

    width = max(len(n) for _, n, _ in results)
    for status, name, note in results:
        mark = "  ok  " if status == PASS else " FAIL "
        line = f"{mark} {name.ljust(width)}"
        if note:
            line += f"   {note}"
        print(line)

    failed = sum(1 for s, _, _ in results if s == FAIL)
    print(f"\n{len(results) - failed}/{len(results)} passed")
    return 1 if failed else 0


def compare_existing(db: str, cpp) -> int:
    """Point both readers at a database that already exists and compare.

    Mutates nothing. This is the case fixtures cannot cover: a file a real
    application has been writing to, with descriptions and extras blobs
    nobody designed for a test.

    Written as one command on purpose. The shell version of this -- two
    redirects and a diff -- reports success when one side dies and writes an
    empty file, which is exactly what it did the first time it was run.
    """
    path = Path(db)
    if not path.exists():
        print(f"no such database: {db}")
        return 2

    print(f"comparing readers against {path}\n")

    # A file with no tables is a real case -- an empty file, or one that is
    # not a session store at all -- and both readers fail on it identically.
    # Reported rather than raised: the answer is "wrong file", not a crash.
    try:
        cpp_raw = cpp("dump-raw", str(path))
        py_raw = oracle("dump-raw", str(path))
    except RuntimeError as e:
        if "no such table" in str(e):
            print("  this file has no folders/sessions tables -- not a session"
                  " store, or an empty file.")
            return 2
        raise

    if not cpp_raw.strip() and not py_raw.strip():
        print("  both readers returned nothing -- the database has no rows.")
        print("  Nothing was compared. Point --against at a populated file.")
        return 2

    check("raw rows agree", cpp_raw, py_raw,
          f"{len(py_raw.splitlines())} rows")

    # Field counts, per line. A tab lost on either side would otherwise show
    # up only as an opaque mismatch.
    widths_py = {len(l.split("\t")) for l in py_raw.splitlines()}
    widths_cpp = {len(l.split("\t")) for l in cpp_raw.splitlines()}
    check("field counts agree", widths_cpp, widths_py,
          f"py={sorted(widths_py)} cpp={sorted(widths_cpp)}")

    # Ordering needs nterm-qt's store, so it is skipped rather than fatal --
    # the raw comparison above is the part that needs no checkout.
    if os.environ.get("NTERMQT_SRC"):
        check("tree order agrees",
              cpp("dump-ordered", str(path)), oracle("dump-ordered", str(path)))
    else:
        print("  note: tree order not compared -- NTERMQT_SRC is not set\n")

    width = max(len(n) for _, n, _ in results)
    for status, name, note in results:
        mark = "  ok  " if status == PASS else " FAIL "
        line = f"{mark} {name.ljust(width)}"
        if note:
            line += f"   {note}"
        print(line)

    failed = sum(1 for s, _, _ in results if s == FAIL)
    print(f"\n{len(results) - failed}/{len(results)} passed")
    return 1 if failed else 0


def _extras_of(db: str, session_id: str) -> str:
    import sqlite3
    conn = sqlite3.connect(db)
    row = conn.execute("SELECT extras FROM sessions WHERE id = ?",
                       (int(session_id),)).fetchone()
    conn.close()
    return row[0]


def _ddl(schema: str) -> str:
    """Collapse whitespace runs in recorded DDL, and nothing else.

    SQLite stores the CREATE statement verbatim, so the two applications'
    source formatting ends up in sqlite_master: nterm-qt's DDL sits in a
    heredoc indented sixteen spaces, the C++ one in a raw string indented
    four. The tables are identical; only the transcript differs.

    Normalising just whitespace keeps this a real assertion. Column names,
    types, defaults, NOT NULL, the REFERENCES clauses and their order all
    still have to match exactly -- which is the thing that matters, because
    CREATE TABLE IF NOT EXISTS means the second application to open a file
    silently accepts whatever the first one wrote.
    """
    return "\n".join(" ".join(line.split()) for line in schema.splitlines())


def _shape(ts: str) -> str:
    """Reduce a timestamp to its shape, so two different instants compare."""
    return "".join("9" if c.isdigit() else c for c in ts)


if __name__ == "__main__":
    sys.exit(main())
