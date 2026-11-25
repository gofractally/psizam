#!/usr/bin/env python3
"""Runs psizam_wasmtime_differential_fuzz --admission-only over a directory
written by gen_admission_corpus.py and groups the splits it reports.

    run_admission_corpus.py --tool BIN --dir DIR [--log FILE]

The files go to the tool in batches. A batch that crashes or does not finish
is run again one file at a time, so a crash or a hang is pinned on its file
and the rest of the batch still counts. Splits are grouped by twist family and
wasmtime's message with the numbers taken out; each group shows how many
modules are in it and its smallest module in hex. Standard library only.
"""

import argparse
import collections
import os
import sys
import re
import subprocess

LINE = re.compile(r"^(SPLIT|PREFLIGHT|ADMIT|REFUSE) (\S+?)(?:: (.*))?$")


def run(tool, files, timeout):
    """Returns ({file: (verdict, message)}, failure or None)."""
    try:
        p = subprocess.run([tool, "--admission-only", "--verbose"] + files, capture_output=True, timeout=timeout)
        out, failure = p.stdout, None if 0 <= p.returncode <= 255 and b"modules " in p.stdout else "exit %d" % p.returncode
    except subprocess.TimeoutExpired as e:
        out, failure = e.stdout or b"", "no result after %ds" % timeout
    seen = {}
    for line in out.decode("utf-8", "replace").splitlines():
        m = LINE.match(line)
        if m:
            seen[m.group(2)] = (m.group(1), m.group(3) or "")
    return seen, failure


def normalise(message):
    message = re.sub(r"0x[0-9a-fA-F]+", "N", message)
    message = re.sub(r"\d+", "N", message)
    return message


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--tool", required=True)
    ap.add_argument("--dir", required=True)
    ap.add_argument("--batch", type=int, default=200)
    ap.add_argument("--timeout", type=int, default=120, help="seconds for a batch, and for one file on a rerun")
    ap.add_argument("--log", help="write one line per module here")
    ap.add_argument("--generate", metavar="GENERATOR",
                    help="write the whole catalogue (seed 1) into --dir with this generator first")
    ap.add_argument("--expect", action="append", default=[], metavar="FAMILY/TWIST",
                    help="a split that is known and accepted (repeatable); with --check any other split fails")
    ap.add_argument("--check", action="store_true",
                    help="exit 1 if a module crashed or hung the tool, or split and is not expected")
    args = ap.parse_args()

    if args.generate:
        os.makedirs(args.dir, exist_ok=True)
        for f in os.listdir(args.dir):
            if f.endswith(".wasm"):
                os.remove(os.path.join(args.dir, f))
        subprocess.run([sys.executable, args.generate, "--seed", "1", "--count", "0", "--out", args.dir],
                       check=True, stdout=subprocess.DEVNULL)
    files = sorted(os.path.join(args.dir, f) for f in os.listdir(args.dir) if f.endswith(".wasm"))
    results, broken = {}, []
    for at in range(0, len(files), args.batch):
        batch = files[at : at + args.batch]
        seen, failure = run(args.tool, batch, args.timeout)
        if failure:
            seen = {}
            for f in batch:
                one, failure = run(args.tool, [f], args.timeout)
                if failure:
                    broken.append((f, failure))
                    one = {f: ("BROKEN", failure)}
                seen.update(one)
        results.update(seen)

    if args.log:
        with open(args.log, "w") as log:
            for f in files:
                verdict, message = results.get(f, ("MISSING", ""))
                log.write("%s %s: %s\n" % (verdict, os.path.basename(f), message))

    count = collections.Counter(v for v, _ in results.values())
    admitted = count["ADMIT"] + count["SPLIT"] + count["PREFLIGHT"]
    print("modules %d, admitted %d, refused %d, splits %d, admitted but refused by the shared-memory preflight %d, "
          "crashed or hung %d" % (len(files), admitted, count["REFUSE"], count["SPLIT"], count["PREFLIGHT"], len(broken)))
    for f, failure in broken:
        print("BROKEN %s: %s" % (f, failure))

    groups = collections.defaultdict(list)
    for f, (verdict, message) in results.items():
        if verdict in ("SPLIT", "PREFLIGHT"):
            family = os.path.basename(f).split("__")[1]
            groups[(verdict, family, normalise(message))].append((os.path.getsize(f), f, message))
    for (verdict, family, _), members in sorted(groups.items()):
        members.sort()
        size, smallest, message = members[0]
        print("\n%s family=%s modules=%d" % (verdict, family, len(members)))
        print("  message: %s" % message)
        print("  smallest: %s (%d bytes)" % (os.path.basename(smallest), size))
        print("  hex: %s" % open(smallest, "rb").read().hex())
        print("  twists: %s" % " ".join(os.path.basename(f).split("__")[2][:-5] for _, f, _ in members[:40])
              + (" ..." if len(members) > 40 else ""))

    if args.check:
        unexpected = []
        for f, (verdict, message) in sorted(results.items()):
            if verdict == "SPLIT":
                parts = os.path.basename(f)[:-5].split("__")
                if "%s/%s" % (parts[1], parts[2]) not in args.expect:
                    unexpected.append((f, message))
        for f, message in unexpected:
            print("UNEXPECTED SPLIT %s: %s" % (os.path.basename(f), message))
        if unexpected or broken or not files:
            sys.exit(1)


if __name__ == "__main__":
    main()
