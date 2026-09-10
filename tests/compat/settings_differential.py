#!/usr/bin/env python3
"""tests/compat/settings_differential.py

Runs the C++ settings layer and nterm-qt's own against the same config.json
content and compares. Same arrangement as the store and theme differentials:
neither side asserts against its own idea of the answer, and the comparison is a
third thing that reads both.

What it checks, per fixture:

  fields      every field the two parse out of the file, one per line
  roundtrip   the file each one writes for that record, BYTE for byte

The second is the strong one. nterm-qt saves with
json.dumps(asdict(settings), indent=2) -- two-space indent, dataclass order, no
sorting, no trailing newline -- so the two writers are directly comparable, and
a file that survives a trip through Omega unchanged is the best evidence
available that the two applications will not churn it past each other.

And three that are not per-fixture:

  defaults    the default record, EXCEPT theme_name, which Omega deliberately
              changes -- see below
  unknown     an unrecognised key must be dropped by both, since from_dict
              filters to known fields and would delete it on the next save
  recent      add_recent_profile's move-to-front and trim behaviour

THE ONE DELIBERATE DIVERGENCE is theme_name. nterm-qt defaults it to
"catppuccin_mocha", and no such theme exists in either theme set -- the default
names a theme that is never found and always falls back. Omega defaults to
enterprise_dark, which ships in both. The check is that the two differ in
exactly that field and no other.

Usage:

    python3 tests/compat/settings_differential.py \\
        --probe build/app/settings_probe \\
        --ntermqt ../ntermqt
"""

import argparse
import difflib
import json
import subprocess
import sys
import tempfile
from pathlib import Path

# Omega's default, and the field it is allowed to differ on.
OMEGA_DEFAULT_THEME = "enterprise_dark"


def import_ntermqt_config(root):
    """Make ntermqt.config importable without running ntermqt/__init__.py.

    That __init__ imports the terminal widget, which imports PySide6, so a
    plain import would drag Qt into a comparison whose Python half has none.
    """
    import importlib
    import types

    pkg_dir = root / "ntermqt"
    if not (pkg_dir / "config.py").exists():
        raise SystemExit(f"no ntermqt/config.py under {root}")

    sys.path.insert(0, str(root))
    stub = types.ModuleType("ntermqt")
    stub.__path__ = [str(pkg_dir)]
    sys.modules["ntermqt"] = stub
    return importlib.import_module("ntermqt.config")


class Result:
    def __init__(self):
        self.passed = 0
        self.failed = 0
        self.messages = []

    def check(self, name, ok, detail=""):
        if ok:
            self.passed += 1
        else:
            self.failed += 1
            self.messages.append(f"FAIL {name}\n{detail}")

    def report(self):
        for m in self.messages:
            print(m)
        print(f"\n{self.passed} passed, {self.failed} failed")
        return 0 if self.failed == 0 else 1


def diff(a, b, label_a="python", label_b="c++"):
    return "".join(
        difflib.unified_diff(
            a.splitlines(keepends=True),
            b.splitlines(keepends=True),
            fromfile=label_a,
            tofile=label_b,
            n=2,
        )
    )


def python_fields(settings):
    """The same lines settings_probe fields prints, in the same order."""
    d = settings.to_dict()
    order = [
        "theme_name", "font_size", "multiline_paste_threshold",
        "scrollback_lines", "default_term_type", "default_keepalive_interval",
        "auto_reconnect", "window_width", "window_height", "window_x",
        "window_y", "window_maximized", "tree_width", "recent_profiles",
        "max_recent",
    ]
    lines = []
    for key in order:
        v = d[key]
        if key == "recent_profiles":
            v = "|".join(v)
        lines.append(f"{key}={v}")
    return "\n".join(lines) + "\n"


def python_save(config_mod, settings, tmpdir, name):
    """Write via SettingsManager so the comparison uses its real save path."""
    path = Path(tmpdir) / name
    mgr = config_mod.SettingsManager(path)
    mgr._settings = settings
    mgr.save()
    return path.read_text()


