#!/usr/bin/env python3
"""Print the CHANGELOG.md section for a version, for use as release notes.

    changelog_section.py 0.3.0 [CHANGELOG.md]
"""
import sys
from pathlib import Path

version = sys.argv[1].lstrip("v")
path = Path(sys.argv[2] if len(sys.argv) > 2 else "CHANGELOG.md")
out, capturing = [], False
for line in path.read_text().splitlines() if path.is_file() else []:
    if line.startswith("## "):
        if capturing:
            break
        capturing = line[3:].strip() == version
        continue
    if capturing:
        out.append(line)
print("\n".join(out).strip() or "See CHANGELOG.md.")
