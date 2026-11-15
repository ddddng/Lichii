#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Texture cache images over frames, from the EDEN_NX2_TC_TRACE=1 lines of a log.

  tc_churn.py <log> [--every N]

Prints one row per N frames (default 10): images created and deleted in those frames, delete
reasons, and what the cache held at the end of them (the per-frame TCTRACE frame line: images,
how many and how many MiB of them the GPU wrote). Standard library only.
"""

import argparse
import collections
import re
import sys

ANSI = re.compile(r"\x1b\[[0-9;]*m")
FRAME = re.compile(r"TCTRACE frame=(\d+) images=(\d+) gpu_written=(\d+) gpu_written_kib=(\d+)")
LINE = re.compile(r"TCTRACE (create|delete) id=(\d+) gpu=(0x[0-9a-f]+) .*?frame=(\d+)")
REASON = re.compile(r"reason=(\S+)")
SHAPE = re.compile(r"fmt=(\d+) (\d+x\d+x\d+)")


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("log")
    parser.add_argument("--every", type=int, default=10)
    args = parser.parse_args()

    created = collections.Counter()
    deleted = collections.Counter()
    reasons = collections.defaultdict(collections.Counter)
    alive_ids = {}
    held = {}
    last_frame = 0
    with open(args.log, encoding="utf-8", errors="replace") as log:
        for raw in log:
            line = ANSI.sub("", raw)
            frame_line = FRAME.search(line)
            if frame_line:
                frame, images, written, written_kib = map(int, frame_line.groups())
                held[frame // args.every] = (images, written, written_kib >> 10)
                last_frame = max(last_frame, frame)
                continue
            match = LINE.search(line)
            if not match:
                continue
            kind, image_id, gpu, frame = match.groups()
            frame = int(frame)
            last_frame = max(last_frame, frame)
            bucket = frame // args.every
            shape = SHAPE.search(line)
            if kind == "create":
                created[bucket] += 1
                alive_ids[image_id] = (gpu, shape.groups() if shape else None)
            else:
                deleted[bucket] += 1
                reasons[bucket][REASON.search(line).group(1)] += 1
                alive_ids.pop(image_id, None)

    print(f"{'frames':>11} {'created':>8} {'deleted':>8} {'images':>6} {'gpu-written':>11} "
          f"{'MiB':>6}  delete reasons")
    for bucket in range(last_frame // args.every + 1):
        first = bucket * args.every
        images, written, written_mib = held.get(bucket, (0, 0, 0))
        why = ", ".join(f"{name} {count}" for name, count in sorted(reasons[bucket].items()))
        print(f"{first:>5}-{first + args.every - 1:<5} {created[bucket]:>8} {deleted[bucket]:>8} "
              f"{images:>6} {written:>11} {written_mib:>6}  {why}")
    print(f"created and not deleted at the end: {len(alive_ids)}")
    by_address = collections.Counter(gpu for gpu, _ in alive_ids.values())
    for gpu, count in by_address.most_common(4):
        print(f"  {count} alive at {gpu}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
