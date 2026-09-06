"""Fixtures no other harness names, run against what they answer.

Each one checks itself by returning its answer, so the exit code is the check.
They were simply never registered anywhere, which is how one of them
(`test_defer_global_assignment`, which asserts that a deferred write cannot
change a return value) sat failing with nobody looking.

A file listed here is accounted for. A file in `tests/` that is listed nowhere
is reported by `tests/check_test_orphans.py`, so the count cannot drift back up.
"""

import argparse
import json
import pathlib
import subprocess
import sys


ROOT = pathlib.Path(__file__).resolve().parent.parent
MANIFEST = ROOT / "tests" / "fixture_manifest.json"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--output", default=".tmp/fixtures")
    args = parser.parse_args()
    compiler = pathlib.Path(args.compiler).resolve()
    out_dir = pathlib.Path(args.output).resolve()
    out_dir.mkdir(parents=True, exist_ok=True)

    manifest = json.loads(MANIFEST.read_text())
    failures = []
    for case in manifest["cases"]:
        source = case["source"]
        stem = pathlib.Path(source).stem
        exe = out_dir / (stem + ".exe")
        build = subprocess.run(
            [str(compiler), "--build", "--stdlib", str(ROOT / "stdlib"),
             str(ROOT / source), "-o", str(exe)],
            cwd=ROOT, capture_output=True, text=True, timeout=1800)
        if build.returncode != 0:
            failures.append("%s: build exited %d\n%s%s"
                            % (source, build.returncode, build.stdout[-1500:],
                               build.stderr[-1500:]))
            continue
        if not case.get("run", True):
            continue
        run = subprocess.run([str(exe)], cwd=ROOT, capture_output=True,
                             text=True, timeout=300)
        expected = case["exit"]
        if sys.platform != "win32":
            expected &= 255
        if run.returncode != expected:
            failures.append("%s: exit %d, expected %d\n%s%s"
                            % (source, run.returncode, expected,
                               run.stdout[-1000:], run.stderr[-1000:]))

    for failure in failures:
        print(failure)
    print("%d fixtures checked, %d failures"
          % (len(manifest["cases"]), len(failures)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
