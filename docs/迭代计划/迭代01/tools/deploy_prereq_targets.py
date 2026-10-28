#!/usr/bin/env python3
"""Derive the targets that POST_BUILD deploy steps copy, so they can be built first.

WonderTrader's runner/porter targets copy sibling plugins (parsers/traders/factories)
in a POST_BUILD step without declaring add_dependencies on them, so a cold build can link
the host before the copied plugin exists and the copy fails (BUG-L00-001, 待审核). Building
these names as a pre-pass makes the copies succeed without touching any product CMakeLists.

The list is read out of the CMakeLists themselves — never typed here — so a newly added
deploy step is picked up automatically. Prints one target name per line on stdout;
unresolvable names and diagnostics go to stderr.
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

COPY_RE = re.compile(r"bin/\$\{PREFIX\}([A-Za-z0-9_]+)\$\{SUFFIX\}")
TARGET_RE = re.compile(r"ADD_(?:LIBRARY|EXECUTABLE)\s*\(?\s*([A-Za-z0-9_]+)", re.I)


def main() -> int:
    src = Path(sys.argv[1] if len(sys.argv) > 1 else "src").resolve()
    if not src.is_dir():
        print(f"error: {src} is not a directory", file=sys.stderr)
        return 2

    wanted: set[str] = set()
    hosts = 0
    for cmk in sorted(src.glob("*/CMakeLists.txt")):
        text = cmk.read_text(encoding="utf-8", errors="ignore")
        if "POST_BUILD" not in text:
            continue
        names = COPY_RE.findall(text)
        if not names:
            print(f"note: {cmk.name} in {cmk.parent.name} has POST_BUILD but no "
                  f"${{PREFIX}}...${{SUFFIX}} copy — check for another copy form",
                  file=sys.stderr)
            continue
        hosts += 1
        wanted.update(names)

    known: set[str] = set()
    for cmk in src.glob("*/CMakeLists.txt"):
        known.update(TARGET_RE.findall(cmk.read_text(encoding="utf-8", errors="ignore")))

    ghost = sorted(n for n in wanted if n not in known)
    for n in ghost:
        print(f"warning: copied artifact '{n}' matches no ADD_LIBRARY/ADD_EXECUTABLE "
              f"in src/*/CMakeLists.txt", file=sys.stderr)

    print(f"# hosts with deploy copies: {hosts}, artifacts to pre-build: "
          f"{len(wanted) - len(ghost)}", file=sys.stderr)
    for name in sorted(wanted - set(ghost)):
        print(name)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
