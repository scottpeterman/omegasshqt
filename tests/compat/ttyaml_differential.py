#!/usr/bin/env python3
"""tests/compat/ttyaml_differential.py

Asks two questions about the TerminalTelemetry YAML support, both by running
the other implementation rather than by asserting what it would do.

  1. Does sessions/ttyaml.cpp read a file the way PyYAML reads it? The C++ is a
     restricted subset reader, so it is allowed to REFUSE something PyYAML
     accepts -- that is the design. It is not allowed to accept something and
     read it differently. Every sample where both succeed must agree field for
     field.

  2. Does sessions/sessionio.cpp import a document the way
     ntermqt/manager/io.py's import_terminal_telemetry does? Same YAML, two
     fresh databases, and the resulting rows compared.

Usage:

    ttyaml_differential.py --probe build/sessions/ttyaml_probe \\
                           --ntermqt /path/to/ntermqt-checkout

The ntermqt path is the directory CONTAINING the ntermqt package. Only
manager/models.py and manager/io.py are loaded, by file path, so PySide6 is not
required -- io.py imports it at module scope, so the Qt names it pulls in are
stubbed before the module is executed.
"""

from __future__ import annotations

import argparse
import importlib.util
import pathlib
import subprocess
import sys
import tempfile
import types

import yaml


# --- samples ---------------------------------------------------------------
# Each is a file the two readers must agree about, or that the C++ may refuse.
# The interesting ones are the quoting and type-resolution edges: a port that
# YAML would make an int, a model that YAML would make a bool, a name with a
# colon or a hash in it.
SAMPLES: dict[str, str] = {
    "plain": """
- folder_name: Core
  sessions:
    - display_name: lab-core-1
      host: 10.10.0.1
      port: 22
      DeviceType: switch
      Model: C9300
      Vendor: cisco
""",
    "two_folders": """
- folder_name: Core
  sessions:
    - display_name: lab-core-1
      host: 10.10.0.1
- folder_name: Edge
  sessions:
    - display_name: lab-edge-1
      host: 10.10.1.1
      port: 2222
""",
    "quoted": """
- folder_name: 'Edge / WAN'
  sessions:
    - display_name: "lab-edge-1"
      host: '10.10.1.1'
      port: '22'
      Model: 'C9300-48'
""",
    "comments": """
# a comment
- folder_name: Lab  # trailing
  sessions:
    # another
    - host: 10.0.0.1
      Model: C9300#R
""",
    "boolish": """
- folder_name: Odd
  sessions:
    - host: 10.0.0.5
      Model: NO
      Vendor: 'yes'
      DeviceType: 'null'
""",
    "empty_folder": """
- folder_name: Nothing
  sessions: []
- folder_name: Something
  sessions:
    - host: 10.0.0.9
""",
    "missing_fields": """
- folder_name: Sparse
  sessions:
    - host: 10.0.0.7
    - display_name: no-host-here
    - host: 10.0.0.8
""",
    "bad_port": """
- folder_name: Sparse
  sessions:
    - host: 10.0.0.8
      port: notanumber
""",
    "unicode": """
- folder_name: Lab-Ost
  sessions:
    - display_name: lab-cœur-1
      host: 10.0.0.10
      Model: Ubiquiti-µ
""",
    "duplicate_hosts": """
- folder_name: A
  sessions:
    - display_name: first
      host: 10.0.0.20
- folder_name: B
  sessions:
    - display_name: second
      host: 10.0.0.20
""",
    "empty_value": """
- folder_name: Blanks
  sessions:
    - host: 10.0.0.30
      Model:
      Vendor: null
      DeviceType: ~
""",
}


# --- loading nterm-qt without PySide6 ---------------------------------------

