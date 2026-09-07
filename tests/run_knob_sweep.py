"""Every environment knob, set, against the answer it is supposed not to change.

A knob that turns an optimization off is a policy decision, and a pass that
reports declining as failing takes the whole compile down with an internal
error. That has now happened twice (METTLE_NO_SIMD, then NO_SLP), both times
found by hand. This runs the sweep instead: set each knob, compile the corpus,
and require the same exit code and the same bytes on stdout as a build with no
knob set at all.

Tracing knobs are in the list too. They are allowed to say anything they like
on stderr and nothing at all on stdout.
"""

import argparse
import os
import pathlib
import subprocess
import sys


ROOT = pathlib.Path(__file__).resolve().parent.parent

# Knobs that select or suppress work. A wrong answer here is a miscompile.
BEHAVIOUR = [
    ("NO_SLP", "1"),
    ("METTLE_NO_SIMD", "1"),
    ("METTLE_LINEAR_ALLOC", "1"),
    ("METTLE_FPO", "0"),
    ("METTLE_IF_CONVERT", "1"),
    ("METTLE_EGRAPH", "1"),
    ("METTLE_FULL_CLEANUP", "1"),
    ("METTLE_INTERVAL_INTERFERENCE", "1"),
    ("METTLE_MIR_ADDR_STORE", "1"),
    ("METTLE_PREFETCH_DIST", "64"),
    ("METTLE_PGO_HOT", "1"),
    ("METTLE_SKIP_PASS", "all"),
    ("METTLE_ML_COLLAPSE_ALL", "1"),
    ("METTLE_ML_SPECULATIVE", "1"),
    ("METTLE_ML_AFFINE", "1"),
    ("METTLE_ML_PTR", "1"),
    ("METTLE_ML_BWLIB", "1"),
    ("METTLE_ML_GF2LIB", "1"),
]

# Knobs that only report. They must not change the program.
REPORTING = [
    ("METTLE_ALIAS_TRACE", "1"),
    ("METTLE_BP_TRACE", "1"),
    ("METTLE_LAYOUT_DEBUG", "1"),
    ("METTLE_LOOP_FINGERPRINT", "1"),
    ("METTLE_MIR_DUMP", "1"),
    ("METTLE_MIR_TRACE", "1"),
    ("METTLE_MIR_DUPLABEL", "1"),
    ("METTLE_PROM_TRACE", "1"),
    ("METTLE_REGALLOC_TRACE", "1"),
    ("METTLE_SHIFT_DEBUG", "1"),
    ("METTLE_SLP_TRACE", "1"),
    ("METTLE_SROA_DEBUG", "1"),
    ("METTLE_SAFETY_TRACE", "1"),
    ("METTLE_TIME_CODEGEN", "1"),
    ("METTLE_TIME_FUNCTIONS", "1"),
    ("METTLE_TIME_IR_PASSES", "1"),
    ("METTLE_TRACE_IR_PASSES", "1"),
    ("METTLE_VERIFY_STATS", "1"),
    ("METTLE_ML_TRACE", "1"),
    ("METTLE_ML_DISP", "1"),
    ("METTLE_ML_RISK", "1"),
    ("METTLE_ML_ACTIONS", "1"),
]

CORPUS = [
    "tests/test_knob_torture.mettle",
    "examples/dot_product/dot_product.mettle",
    "examples/heapsort/heapsort.mettle",
    "examples/crc32/crc32.mettle",
]

MODES = [("release", ["--release"]), ("safe", ["--release", "--safe"])]


def deterministic(text):
    """Drop what a benchmark prints about itself.

    The corpus doubles as the benchmark corpus, so several of these programs
    report how long they took. That is the one line that legitimately differs
    between two runs of the same binary, and comparing it would bury a real
    divergence under noise.
    """
    kept = []
    for line in text.splitlines():
        lowered = line.lower()
        if "time:" in lowered or "per pass" in lowered or " us" in lowered:
            continue
        kept.append(line)
    return "\n".join(kept)


def build_and_run(compiler, source, flags, out_dir, tag, env):
    stem = pathlib.Path(source).stem
    exe = out_dir / ("%s.%s.exe" % (stem, tag))
    build = subprocess.run(
        [str(compiler), "--build", *flags, "--stdlib", str(ROOT / "stdlib"),
         str(ROOT / source), "-o", str(exe)],
        cwd=ROOT, capture_output=True, text=True, timeout=300, env=env)
    if build.returncode != 0:
        return None, "build exited %d\n%s%s" % (build.returncode,
                                                build.stdout[-2000:],
                                                build.stderr[-2000:])
    run = subprocess.run([str(exe)], cwd=ROOT, capture_output=True, text=True,
                         timeout=120, env=env)
    return (run.returncode, deterministic(run.stdout)), None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--output", default=".tmp/knob-sweep")
    args = parser.parse_args()
    compiler = pathlib.Path(args.compiler).resolve()
    out_dir = pathlib.Path(args.output).resolve()
    out_dir.mkdir(parents=True, exist_ok=True)

    failures = []
    baseline = {}
    clean_env = {k: v for k, v in os.environ.items()
                 if not k.startswith("METTLE_") and k != "NO_SLP"}

    for source in CORPUS:
        for mode, flags in MODES:
            got, why = build_and_run(compiler, source, flags, out_dir,
                                     "base." + mode, clean_env)
            if why:
                failures.append("baseline %s (%s): %s" % (source, mode, why))
                continue
            baseline[(source, mode)] = got

    checked = 0
    for knob, value in BEHAVIOUR + REPORTING:
        env = dict(clean_env)
        env[knob] = value
        for source in CORPUS:
            for mode, flags in MODES:
                if (source, mode) not in baseline:
                    continue
                checked += 1
                got, why = build_and_run(compiler, source, flags, out_dir,
                                         "knob." + mode, env)
                if why:
                    failures.append("%s=%s %s (%s): %s"
                                    % (knob, value, source, mode, why))
                    continue
                want = baseline[(source, mode)]
                if got != want:
                    failures.append(
                        "%s=%s %s (%s): exit %d, output\n  %r\nexpected exit "
                        "%d, output\n  %r"
                        % (knob, value, source, mode, got[0], got[1][:400],
                           want[0], want[1][:400]))

    for failure in failures:
        print(failure)
    print("%d knob builds checked, %d failures" % (checked, len(failures)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
