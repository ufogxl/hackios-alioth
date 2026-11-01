#!/usr/bin/env python3
"""The device's status, and the README table that must not be written by hand.

A device records what works in `devices/<name>/status`, one line per area:

    <area>  <works|partial|missing>  <note>

and this turns that into the one table a reader sees. Keeping the table here instead of in the
README is not tidiness: the README's table said brightness was broken a day after the control
centre's slider had started working, because a status kept in prose drifts from the phone. The
status file is the record; the table is a rendering of it.

    tools/device-status.py table  alioth             print the table
    tools/device-status.py update alioth README.md   replace what is between the markers
    tools/device-status.py check  alioth             the file parses, and says what is wrong if not

The markers are, in the file being updated:

    <!-- status:alioth begin -->
    ...
    <!-- status:alioth end -->

Nothing between them survives; everything outside them is left alone.
"""

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# The areas a port is judged by, in the order they are brought up. A phone's own order is its own;
# this is the one the reference port uses, and a line outside it is a mistake worth hearing about.
AREAS = [
    "display", "touch", "buttons", "cpu", "gpu", "storage",
    "audio", "battery", "thermal", "haptic",
    "camera", "torch", "wifi", "bluetooth", "cellular", "network", "usb", "sensors",
]
STATES = ["works", "partial", "missing"]


def read_status(device):
    path = os.path.join(ROOT, "devices", device, "status")
    try:
        with open(path, encoding="utf-8") as f:
            lines = f.readlines()
    except OSError as e:
        sys.exit("device-status.py: %s: %s" % (path, e))

    seen = {}
    problems = []
    for number, raw in enumerate(lines, 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split(None, 2)
        if len(parts) < 2:
            problems.append("%s:%d: not '<area> <state> [note]': %r" % (path, number, line))
            continue
        area, state = parts[0], parts[1]
        note = parts[2] if len(parts) > 2 else ""
        if area not in AREAS:
            problems.append("%s:%d: unknown area %r (known: %s)" % (path, number, area, ", ".join(AREAS)))
        if state not in STATES:
            problems.append("%s:%d: state %r is not one of %s" % (path, number, state, ", ".join(STATES)))
        if area in seen:
            problems.append("%s:%d: %r already has a line" % (path, number, area))
        if state != "missing" and not note:
            problems.append("%s:%d: %r is %s and says nothing about how it was seen" % (path, number, area, state))
        seen[area] = (state, note)

    return seen, problems


def table(device):
    seen, problems = read_status(device)
    if problems:
        sys.exit("device-status.py: the status file does not hold together:\n  " + "\n  ".join(problems))

    rows = ["| Area | State | Note |", "|---|---|---|"]
    for area in AREAS:
        state, note = seen.get(area, ("missing", ""))
        rows.append("| %s | %s | %s |" % (area, state, note or "—"))

    counts = {s: sum(1 for a in AREAS if seen.get(a, ("missing", ""))[0] == s) for s in STATES}
    return "\n".join(rows), counts


def main(argv):
    if len(argv) < 3 or argv[1] not in ("table", "update", "check"):
        sys.exit(__doc__.strip().splitlines()[-1] + "\n\nusage: device-status.py table|update|check <device> [file]")

    command, device = argv[1], argv[2]
    body, counts = table(device)

    if command == "table":
        print(body)
        return 0

    if command == "check":
        print("%s: %d works, %d partial, %d missing" %
              (device, counts["works"], counts["partial"], counts["missing"]))
        return 0

    if len(argv) < 4:
        sys.exit("device-status.py: update needs the file to update")
    path = argv[3]
    begin = "<!-- status:%s begin -->" % device
    end = "<!-- status:%s end -->" % device
    try:
        with open(path, encoding="utf-8") as f:
            text = f.read()
    except OSError as e:
        sys.exit("device-status.py: %s: %s" % (path, e))

    if text.count(begin) != 1 or text.count(end) != 1:
        sys.exit("device-status.py: %s needs exactly one %r and one %r" % (path, begin, end))

    start = text.index(begin) + len(begin)
    stop = text.index(end)
    new = text[:start] + "\n" + body + "\n" + text[stop:]
    if new == text:
        print("%s: already up to date" % path)
        return 0
    with open(path, "w", encoding="utf-8") as f:
        f.write(new)
    print("%s: table replaced from devices/%s/status (%d works, %d partial, %d missing)" %
          (path, device, counts["works"], counts["partial"], counts["missing"]))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