def _stub_qt() -> None:
    """io.py imports PySide6 at module scope for its dialogs.

    The import-side functions this compares against touch none of it, so the Qt
    names are stubbed rather than installed: requiring a GUI toolkit to check a
    YAML reader would be a dependency on the wrong thing.
    """
    if "PySide6" in sys.modules:
        return

    class _Any:
        def __init__(self, *a, **k):
            pass

        def __getattr__(self, name):
            return _Any()

        def __call__(self, *a, **k):
            return _Any()

    for name in ("PySide6", "PySide6.QtWidgets", "PySide6.QtCore",
                 "PySide6.QtGui"):
        module = types.ModuleType(name)
        module.__getattr__ = lambda _n: _Any  # type: ignore[attr-defined]
        sys.modules[name] = module


def _load(name: str, path: pathlib.Path, package_stub: str | None = None):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    if package_stub:
        module.__package__ = package_stub
    spec.loader.exec_module(module)
    return module


def load_ntermqt(root: pathlib.Path):
    """Loads manager/models.py and manager/io.py by path.

    io.py uses `from .models import ...`, so models is registered under the
    package name io.py expects before io.py is executed.
    """
    _stub_qt()

    manager = types.ModuleType("ntmanager")
    manager.__path__ = [str(root / "ntermqt" / "manager")]
    sys.modules["ntmanager"] = manager

    models = _load("ntmanager.models", root / "ntermqt" / "manager" / "models.py",
                   package_stub="ntmanager")
    manager.models = models
    io_mod = _load("ntmanager.io", root / "ntermqt" / "manager" / "io.py",
                   package_stub="ntmanager")
    return models, io_mod


# --- canonical forms -------------------------------------------------------

def python_parse_dump(text: str) -> list[str]:
    """The same canonical lines the probe's `parse` mode prints."""
    data = yaml.safe_load(text)
    lines: list[str] = []
    for folder in data or []:
        lines.append(f"folder\t{_scalar(folder.get('folder_name', ''))}")
        for device in folder.get("sessions") or []:
            lines.append("device")
            for key, value in device.items():
                lines.append(f"field\t{key}\t{_scalar(value)}")
    return lines


def _scalar(value) -> str:
    """YAML types rendered the way the C++ reader renders them: as strings.

    The C++ has no type resolution -- every scalar is text -- so PyYAML's
    result is mapped onto that. None becomes empty, which is what the reader
    does with null and ~. Booleans are the interesting case: PyYAML turns an
    unquoted NO into False, and the C++ keeps "NO", so those samples are
    expected to disagree and are marked below rather than papered over.
    """
    if value is None:
        return ""
    if isinstance(value, bool):
        return "true" if value else "false"
    return str(value)


# Samples where PyYAML's type resolution and a text-only reader legitimately
# differ. Listed explicitly so the list is a decision rather than a silence.
TYPE_RESOLUTION_DIFFERS = {"boolish"}

# Samples where the two IMPORTERS differ on purpose, with the reason. These are
# still parsed and compared above; only the import comparison is skipped.
#
#   bad_port: import_terminal_telemetry does int(sess.get("port", 22)) with no
#   guard, so a port that is not a number raises ValueError out of the whole
#   call -- one malformed row abandons the import, after earlier rows have
#   already been committed, leaving a half-imported tree and no summary.
#   sessions/sessionio.cpp falls back to 22 for that row and carries on. This
#   is a deliberate divergence, not an oversight: refusing to match nterm-qt
#   here means refusing to reproduce a partial write with no report.
#
#   duplicate_hosts: two entries in ONE file sharing a host. The Python tracks
#   what it has added in existing_sessions, but stores the SavedSession it
#   built rather than one carrying the id add_session returned, so the tracked
#   row has id=None. The second entry then takes that None, and
#   update_session runs "WHERE id IS NULL", which matches nothing. The row is
#   silently discarded while the return value counts it as imported --
#   verified: import_terminal_telemetry returns (2, 2, 0) and leaves one row.
#   sessions/sessionio.cpp writes the new id back, so the last entry wins and
#   the count is true. Matching nterm-qt here would mean reproducing a report
#   that disagrees with the database.
IMPORT_DIFFERS = {
    "bad_port": "nterm-qt raises on a non-numeric port; Omega falls back to 22",
    "duplicate_hosts":
        "nterm-qt drops an in-file duplicate but counts it; Omega applies it",
}


