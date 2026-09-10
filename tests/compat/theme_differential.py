#!/usr/bin/env python3
"""tests/compat/theme_differential.py

Runs the C++ theme system and nterm-qt's own against the same theme files and
compares the output. Same shape as the session-store differential: neither side
asserts against its own idea of the answer, and the comparison is a third thing
that reads both.

Three checks per theme file:

  loader      every field the two parse out of the file, including colours
  stylesheet  the generated QSS, byte for byte
  derive      lighten/darken on the theme's four chrome colours

And two that are not per-file:

  broken      a file that fails to load must fail on BOTH sides. The shipped
              clean.yaml is zero bytes, so this is not hypothetical
  strict      an unknown top-level key must be refused by both, because
              engine.py's Theme(**data) raises TypeError on one and drops the
              whole theme

Usage:

    python3 tests/compat/theme_differential.py \\
        --probe build/tests/compat/theme_probe \\
        --themes ../ntermqt/ntermqt/theme/themes \\
        --ntermqt ../ntermqt

The nterm-qt checkout is imported, not vendored. It is the reference; a copy of
it here would drift and the drift would look like a pass.
"""

import argparse
import difflib
import subprocess
import sys
import tempfile
from pathlib import Path


def import_ntermqt_theme(root):
    """Make ntermqt.theme importable without running ntermqt/__init__.py.

    That __init__ imports the terminal widget, which imports PySide6, so a
    plain `import ntermqt.theme.engine` drags Qt into a comparison that has no
    Qt on either side. A stub package with __path__ pointed at the real
    directory lets the submodules import normally while the init is skipped.
    """
    import importlib
    import types

    pkg_dir = root / "ntermqt"
    if not (pkg_dir / "theme" / "engine.py").exists():
        raise SystemExit(f"no ntermqt/theme/engine.py under {root}")

    sys.path.insert(0, str(root))
    stub = types.ModuleType("ntermqt")
    stub.__path__ = [str(pkg_dir)]
    sys.modules["ntermqt"] = stub
    importlib.import_module("ntermqt.resources")
    importlib.import_module("ntermqt.theme.engine")
    importlib.import_module("ntermqt.theme.stylesheet")


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


def run_probe(probe, *args, expect_ok=True):
    proc = subprocess.run([str(probe), *args], capture_output=True, text=True)
    if expect_ok and proc.returncode != 0:
        raise RuntimeError(f"{args}: exit {proc.returncode}: {proc.stderr.strip()}")
    return proc


def python_dump(theme):
    """The same fields theme_probe dump prints, in the same order."""
    lines = []
    for key in sorted(theme.terminal_colors):
        lines.append(f"color.{key}={theme.terminal_colors[key]}")
    lines.append(f"name={theme.name}")
    lines.append(f"font_family={theme.font_family}")
    lines.append(f"font_size={theme.font_size}")
    lines.append(f"background_color={theme.background_color}")
    lines.append(f"foreground_color={theme.foreground_color}")
    lines.append(f"border_color={theme.border_color}")
    lines.append(f"accent_color={theme.accent_color}")
    lines.append(f"overlay_background={theme.overlay_background}")
    lines.append(f"overlay_text_color={theme.overlay_text_color}")
    return "\n".join(lines) + "\n"


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


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--probe", required=True, help="path to theme_probe")
    ap.add_argument("--themes", required=True, help="theme YAML directory")
    ap.add_argument("--ntermqt", required=True, help="nterm-qt checkout root")
    args = ap.parse_args()

    import_ntermqt_theme(Path(args.ntermqt).resolve())
    from ntermqt.theme.engine import Theme
    from ntermqt.theme.stylesheet import generate_stylesheet, _lighten, _darken

    probe = Path(args.probe).resolve()
    theme_dir = Path(args.themes).resolve()
    files = sorted(theme_dir.glob("*.yaml"))
    if not files:
        print(f"no theme files in {theme_dir}")
        return 1

    r = Result()

    for path in files:
        # --- does it load at all, and do both sides agree about that? -------
        try:
            theme = Theme.load(path)
            py_ok = True
        except Exception as exc:  # exactly what load_themes catches
            theme = None
            py_ok = False
            py_error = exc

        cpp = run_probe(probe, "dump", str(path), expect_ok=False)
        cpp_ok = cpp.returncode == 0

        r.check(
            f"{path.name}: load agrees",
            py_ok == cpp_ok,
            f"python loaded={py_ok} c++ loaded={cpp_ok}\n"
            f"  python: {'ok' if py_ok else py_error}\n"
            f"  c++:    {'ok' if cpp_ok else cpp.stderr.strip()}",
        )
        if not (py_ok and cpp_ok):
            continue

        # --- loader ---------------------------------------------------------
        r.check(
            f"{path.name}: fields",
            python_dump(theme) == cpp.stdout,
            diff(python_dump(theme), cpp.stdout),
        )

        # --- stylesheet, byte for byte --------------------------------------
        py_qss = generate_stylesheet(theme)
        cpp_qss = run_probe(probe, "stylesheet", str(path)).stdout
        r.check(
            f"{path.name}: stylesheet",
            py_qss == cpp_qss,
            diff(py_qss, cpp_qss),
        )

        # --- the derive helpers, on this theme's own colours -----------------
        for label, value in (
            ("background", theme.background_color),
            ("foreground", theme.foreground_color),
            ("border", theme.border_color),
            ("accent", theme.accent_color),
        ):
            out = run_probe(probe, "derive", value).stdout
            want = f"lighten={_lighten(value, 0.1)}\ndarken={_darken(value, 0.1)}\n"
            r.check(f"{path.name}: derive {label}", out == want, diff(want, out))

    # --- a file both must refuse -------------------------------------------
    with tempfile.TemporaryDirectory() as tmp:
        bad = Path(tmp) / "unknown_key.yaml"
        bad.write_text('name: bogus\nunknown_key: "x"\n')
        try:
            Theme.load(bad)
            py_ok = True
        except Exception:
            py_ok = False
        cpp_ok = run_probe(probe, "dump", str(bad), expect_ok=False).returncode == 0
        r.check(
            "unknown top-level key refused by both",
            (not py_ok) and (not cpp_ok),
            f"python loaded={py_ok} c++ loaded={cpp_ok}",
        )

        empty = Path(tmp) / "empty.yaml"
        empty.write_text("")
        try:
            Theme.load(empty)
            py_ok = True
        except Exception:
            py_ok = False
        cpp_ok = run_probe(probe, "dump", str(empty), expect_ok=False).returncode == 0
        r.check(
            "empty file refused by both",
            (not py_ok) and (not cpp_ok),
            f"python loaded={py_ok} c++ loaded={cpp_ok}",
        )

    return r.report()


if __name__ == "__main__":
    sys.exit(main())
