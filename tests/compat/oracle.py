#!/usr/bin/env python3
"""tests/compat/oracle.py

Ground truth for the session store, produced by nterm-qt's own code.

The compatibility claims in Phase 1 of the roadmap -- ordering, delete
semantics, position assignment, extras preservation, timestamp format -- are
all claims about agreeing with a specific Python implementation. This drives
that implementation directly so the claims can be tested instead of asserted.

ntermqt/manager/models.py imports sqlite3, json, dataclasses, pathlib and
datetime, and nothing else. No Qt, no package __init__ side effects, so it can
be loaded straight from a source checkout by path.

Every subcommand here has a twin in tests/compat/compat_probe.cpp taking the
same arguments and printing the same format. The differential driver runs one
of each against copies of the same database and compares.

    NTERMQT_SRC=/path/to/nterm-qt ./oracle.py <command> [args]
"""

from __future__ import annotations

import argparse
import importlib.util
import json
import os
import sqlite3
import sys
from pathlib import Path


def load_session_store():
    """Import SessionStore from an nterm-qt checkout without installing it."""
    src = os.environ.get("NTERMQT_SRC")
    if not src:
        sys.exit("NTERMQT_SRC is not set (path to an nterm-qt checkout)")

    # The package has been called both nterm and ntermqt -- it was flattened
    # and renamed, and checkouts of either vintage are valid oracles. Which
    # one answered is printed, because a schema difference between vintages
    # would otherwise be invisible in a passing run.
    candidates = [
        Path(src) / "ntermqt" / "manager" / "models.py",
        Path(src) / "nterm" / "manager" / "models.py",
    ]
    models = next((c for c in candidates if c.exists()), None)
    if models is None:
        tried = "\n  ".join(str(c) for c in candidates)
        sys.exit(f"no session store found under {src}. Tried:\n  {tried}")
    if os.environ.get("ORACLE_VERBOSE"):
        print(f"oracle: {models}", file=sys.stderr)

    spec = importlib.util.spec_from_file_location("ntermqt_models", models)
    module = importlib.util.module_from_spec(spec)
    # Registered before exec: @dataclass resolves string annotations by
    # looking its own class's __module__ up in sys.modules, and models.py has
    # `from __future__ import annotations`, so every annotation is a string.
    # Without this the decorator fails on the first dataclass in the file.
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


# --- output formats -------------------------------------------------------
#
# Tab-separated, NULL as \N, one record per line. Deliberately not JSON: both
# sides have to emit it byte-identically, and a hand-rolled tab format has no
# room for the two implementations to disagree about float formatting or key
# order. The C++ twin prints the same lines.

NULL = r"\N"


def _opt(v) -> str:
    return NULL if v is None else str(v)


def dump_raw(db_path: str) -> None:
    """Every row, by id, straight from SQL -- no store involved.

    This is the state comparison used after a mutation: it reads what is
    actually on disk rather than what either implementation believes it wrote.
    """
    conn = sqlite3.connect(db_path)
    conn.row_factory = sqlite3.Row

    for r in conn.execute(
        "SELECT id, name, parent_id, position, expanded FROM folders ORDER BY id"
    ):
        print("\t".join(["folder", str(r["id"]), r["name"],
                         _opt(r["parent_id"]), str(r["position"]),
                         str(r["expanded"])]))

    for r in conn.execute(
        "SELECT id, name, description, hostname, port, credential_name, "
        "folder_id, position, connect_count, extras FROM sessions ORDER BY id"
    ):
        print("\t".join(["session", str(r["id"]), r["name"], r["description"],
                         r["hostname"], str(r["port"]),
                         _opt(r["credential_name"]), _opt(r["folder_id"]),
                         str(r["position"]), str(r["connect_count"]),
                         r["extras"]]))
    conn.close()


def dump_ordered(db_path: str, store_mod) -> None:
    """The tree as the store hands it out, depth first.

    Where dump_raw compares state, this compares *ordering* -- the thing
    ORDER BY position, name decides and the thing a tree view renders.
    """
    store = store_mod.SessionStore(Path(db_path))

    def walk(parent_id, depth):
        for f in store.list_folders(parent_id):
            print("\t".join(["folder", str(depth), str(f.id), f.name,
                             str(f.position)]))
            walk(f.id, depth + 1)
        for s in store.list_sessions(parent_id):
            print("\t".join(["session", str(depth), str(s.id), s.name,
                             str(s.position)]))

    walk(None, 0)
    store.close()


# --- mutations, through the real store ------------------------------------

