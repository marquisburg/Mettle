"""Native answers against each other, and against the compile-time interpreter.

`--verify` snapshots IR before a pass, interprets both versions and compares.
That proves the optimizer preserves meaning and says nothing about whether the
encoder emitted what the IR asked for. The signed-zero miscompile lived in the
half it could not see, which is why 639 tests, a differential fuzzer and
translation validation all missed it.

This runs `tests/oracle_cases.mettle` three ways and requires one answer:

  - the register-allocating backend and the baseline one, at -O0 and --release,
    which catches a lowering that only one of the two got right;
  - the compile-time interpreter, which catches an encoder that disagrees with
    the language.

It holds no expected values. Every answer is compared against another engine's
answer for the same expression, so the suite cannot drift into asserting what
the compiler currently does.
"""

import argparse
import os
import pathlib
import re
import subprocess
import sys


ROOT = pathlib.Path(__file__).resolve().parent.parent
SOURCE = "tests/oracle_cases.mettle"

NATIVE = [
    ("mir/debug", [], {"METTLE_MIR": None}),
    ("mir/opt", ["-O"], {"METTLE_MIR": None}),
    ("mir/release", ["--release"], {"METTLE_MIR": None}),
    ("baseline/debug", [], {"METTLE_MIR": "0"}),
    ("baseline/release", ["--release"], {"METTLE_MIR": "0"}),
    # The checked build answers the same questions or it is not the same
    # language, and the instrumentation runs over every one of these values.
    ("safe/release", ["--release", "--safe"], {"METTLE_MIR": None}),
    ("safe/debug", ["--safe"], {"METTLE_MIR": None}),
    # --check-overflow is deliberately absent: several cases here overflow a
    # signed type on purpose, which is the behaviour being compared, and that
    # mode is supposed to stop the program when they do.
    # No vectorizer, no SLP: the scalar answer is the reference.
    ("scalar/release", ["--release"],
     {"METTLE_MIR": None, "METTLE_NO_SIMD": "1", "NO_SLP": "1"}),
]


def run(args, env=None, timeout=300):
    return subprocess.run([str(a) for a in args], cwd=ROOT, capture_output=True,
                          text=True, timeout=timeout, env=env)


def answers(text):
    found = {}
    for line in text.splitlines():
        match = re.match(r"^([a-z0-9_]+)=(-?\d+)$", line.strip())
        if match:
            found[match.group(1)] = int(match.group(2))
    return found


def native_answers(compiler, out_dir, label, flags, overrides):
    env = dict(os.environ)
    for key, value in overrides.items():
        if value is None:
            env.pop(key, None)
        else:
            env[key] = value
    exe = out_dir / ("oracle." + label.replace("/", ".") + ".exe")
    build = run([compiler, "--build", *flags, "--stdlib", ROOT / "stdlib",
                 ROOT / SOURCE, "-o", exe], env=env)
    if build.returncode:
        return None, "build exited %d\n%s%s" % (build.returncode, build.stdout,
                                                build.stderr)
    result = run([exe], env=env, timeout=120)
    if result.returncode:
        return None, "run exited %d" % result.returncode
    return answers(result.stdout), None


def interpreter_answers(compiler, out_dir, expected):
    """Ask the interpreter the same questions, through assertions.

    The interpreter has no output of its own to read, so each case becomes a
    `@test` asserting the answer some native build gave. A disagreement is an
    assertion failure naming both values, which is the report we want anyway.
    """
    source = (ROOT / SOURCE).read_text()
    probe = out_dir / "oracle_interp.mettle"
    body = [source, "", "// Generated: one assertion per case, against the "
            "native answer.", ""]
    for name in sorted(expected):
        body.append("@test fn interp_%s() {" % name)
        body.append("  assert_eq(case_%s(0), %d);" % (name, expected[name]))
        body.append("}")
        body.append("")
    probe.write_text("\n".join(body))
    result = run([compiler, "test", probe, "--stdlib", ROOT / "stdlib"])
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--output", default=".tmp/oracle")
    args = parser.parse_args()
    compiler = pathlib.Path(args.compiler).resolve()
    out_dir = pathlib.Path(args.output).resolve()
    out_dir.mkdir(parents=True, exist_ok=True)

    failures = []
    collected = {}
    for label, flags, overrides in NATIVE:
        got, why = native_answers(compiler, out_dir, label, flags, overrides)
        if why:
            failures.append("%s: %s" % (label, why))
            continue
        collected[label] = got

    if not collected:
        print("no configuration produced answers")
        return 1

    reference_label = sorted(collected)[0]
    reference = collected[reference_label]
    if not reference:
        failures.append("%s printed no answers" % reference_label)

    for label, got in sorted(collected.items()):
        if label == reference_label:
            continue
        for name in sorted(set(reference) | set(got)):
            a = reference.get(name)
            b = got.get(name)
            if a != b:
                failures.append("%s: %s gave %s, %s gave %s"
                                % (name, reference_label, a, label, b))

    if reference:
        result = interpreter_answers(compiler, out_dir, reference)
        if result.returncode:
            failures.append("the interpreter disagrees with %s:\n%s%s"
                            % (reference_label, result.stdout[-4000:],
                               result.stderr[-4000:]))

    for failure in failures:
        print(failure)
    print("%d cases across %d native configurations, %d failures"
          % (len(reference), len(collected), len(failures)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
