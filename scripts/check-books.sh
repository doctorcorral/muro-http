#!/usr/bin/env bash
# Type-check every unique book in muro.exs with the manifest fuel,
# then compare committed targets. This is the typecheck plus the
# byte comparison that `mix muro.gen --check` does, split so the
# books can be checked in one process each.
#
# One book at a time by default: Muro already checks the definitions
# in a book concurrently, and a book that imports a heavy book (conn
# imports request) already uses every core. Set MURO_CHECK_JOBS to
# overlap books.
set -u
set -o pipefail

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$root" || exit 1

fuel=$(python3 scripts/muro_books.py fuel) || exit 1
mapfile -t books < <(python3 scripts/muro_books.py books) || exit 1
if [ "${#books[@]}" -eq 0 ]; then
  echo "scripts/check-books.sh: no books" >&2
  exit 1
fi

jobs=${MURO_CHECK_JOBS:-1}
case "$jobs" in
  ''|*[!0-9]*|0)
    echo "MURO_CHECK_JOBS must be a positive integer" >&2
    exit 1
    ;;
esac

echo "type-checking ${#books[@]} books with fuel $fuel, $jobs at a time"

fail=0
run_one() {
  book=$1
  start=$(date +%s)
  echo "start $book"
  if mix muro.check --fuel "$fuel" "$book"; then
    end=$(date +%s)
    echo "ok $book $((end - start))s"
    return 0
  else
    end=$(date +%s)
    echo "fail $book $((end - start))s" >&2
    return 1
  fi
}

if [ "$jobs" -eq 1 ]; then
  for book in "${books[@]}"; do
    if ! run_one "$book"; then
      fail=1
    fi
  done
else
  # Overlap books. Failures do not stop the others, so every book
  # is reported. GNU and BSD xargs both accept -P and -n 1.
  export -f run_one
  export fuel
  if ! printf '%s\n' "${books[@]}" | xargs -P "$jobs" -n 1 -I{} bash -c 'run_one "$@"' _ {}; then
    fail=1
  fi
fi

if [ "$fail" -ne 0 ]; then
  echo "scripts/check-books.sh: a book did not check" >&2
  exit 1
fi

echo "comparing committed modules"
mix muro.gen --check --skip-check