def python_import_dump(models, io_mod, text: str, db: pathlib.Path,
                       merge: bool) -> list[str]:
    path = db.with_suffix(".yaml")
    path.write_text(text)
    store = models.SessionStore(db)
    folders, imported, skipped = io_mod.import_terminal_telemetry(
        store, path, merge=merge)

    lines = [
        f"folders_created\t{folders}",
        f"sessions_imported\t{imported}",
        f"sessions_skipped\t{skipped}",
    ]
    tree = store.get_tree()
    for folder in tree["folders"]:
        parent = folder.parent_id if folder.parent_id is not None else -1
        lines.append(f"folder\t{folder.name}\t{parent}")
    for session in sorted(tree["sessions"], key=lambda s: s.hostname):
        extras = session.extras or {}
        lines.append(
            "session\t{}\t{}\t{}\t{}\t{}\t{}\t{}".format(
                session.hostname, session.name, session.description,
                session.port, extras.get("vendor", ""),
                extras.get("device_type", ""), extras.get("model", "")))
    store.close()
    return lines


def probe(probe_path: str, *args: str) -> list[str]:
    out = subprocess.run([probe_path, *args], capture_output=True, text=True,
                         check=False)
    return out.stdout.splitlines()


# --- the comparison --------------------------------------------------------

def diff(name: str, kind: str, expected: list[str], actual: list[str]) -> bool:
    if expected == actual:
        print(f"  ok    {name} [{kind}]")
        return True
    print(f"  FAIL  {name} [{kind}]")
    for i in range(max(len(expected), len(actual))):
        e = expected[i] if i < len(expected) else "<missing>"
        a = actual[i] if i < len(actual) else "<missing>"
        if e != a:
            print(f"          python: {e!r}")
            print(f"          c++   : {a!r}")
    return False


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--probe", required=True)
    ap.add_argument("--ntermqt", required=True)
    args = ap.parse_args()

    models, io_mod = load_ntermqt(pathlib.Path(args.ntermqt))

    passed = failed = skipped = 0
    with tempfile.TemporaryDirectory() as tmp:
        tmpdir = pathlib.Path(tmp)

        print("parsing")
        for name, text in SAMPLES.items():
            yaml_path = tmpdir / f"{name}.yaml"
            yaml_path.write_text(text)
            actual = probe(args.probe, "parse", str(yaml_path))

            if actual and actual[0].startswith("ERROR"):
                # A refusal is allowed; silently accepting-and-differing is not.
                print(f"  skip  {name} [parse] — refused at line "
                      f"{actual[0].split(chr(9))[1]}")
                skipped += 1
                continue

            if name in TYPE_RESOLUTION_DIFFERS:
                print(f"  skip  {name} [parse] — PyYAML type resolution, "
                      "documented divergence")
                skipped += 1
                continue

            expected = python_parse_dump(text)
            if diff(name, "parse", expected, actual):
                passed += 1
            else:
                failed += 1

        print("\nimporting")
        for name, text in SAMPLES.items():
            if name in TYPE_RESOLUTION_DIFFERS:
                skipped += 1
                continue
            if name in IMPORT_DIFFERS:
                print(f"  skip  {name} [import] — {IMPORT_DIFFERS[name]}")
                skipped += 1
                continue
            for merge in (True, False):
                mode = "merge" if merge else "skip"
                py_db = tmpdir / f"{name}-{mode}-py.db"
                cc_db = tmpdir / f"{name}-{mode}-cc.db"
                yaml_path = tmpdir / f"{name}.yaml"
                yaml_path.write_text(text)

                expected = python_import_dump(models, io_mod, text, py_db, merge)
                actual = probe(args.probe, "import", str(cc_db),
                               str(yaml_path), mode)
                if actual and actual[0].startswith("ERROR"):
                    print(f"  skip  {name} [{mode}] — refused")
                    skipped += 1
                    continue
                if diff(name, mode, expected, actual):
                    passed += 1
                else:
                    failed += 1

    print(f"\n{passed} passed, {failed} failed, {skipped} skipped")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
