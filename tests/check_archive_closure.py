"""Every backend source reaches the archive, with no list to keep in step.

`build.bat` used to type out the IR core as twenty object names while the
compile step above it wildcarded the same directory. A new `src/ir/foo.c` then
landed in `mettle.exe` and was silently absent from `libmtlc`, and because the
freestanding link is an archive-closure gate, that surfaced as an unresolved
symbol far from the change and on one operating system only.

Both build systems now wildcard and exclude the lowering translation units by
name. This checks the result rather than the rule: list the archive's members
and require one for every source that belongs in it.
"""

import argparse
import pathlib
import subprocess
import sys


ROOT = pathlib.Path(__file__).resolve().parent.parent

# The lowering half is the frontend's, and stays out of the backend archive.
LOWERING = {
    "ir_lowering", "ir_lower_address", "ir_lower_defer", "ir_lower_expr",
    "ir_lower_stmt", "ir_lower_support", "ir_lower_switch_match",
    "ir_lower_types",
}

DIRECTORIES = ["src/ir", "src/ir/optimizer", "src/codegen/binary",
               "src/codegen/asm", "src/linker"]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--archive", required=True)
    parser.add_argument("--ar", default="ar")
    args = parser.parse_args()

    archive = pathlib.Path(args.archive)
    if not archive.exists():
        print("no archive at %s" % archive)
        return 1

    listing = subprocess.run([args.ar, "t", str(archive)], cwd=ROOT,
                             capture_output=True, text=True)
    if listing.returncode:
        print("could not list %s:\n%s%s" % (archive, listing.stdout,
                                            listing.stderr))
        return 1
    members = {pathlib.Path(line.strip()).stem
               for line in listing.stdout.splitlines() if line.strip()}

    missing = []
    for directory in DIRECTORIES:
        for source in sorted((ROOT / directory).glob("*.c")):
            if source.stem in LOWERING:
                continue
            if source.stem not in members:
                missing.append("%s/%s.c" % (directory, source.stem))

    for name in missing:
        print("%s compiles into the compiler but not into %s"
              % (name, archive.name))
    print("%d archive members, %d sources missing" % (len(members), len(missing)))
    return 1 if missing else 0


if __name__ == "__main__":
    sys.exit(main())
