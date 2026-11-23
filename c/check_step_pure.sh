#!/bin/sh
# The protocol step must not call the operating system. Readiness,
# sockets, and the clock stay in the adapter.
set -e
cd "$(dirname "$0")/.."
pat='\<(read|write|accept|accept4|kqueue|kevent|epoll_wait|epoll_ctl|poll|fork|sendmsg|recvmsg|socket|connect|listen|bind|close|send|recv|fcntl|ioctl|select|pipe|dup|dup2|mmap|open|time|nanosleep|clock_gettime|gmtime_r|gmtime)\('
if grep -nE "$pat" c/step.c c/step.h; then
  echo "c/step.c calls the OS" >&2
  exit 1
fi
if grep -nE '#include <(unistd|sys/socket|sys/epoll|sys/event|poll|fcntl|sys/ioctl|sys/time|time)\.h>' c/step.c c/step.h; then
  echo "c/step.c includes an OS header" >&2
  exit 1
fi
exit 0
