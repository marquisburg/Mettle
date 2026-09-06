"""Every tests/*.mettle is referenced by a harness, or says why it is not.

A fixture nothing runs is not a test. Eighty-five of them had accumulated:
debugging repros that print values for a human to read and assert nothing, and
`err_*` files expecting a diagnostic that three of them no longer produce. The
second kind is the dangerous one, because a check that stopped holding looks
exactly like a check that is still there.

A file is accounted for when some harness names it, or when it is a support
file another fixture imports, or when it is listed below with a reason.
"""

import argparse
import io
import os
import pathlib
import re
import sys


ROOT = pathlib.Path(__file__).resolve().parent.parent
TESTS = ROOT / "tests"

# Harnesses and generators that name fixtures.
REFERENCES = [
    "tests/run_tests.ps1",
    "tests/run_safety_identity.py",
    "tests/run_safety_optimizer.py",
    "tests/run_knob_sweep.py",
    "tests/run_interp_native_oracle.py",
    "tests/run_fixture_manifest.py",
    "tests/safety_optimizer_corpus.json",
    "tests/fixture_manifest.json",
    "Makefile",
    "build.bat",
]

# Fixtures no harness names on purpose. Each needs a reason, and the reason is
# the point: an unexplained orphan is a check that quietly stopped running.
ALLOWED = {}


def referenced_names():
    """Every fixture stem any harness mentions, however it spells it."""
    seen = set()
    for name in REFERENCES:
        path = ROOT / name
        if not path.exists():
            continue
        text = io.open(path, encoding="utf-8", errors="replace").read()
        for stem in re.findall(r"([A-Za-z0-9_./\\-]+)\.mettle", text):
            seen.add(pathlib.PurePath(stem.replace("\\", "/")).name)
        # A harness may build a name from a table of bare stems.
        for stem in re.findall(r'"(test_[A-Za-z0-9_]+)"', text):
            seen.add(stem)
        for stem in re.findall(r'"(err_[A-Za-z0-9_]+)"', text):
            seen.add(stem)
    return seen


def imported_names():
    """Fixtures other fixtures pull in, which the harness never names."""
    seen = set()
    for source in TESTS.glob("*.mettle"):
        text = io.open(source, encoding="utf-8", errors="replace").read()
        for target in re.findall(r'import\s+"([^"]+)"', text):
            seen.add(pathlib.PurePath(target).name)
    return seen


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--list", action="store_true",
                        help="print every orphan instead of failing")
    args = parser.parse_args()

    named = referenced_names()
    imported = imported_names()

    orphans = []
    for source in sorted(TESTS.glob("*.mettle")):
        stem = source.stem
        if stem in named or source.name in named:
            continue
        if stem in imported or source.name in imported:
            continue
        if stem in ALLOWED:
            continue
        orphans.append(stem)

    if args.list:
        for stem in orphans:
            print(stem)
        return 0

    for stem in orphans:
        print("tests/%s.mettle is referenced by no harness" % stem)
    total = len(list(TESTS.glob("*.mettle")))
    print("%d fixtures, %d referenced by nothing" % (total, len(orphans)))
    return 1 if orphans else 0


if __name__ == "__main__":
    sys.exit(main())
