"""Check T2-01 vectorization, origin preservation and the fixed audit corpus."""

import argparse
import hashlib
import json
import pathlib
import re
import subprocess
import sys


ROOT = pathlib.Path(__file__).resolve().parent.parent
TOTALS = ("accesses", "proved", "hoisted", "spanned", "extentTests", "regionCalls")


def command(args, timeout=120):
    return subprocess.run([str(arg) for arg in args], cwd=ROOT,
                          capture_output=True, text=True, timeout=timeout)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--output", default=".tmp/safety-optimizer")
    parser.add_argument("--skip-corpus", action="store_true")
    args = parser.parse_args()
    compiler = pathlib.Path(args.compiler).resolve()
    output = pathlib.Path(args.output).resolve()
    output.mkdir(parents=True, exist_ok=True)
    failures = []
    measured = []

    def build(source):
        stem = pathlib.Path(source).stem
        executable = output / (stem + ".exe")
        result = command([compiler, "--build", "--release", "--safe",
                          "--explain-json", "--stdlib", ROOT / "stdlib",
                          ROOT / source, "-o", executable])
        (output / (stem + ".compile.txt")).write_text(result.stdout + result.stderr)
        if result.returncode:
            failures.append(f"{source}: compilation failed\n{result.stdout}{result.stderr}")
            return None, None
        return executable, json.loads(executable.with_suffix(".explain.json").read_text())

    for source in ("tests/test_safe_vectorized.mettle",
                   "tests/test_safe_vectorized_heap.mettle"):
        executable, report = build(source)
        if executable is None:
            continue
        for function in ("scale", "total"):
            remarks = [remark for remark in report["remarks"]
                       if remark.get("fn") == function and remark.get("kind") == "loop"]
            if not any(remark.get("positive") and "vectorized" in remark["headline"]
                       for remark in remarks):
                failures.append(f"{source}: {function} did not vectorize")
        run = command([executable], timeout=30)
        if run.returncode:
            failures.append(f"{source}: wrong result {run.returncode}\n{run.stdout}{run.stderr}")

    executable, _ = build("tests/test_safe_plain_storage_stale.mettle")
    if executable is not None:
        run = command([executable], timeout=30)
        if run.returncode != 1 or "after it was freed" not in run.stdout + run.stderr:
            failures.append("private integer storage lost a pointer origin\n" + run.stdout + run.stderr)

    executable, _ = build("tests/test_safe_widen_overflow.mettle")
    if executable is not None:
        run = command([executable], timeout=10)
        if run.returncode != 1 or "outside its allocation" not in run.stdout + run.stderr:
            failures.append("widened byte count overflow escaped its check\n" + run.stdout + run.stderr)

    for source, expected_exit, message in (
            ("tests/test_safe_zero_trip.mettle", 0, ""),
            ("tests/test_safe_loop_region_end.mettle", 1, "after it was freed")):
        executable, _ = build(source)
        if executable is not None:
            run = command([executable], timeout=10)
            if run.returncode != expected_exit or message not in run.stdout + run.stderr:
                failures.append(f"{source}: unexpected result {run.returncode}\n" +
                                run.stdout + run.stderr)

    # A loop the analysis cannot settle resolves its allocation once and
    # compares against that extent. An empty resolution means the origin named
    # nothing live, and reading it as a limit lets every access in the loop
    # through, which is exactly where an escape costs the most.
    executable, report = build("tests/test_safe_span_unknown.mettle")
    if executable is not None:
        if not any(note["kind"] == "span" and note["function"] == "walk"
                   for note in report["safety"]["survivors"]):
            failures.append("the span resolution path is no longer being tested")
        for argument, message in (("2", "no tracked allocation identity"),
                                  ("3", "after it was freed")):
            run = command([executable, argument], timeout=10)
            if run.returncode != 1 or message not in run.stdout + run.stderr:
                failures.append(
                    f"an empty span resolution passed unchecked ({argument})\n" +
                    run.stdout + run.stderr)

    manifest = json.loads((ROOT / "tests/safety_optimizer_corpus.json").read_text())
    if not args.skip_corpus:
        for case in manifest["cases"]:
            source = case["source"]
            digest = hashlib.sha256((ROOT / source).read_text().encode()).hexdigest()
            if digest != case["sha256"]:
                failures.append(f"{source}: corpus source changed; baseline must be remeasured")
                continue
            executable, report = build(source)
            if executable is None:
                continue
            measured.append({"source": source,
                             "safety": {key: report["safety"][key] for key in TOTALS}})
            for probe in case["runs"]:
                run = command([executable, *probe["args"]], timeout=30)
                expected_exit = probe["exit"]
                if sys.platform != "win32" and expected_exit is not None:
                    expected_exit &= 255
                failure = re.search(r"Fatal error:[^\r\n]*", run.stdout + run.stderr)
                actual = failure.group(0) if failure else None
                if run.returncode != expected_exit or actual != probe["fatal"]:
                    failures.append(f"{source} {probe['args']}: exit {run.returncode}, "
                                    f"expected {expected_exit}\n"
                                    f"expected: {probe['fatal']}\nactual:   {actual}")

    baseline = {key: sum(case["baseline"][key] for case in manifest["cases"])
                for key in TOTALS}
    current = {key: sum(case["safety"][key] for case in measured) for key in TOTALS}
    (output / "measurements.json").write_text(json.dumps(
        {"baseline": baseline, "current": current, "sources": measured,
         "failures": failures}, indent=2) + "\n")
    for failure in failures:
        print(failure)
    print(f"{len(measured)} corpus sources checked, {len(failures)} failures")
    return bool(failures)


if __name__ == "__main__":
    sys.exit(main())