def cmd_build_fixture(args, store_mod) -> None:
    """A deterministic tree with the awkward cases built in.

    Tied positions, a folder nested three deep, sessions at root, a session
    with no credential, and extras carrying keys no application knows.
    """
    store = store_mod.SessionStore(Path(args.db))
    S = store_mod.SavedSession
    F = store_mod.SessionFolder

    lab = store.add_folder("lab")
    edge = store.add_folder("edge", lab)
    core = store.add_folder("core", lab)
    deep = store.add_folder("deep", edge)

    store.add_session(S(name="rtr01", hostname="rtr01.lab.example",
                        credential_name="lab-admin", folder_id=edge))
    store.add_session(S(name="rtr02", hostname="rtr02.lab.example",
                        credential_name="lab-admin", folder_id=edge))
    store.add_session(S(name="sw01", hostname="sw01.lab.example",
                        port=2222, credential_name="lab-admin",
                        folder_id=core))
    store.add_session(S(name="probe", hostname="probe.lab.example",
                        folder_id=deep,
                        extras={"term": "vt100", "unknown_key": [1, 2, 3]}))
    store.add_session(S(name="jump", hostname="jump.lab.example",
                        description="bastion"))

    # Tie two sibling positions so ORDER BY position, name has to break it on
    # name. Nothing in the normal insert path produces this; a bulk position
    # rewrite after a drag does.
    sessions = {s.name: s for s in store.list_all_sessions()}
    for name in ("rtr01", "rtr02"):
        s = sessions[name]
        s.position = 0
        store.update_session(s)

    # Same for folders: edge and core both at position 0.
    folders = {f.name: f for f in store.get_tree()["folders"]}
    for name in ("edge", "core"):
        f = folders[name]
        f.position = 0
        store.update_folder(f)

    store.close()
    print(f"fixture: {args.db}", file=sys.stderr)


def cmd_create(args, store_mod) -> None:
    """Create the store and nothing else -- _ensure_db does the work."""
    store_mod.SessionStore(Path(args.db)).close()


def cmd_schema(args, _store_mod) -> None:
    """The DDL as SQLite recorded it, for comparing who created what."""
    conn = sqlite3.connect(args.db)
    for row in conn.execute(
        "SELECT type, name, sql FROM sqlite_master "
        "WHERE sql IS NOT NULL ORDER BY type, name"
    ):
        print("\t".join([row[0], row[1], row[2]]))
    conn.close()


def cmd_delete_folder(args, store_mod) -> None:
    store = store_mod.SessionStore(Path(args.db))
    store.delete_folder(args.folder_id)
    store.close()


def cmd_add_session(args, store_mod) -> None:
    store = store_mod.SessionStore(Path(args.db))
    S = store_mod.SavedSession
    folder = None if args.folder == "-" else int(args.folder)
    sid = store.add_session(S(name=args.name, hostname=args.hostname,
                              folder_id=folder,
                              extras=json.loads(args.extras)))
    store.close()
    print(sid)


def cmd_resave_all(args, store_mod) -> None:
    """Read every session and write it straight back, unchanged.

    The extras torture test. The Python re-serialises the blob on every write,
    so this is where a C++-authored extras value gets rewritten by Python's
    json.dumps -- key order, separators and all.
    """
    store = store_mod.SessionStore(Path(args.db))
    for s in store.list_all_sessions():
        store.update_session(s)
    store.close()


def cmd_record_connect(args, store_mod) -> None:
    store = store_mod.SessionStore(Path(args.db))
    store.record_connect(args.session_id)
    store.close()


def cmd_move_session(args, store_mod) -> None:
    store = store_mod.SessionStore(Path(args.db))
    folder = None if args.folder == "-" else int(args.folder)
    store.move_session(args.session_id, folder)
    store.close()


def cmd_move_folder(args, store_mod) -> None:
    store = store_mod.SessionStore(Path(args.db))
    parent = None if args.parent == "-" else int(args.parent)
    try:
        store.move_folder(args.folder_id, parent)
        print("ok")
    except ValueError as e:
        print(f"rejected: {e}")
    store.close()


def cmd_timestamps(args, _store_mod) -> None:
    """created_at / last_connected as stored, for format comparison."""
    conn = sqlite3.connect(args.db)
    for row in conn.execute(
        "SELECT id, created_at, last_connected FROM sessions ORDER BY id"
    ):
        print("\t".join([str(row[0]), _opt(row[1]), _opt(row[2])]))
    conn.close()


def main() -> None:
    p = argparse.ArgumentParser(description=__doc__)
    sub = p.add_subparsers(dest="cmd", required=True)

    def add(name, fn, *args):
        sp = sub.add_parser(name)
        sp.add_argument("db")
        for a, kw in args:
            sp.add_argument(a, **kw)
        sp.set_defaults(fn=fn)
        return sp

    add("build-fixture", cmd_build_fixture)
    add("create", cmd_create)
    add("schema", cmd_schema)
    add("dump-raw", lambda a, m: dump_raw(a.db))
    add("dump-ordered", lambda a, m: dump_ordered(a.db, m))
    add("delete-folder", cmd_delete_folder, ("folder_id", {"type": int}))
    add("add-session", cmd_add_session,
        ("name", {}), ("hostname", {}), ("folder", {}), ("extras", {}))
    add("resave-all", cmd_resave_all)
    add("record-connect", cmd_record_connect, ("session_id", {"type": int}))
    add("move-session", cmd_move_session,
        ("session_id", {"type": int}), ("folder", {}))
    add("move-folder", cmd_move_folder,
        ("folder_id", {"type": int}), ("parent", {}))
    add("timestamps", cmd_timestamps)

    args = p.parse_args()

    # dump-raw and timestamps read the database with plain sqlite3 and never
    # touch nterm-qt's code, so they must not require a checkout to point at.
    # They are the two commands most likely to be run alone -- against a real
    # database, to see whether the two implementations read it the same way --
    # and demanding NTERMQT_SRC for them turns that into an empty file and a
    # diff that silently compares nothing.
    needs_oracle = args.cmd not in ("dump-raw", "timestamps", "schema")
    args.fn(args, load_session_store() if needs_oracle else None)


if __name__ == "__main__":
    main()
