"""Check valid and invalid pointer flows in both build modes and allocators."""
import argparse
import pathlib
import subprocess
import sys


VALID = {
    "clean": 42,
    "aggregate_clean": 42,
    "return_clean": 42,
    "global_clean": 42,
    "allocator_clean": 42,
    "literal": 194,
    "foreign": 98,
    "string_clean": 217,
}
INVALID = {
    name: "after it was freed"
    for name in (
        "realloc", "call", "indirect", "field", "copy", "memcpy",
        "byte_copy", "stack_reuse", "double_free", "aggregate_stale",
        "allocator_stale",
        "foreign_stale",
    )
}
INVALID.update({
    "static_free": "free of stack or global memory",
    "derived": "outside its allocation",
    "memcpy_overflow": "outside its allocation",
    "unknown": "no tracked allocation identity",
    "foreign_overflow": "outside its allocation",
})


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--output", default="bin/safety-regression")
    args = parser.parse_args()
    compiler = pathlib.Path(args.compiler).resolve()
    output = pathlib.Path(args.output).resolve()
    output.mkdir(parents=True, exist_ok=True)
    root = pathlib.Path(__file__).resolve().parent.parent
    failures = []
    count = 0
    for release in (False, True):
        for native in (False, True):
            flags = ["--safe"]
            if release:
                flags.append("--release")
            if native:
                flags.append("--native-heap")
            for name in list(VALID) + list(INVALID):
                label = f"{name} release={release} native={native}"
                executable = output / (name + (".exe" if sys.platform == "win32" else ""))
                source = root / "tests" / f"test_safe_identity_{name}.mettle"
                build = subprocess.run(
                    [str(compiler), *flags, "--stdlib", str(root / "stdlib"),
                     "--build", str(source), "-o", str(executable)],
                    cwd=root, capture_output=True, text=True, timeout=120,
                )
                count += 1
                if build.returncode:
                    failures.append(f"{label}: build failed\n{build.stdout}{build.stderr}")
                    continue
                run = subprocess.run([str(executable)], cwd=root, capture_output=True,
                                     text=True, timeout=30)
                expected = VALID.get(name, 1)
                message = run.stdout + run.stderr
                if run.returncode != expected or (name in INVALID and INVALID[name] not in message):
                    failures.append(f"{label}: exit {run.returncode}, expected {expected}\n{message}")
    for failure in failures:
        print(failure)
    print(f"{count} compiled cases, {len(failures)} failures")
    return bool(failures)


if __name__ == "__main__":
    sys.exit(main())
