#!/usr/bin/env python3
"""ctest helper for "this combination of defines must not compile" checks.

`./test`'s negative_checks()/engine_event_queue_negative_checks()/
signed_char_negative_checks()/etc. compiled a production source under a
handful of flag combinations and required each one to fail (a missing
KISAK_SP guard, KISAK_MP/KISAK_NO_FASTFILES leaking into a Switch-only TU,
non-Switch builds of a Horizon-only file, or -funsigned-char slipping past
the q_shared.h guard) -- except signed_char, which also has one combination
that must compile cleanly (-fsigned-char).  A plain CMake add_executable()
can't express "this must fail to build" without breaking `cmake --build`,
so this script does the compile itself, at ctest time, exactly like the
shell functions did, and is the test's whole COMMAND.

Usage:
  negative_compile_check.py --cc CC --std STD --source SRC
      [-I dir]... [--common-flag FLAG]...
      [--fail-variant [FLAG...] --end]...   # compile must FAIL
      [--pass-variant [FLAG...] --end]...   # compile must SUCCEED

Exit 0 (ctest PASS) only if every --fail-variant failed to compile and every
--pass-variant compiled cleanly; otherwise exit 1 and name which variant(s)
did not behave as required.
"""
import argparse
import subprocess
import sys
import tempfile
import os


def parse_variants(argv, flag):
    """Pull repeated `--flag [args...] --end` groups out of argv by hand:
    argparse's nargs='*' can't tell a variant's own flags (e.g. -DKISAK_SP)
    apart from the next --fail-variant/--pass-variant, so each group is
    terminated explicitly with --end."""
    variants = []
    i = 0
    while i < len(argv):
        if argv[i] == flag:
            i += 1
            group = []
            while i < len(argv) and argv[i] != "--end":
                group.append(argv[i])
                i += 1
            variants.append(group)
        i += 1
    return variants


def main():
    argv = sys.argv[1:]
    p = argparse.ArgumentParser()
    p.add_argument("--cc", required=True)
    p.add_argument("--std", required=True)
    p.add_argument("--source", required=True)
    p.add_argument("-I", action="append", default=[], dest="includes")
    p.add_argument("--common-flag", action="append", default=[])
    args, _ = p.parse_known_args(argv)

    fail_variants = parse_variants(argv, "--fail-variant")
    pass_variants = parse_variants(argv, "--pass-variant")
    if not fail_variants and not pass_variants:
        print("negative_compile_check.py: no --fail-variant/--pass-variant given",
              file=sys.stderr)
        return 2

    common = list(args.common_flag)
    for inc in args.includes:
        common += ["-I", inc]

    problems = []
    with tempfile.TemporaryDirectory() as td:
        def compile_variant(flags):
            obj = os.path.join(td, f"neg-{len(problems)}-{os.getpid()}.o")
            cmd = [args.cc, f"-std={args.std}", "-Werror", *common, *flags,
                   "-c", args.source, "-o", obj]
            r = subprocess.run(cmd, capture_output=True, text=True)
            return r

        for variant in fail_variants:
            r = compile_variant(variant)
            if r.returncode == 0:
                problems.append(f"expected-fail variant {variant} compiled cleanly")
        for variant in pass_variants:
            r = compile_variant(variant)
            if r.returncode != 0:
                problems.append(f"expected-pass variant {variant} failed to compile:\n{r.stderr}")

    if problems:
        for problem in problems:
            print(f"FAIL: {problem}", file=sys.stderr)
        return 1
    print(f"OK: {len(fail_variants)} reject/{len(pass_variants)} accept variant(s) behaved as expected")
    return 0


if __name__ == "__main__":
    sys.exit(main())
