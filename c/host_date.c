#include "host_date.h"

#include <stdio.h>
#include <string.h>

static const char *const k_wday[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
static const char *const k_mon[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

int host_date_format(time_t sec, char out[30]) {
  struct tm tm;
  int n;
  if (!out || gmtime_r(&sec, &tm) == 0) return -1;
  if (tm.tm_wday < 0 || tm.tm_wday > 6 || tm.tm_mon < 0 || tm.tm_mon > 11) return -1;
  n = snprintf(out, 30, "%s, %02d %s %04d %02d:%02d:%02d GMT", k_wday[tm.tm_wday], tm.tm_mday,
               k_mon[tm.tm_mon], tm.tm_year + 1900, tm.tm_hour, tm.tm_min, tm.tm_sec);
  if (n != 29) return -1;
  return 0;
}

int host_date_now(char out[30]) {
  static time_t cached = (time_t)-1;
  static char buf[30];
  time_t now;
  if (!out) return -1;
  now = time(NULL);
  if (cached == (time_t)-1 || now != cached || buf[0] == 0) {
    if (host_date_format(now, buf) < 0) return -1;
    cached = now;
  }
  memcpy(out, buf, 30);
  return 0;
}

void host_date_fill(char out[30]) {
  if (!out) return;
  if (host_date_now(out) != 0) memcpy(out, "Thu, 01 Jan 1970 00:00:00 GMT", 30);
}
