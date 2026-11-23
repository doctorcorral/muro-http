#!/bin/sh
# The protocol step must not call the operating system. Readiness,
# sockets, and the clock stay in the adapter. The grep covers c/step.c,
# c/step.h, and the headers they include. The nm check is the undefined
# symbols in the compiled c/step.o.
set -e
cd "$(dirname "$0")/.."

funcs='read|write|accept|accept4|kqueue|kevent|epoll_wait|epoll_ctl|poll|ppoll|fork|sendmsg|recvmsg|socket|connect|listen|bind|close|send|recv|sendto|recvfrom|pread|pwrite|readv|writev|fcntl|ioctl|select|pselect|pipe|dup|dup2|mmap|open|time|nanosleep|clock_gettime|gmtime_r|gmtime|gettimeofday|usleep|sleep|syscall'
headers='unistd\.h|sys/socket\.h|sys/epoll\.h|sys/event\.h|poll\.h|fcntl\.h|sys/ioctl\.h|sys/time\.h|time\.h|sys/mman\.h|sys/select\.h|sys/uio\.h'

# libc the step may call. Sanitizer and stack-check symbols are
# prefixes after leading underscores are stripped. Every other
# undefined symbol must be defined in the books, the admit wall, or
# the render and stream helpers.
libc='memcpy memmove memset memcmp malloc free realloc calloc strlen bzero'
allow_prefix='asan_ ubsan_ sanitizer_ stack_chk chkstk'
objs='c/proto.o c/admit_glue.o c/host_render.o c/host_stream.o c/render.o c/conn.o c/scan_cache.o c/step_memo.o c/request.o c/shim.o c/session.o c/admit.o'

seen=$(mktemp)
queue=$(mktemp)
defined=$(mktemp)
miss=$(mktemp)
err=$(mktemp)
trap 'rm -f "$seen" "$queue" "$defined" "$miss" "$err"' EXIT
printf '%s\n' c/step.c c/step.h > "$queue"

n=0
while true; do
  n=$((n + 1))
  file=$(sed -n "${n}p" "$queue")
  [ -n "$file" ] || break
  if grep -qxF "$file" "$seen"; then
    continue
  fi
  echo "$file" >> "$seen"
  if [ ! -f "$file" ]; then
    echo "c/check_step_pure.sh: missing $file" >&2
    exit 1
  fi
  if grep -nE "^[[:space:]]*#[[:space:]]*include[[:space:]]*[<\"]($headers)[>\"]" "$file"; then
    echo "c/step includes an OS header via $file" >&2
    exit 1
  fi
  # poll(2) in a comment is a manual reference, not a call.
  if grep -nE "\\<($funcs)[[:space:]]*\\([^0-9]" "$file"; then
    echo "c/step calls the OS via $file" >&2
    exit 1
  fi
  grep -E '^[[:space:]]*#[[:space:]]*include[[:space:]]*"' "$file" > "$err" || true
  while IFS= read -r line; do
    [ -n "$line" ] || continue
    inc=$(printf '%s\n' "$line" | sed -n 's/.*"\([^"]*\)".*/\1/p')
    [ -n "$inc" ] || continue
    found=
    base=$(dirname "$file")
    for dir in "$base" c c/generated; do
      if [ -f "$dir/$inc" ]; then
        echo "$dir/$inc" >> "$queue"
        found=1
        break
      fi
    done
    if [ -z "$found" ]; then
      echo "c/check_step_pure.sh: cannot follow include $inc from $file" >&2
      exit 1
    fi
  done < "$err"
done

if [ ! -f c/step.o ]; then
  echo "c/step.o is missing; build it before the symbol check" >&2
  exit 1
fi

: > "$defined"
for obj in $objs; do
  if [ ! -f "$obj" ]; then
    echo "c/check_step_pure.sh: missing $obj" >&2
    exit 1
  fi
  # Last field is the symbol. Skip undefined (type U). Strip leading
  # underscores so macOS _memcpy and Linux memcpy match.
  nm -g "$obj" | awk '
    {
      sym = $NF
      kind = $(NF - 1)
      if (kind == "U" || $1 == "U") next
      sub(/^_+/, "", sym)
      if (sym != "") print sym
    }
  ' >> "$defined"
done

nm -u c/step.o | awk '
  {
    sym = $NF
    sub(/^_+/, "", sym)
    if (sym != "") print sym
  }
' > "$err"

while IFS= read -r sym; do
  [ -n "$sym" ] || continue
  ok=0
  for name in $libc; do
    if [ "$sym" = "$name" ]; then
      ok=1
      break
    fi
  done
  if [ "$ok" -eq 0 ]; then
    for pre in $allow_prefix; do
      case "$sym" in
        "$pre"*) ok=1 ;;
      esac
      [ "$ok" -eq 1 ] && break
    done
  fi
  if [ "$ok" -eq 0 ] && grep -qxF "$sym" "$defined"; then
    ok=1
  fi
  if [ "$ok" -eq 0 ]; then
    echo "$sym" >> "$miss"
  fi
done < "$err"

if [ -s "$miss" ]; then
  while IFS= read -r sym; do
    echo "c/step.o undefined symbol not allowed: $sym" >&2
  done < "$miss"
  exit 1
fi
exit 0