def fixtures():
    """Config files worth disagreeing about, as (name, text) pairs."""
    full = {
        "theme_name": "enterprise_dark",
        "font_size": 13,
        "multiline_paste_threshold": 3,
        "scrollback_lines": 25000,
        "default_term_type": "vt100",
        "default_keepalive_interval": 60,
        "auto_reconnect": False,
        "window_width": 1440,
        "window_height": 900,
        "window_x": 120,
        "window_y": 64,
        "window_maximized": False,
        "tree_width": 310,
        "recent_profiles": ["lab-core-sw01", "lab-edge-sw02"],
        "max_recent": 5,
    }
    yield "full", json.dumps(full, indent=2)

    # A maximized window: geometry is whatever it was before, and x/y are set.
    maximized = dict(full, window_maximized=True)
    yield "maximized", json.dumps(maximized, indent=2)

    # Never placed: window_x and window_y are null rather than absent, which is
    # how nterm-qt writes a window that has not been moved yet.
    never_placed = dict(full, window_x=None, window_y=None)
    yield "null_position", json.dumps(never_placed, indent=2)

    # Empty recent list -- json.dumps writes [] with nothing inside it, and an
    # emitter that expands it to a multi-line array diverges on the first save.
    yield "empty_recents", json.dumps(dict(full, recent_profiles=[]), indent=2)

    # A partial file: everything not present falls back to the dataclass
    # default on both sides. Real config.json files from older versions look
    # like this.
    yield "partial", json.dumps({"theme_name": "gruvbox_dark", "font_size": 16},
                                indent=2)

    # Non-ASCII in a profile name. json.dumps escapes it as \\uXXXX by default,
    # so an emitter writing raw UTF-8 would produce a file that parses the same
    # and compares differently -- and would rewrite the file on every save.
    yield "unicode", json.dumps(
        dict(full, recent_profiles=["lab-café-01", "südwest-sw03"]), indent=2)

    # An empty object: every field defaults.
    yield "empty_object", "{}"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--probe", required=True, help="path to settings_probe")
    ap.add_argument("--ntermqt", required=True, help="nterm-qt checkout root")
    args = ap.parse_args()

    config_mod = import_ntermqt_config(Path(args.ntermqt).resolve())
    AppSettings = config_mod.AppSettings
    probe = Path(args.probe).resolve()

    def run(*a):
        p = subprocess.run([str(probe), *a], capture_output=True, text=True)
        if p.returncode != 0:
            raise RuntimeError(f"{a}: exit {p.returncode}: {p.stderr.strip()}")
        return p.stdout

    r = Result()

    with tempfile.TemporaryDirectory() as tmp:
        for name, text in fixtures():
            path = Path(tmp) / f"{name}.json"
            path.write_text(text)

            parsed = json.loads(text)
            settings = AppSettings.from_dict(parsed)

            # The one deliberate divergence, applied where it applies: when a
            # file does not name a theme, the two fall back to different
            # defaults on purpose -- catppuccin_mocha, which resolves to
            # nothing, against enterprise_dark, which ships in both. Aligning
            # the Python side here keeps the rest of the fixture comparable
            # rather than dropping it, and the "defaults differ only in
            # theme_name" check below is what actually guards the divergence.
            if "theme_name" not in parsed:
                settings.theme_name = OMEGA_DEFAULT_THEME

            r.check(f"{name}: fields",
                    python_fields(settings) == run("fields", str(path)),
                    diff(python_fields(settings), run("fields", str(path))))

            py_out = python_save(config_mod, settings, tmp, f"{name}.out.json")
            cpp_out = run("roundtrip", str(path))
            r.check(f"{name}: roundtrip bytes", py_out == cpp_out,
                    diff(py_out, cpp_out))

        # --- the defaults, and the one field they may differ on -------------
        py_defaults = AppSettings().to_dict()
        cpp_defaults = json.loads(run("defaults"))

        differing = {k for k in py_defaults
                     if py_defaults[k] != cpp_defaults.get(k)}
        r.check("defaults differ only in theme_name",
                differing == {"theme_name"},
                f"fields that differ: {sorted(differing) or 'none'}\n"
                f"  python: {py_defaults}\n  c++:    {cpp_defaults}")
        r.check("omega defaults to a theme that exists",
                cpp_defaults.get("theme_name") == OMEGA_DEFAULT_THEME,
                f"theme_name is {cpp_defaults.get('theme_name')!r}")

        # --- unknown keys are dropped by both -------------------------------
        odd = Path(tmp) / "unknown.json"
        odd.write_text(json.dumps(
            {"theme_name": "nord", "omega_only_setting": 42}, indent=2))
        cpp_out = json.loads(run("roundtrip", str(odd)))
        py_out = AppSettings.from_dict(json.loads(odd.read_text())).to_dict()
        r.check("unknown key dropped by both",
                "omega_only_setting" not in cpp_out
                and "omega_only_setting" not in py_out,
                f"c++ kept it: {'omega_only_setting' in cpp_out}, "
                f"python kept it: {'omega_only_setting' in py_out}")

        # --- add_recent_profile ---------------------------------------------
        #
        # Two cases, because one is not enough. With max_recent 3 the trim
        # happens to produce the same list whether or not an existing entry is
        # moved to the front, so that case alone passes against an
        # implementation that only prepends. The headroom case is the one that
        # separates them, and the trim case is still worth keeping for the cap.
        recent_cases = [
            ("headroom", {"recent_profiles": ["a", "b", "c"], "max_recent": 10},
             ["d", "b", "e"]),
            ("trim", {"recent_profiles": ["a", "b", "c"], "max_recent": 3},
             ["d", "b", "e"]),
        ]
        for label, content, pushes in recent_cases:
            base = Path(tmp) / f"recent_{label}.json"
            base.write_text(json.dumps(content, indent=2))

            py = AppSettings.from_dict(json.loads(base.read_text()))
            for name in pushes:
                py.add_recent_profile(name)
            cpp = json.loads(run("recent", str(base), *pushes))
            r.check(f"add_recent_profile: {label}",
                    py.recent_profiles == cpp["recent_profiles"],
                    f"python: {py.recent_profiles}\nc++:    {cpp['recent_profiles']}")

    return r.report()


if __name__ == "__main__":
    sys.exit(main())
