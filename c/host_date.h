#ifndef MURO_HTTP_HOST_DATE_H
#define MURO_HTTP_HOST_DATE_H

#include <time.h>

/* IMF-fixdate, 29 bytes plus a NUL. English names, UTC. Not the
   locale's strftime. Returns 0, or -1 if the clock cannot be read.
   The book places these bytes. It does not check their shape. */

int host_date_format(time_t sec, char out[30]);

/* Cached for the current time(NULL) second. */
int host_date_now(char out[30]);

/* host_date_now, or Thu, 01 Jan 1970 00:00:00 GMT if the clock fails.
   out is always 29 bytes and a NUL. */
void host_date_fill(char out[30]);

#endif
