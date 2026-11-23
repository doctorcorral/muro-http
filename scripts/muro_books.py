#!/usr/bin/env python3
"""Read unique books and fuel from muro.exs, without starting Mix.

Commands:
  books   one book path per line, first-seen order from modules:
  json    a JSON array of those paths
  fuel    the manifest fuel as a decimal integer
"""

from __future__ import annotations

import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MANIFEST = ROOT / "muro.exs"


def load() -> tuple[int, list[str]]:
    text = MANIFEST.read_text()
    fuel_m = re.search(r"^\s*fuel:\s*([0-9_]+)\s*,?\s*$", text, re.M)
    if not fuel_m:
        raise SystemExit("muro.exs: no fuel:")
    fuel = int(fuel_m.group(1).replace("_", ""))
    # Only quoted book paths in the modules list. Neg-test books are
    # not listed in muro.exs and are not part of mix muro.gen --check.
    books: list[str] = []
    for m in re.finditer(r'"([^"]+\.muro)"', text):
        path = m.group(1)
        if path not in books:
            books.append(path)
    if not books:
        raise SystemExit("muro.exs: no books")
    return fuel, books


def main(argv: list[str]) -> int:
    cmd = argv[1] if len(argv) > 1 else "books"
    fuel, books = load()
    if cmd == "fuel":
        print(fuel)
    elif cmd == "json":
        print(json.dumps(books))
    elif cmd == "books":
        print("\n".join(books))
    else:
        raise SystemExit("usage: muro_books.py [books|json|fuel]")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
