#!/usr/bin/env python3
"""Fail if any function's stack frame exceeds the budget.

gcc -fstack-usage writes a .su file per translation unit, one line per
function: "file:line:col<tab>bytes<tab>qualifier". The qualifier is "static"
for a frame gcc sized at compile time, "dynamic" or "bounded" when the frame
depends on a runtime value.

The budget matters more than usual here: the Win98 agent runs with a
default 1 MiB main-thread stack and smaller worker stacks, and a frame that
quietly holds a whole packet or compression window is how that overflows.
An overflow is not the kind of bug that shows up in a test run.
"""

import argparse
import os
import sys

# 64 KiB, matching the project-wide limit. A frame anywhere near this is
# almost certainly an array that belongs on the heap.
DEFAULT_LIMIT = 64 * 1024


def parse(path):
    """Yield (function, bytes, qualifier) from one .su file."""
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        for line in fh:
            parts = line.rstrip("\n").split("\t")
            if len(parts) < 3:
                continue
            location, size, qualifier = parts[0], parts[1], parts[2]
            try:
                yield location, int(size), qualifier
            except ValueError:
                continue


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("directory", help="directory holding the .su files")
    ap.add_argument("--limit", type=int, default=DEFAULT_LIMIT,
                    help="maximum frame size in bytes (default %d)" % DEFAULT_LIMIT)
    ap.add_argument("--top", type=int, default=5,
                    help="how many of the largest frames to print")
    args = ap.parse_args()

    if not os.path.isdir(args.directory):
        sys.stderr.write("check_stack: no such directory: %s\n" % args.directory)
        return 2

    frames = []
    for name in sorted(os.listdir(args.directory)):
        if name.endswith(".su"):
            frames.extend(parse(os.path.join(args.directory, name)))

    if not frames:
        sys.stderr.write("check_stack: no .su files in %s\n" % args.directory)
        return 2

    frames.sort(key=lambda f: f[1], reverse=True)
    over = [f for f in frames if f[1] > args.limit]

    for location, size, qualifier in frames[:args.top]:
        sys.stdout.write("  %8d  %-10s %s\n" % (size, qualifier, location))

    # A dynamic frame has no compile-time bound, so the limit cannot be
    # checked against it. Worth naming rather than passing silently.
    dynamic = [f for f in frames if "dynamic" in f[2]]
    for location, size, qualifier in dynamic:
        sys.stdout.write("  note: %s has a %s frame (%d bytes measured)\n"
                         % (location, qualifier, size))

    if over:
        sys.stderr.write("check_stack: %d function(s) over the %d byte limit:\n"
                         % (len(over), args.limit))
        for location, size, _ in over:
            sys.stderr.write("  %d bytes  %s\n" % (size, location))
        return 1

    sys.stdout.write("  stack usage OK (largest %d bytes, limit %d)\n"
                     % (frames[0][1], args.limit))
    return 0


if __name__ == "__main__":
    sys.exit(main())
