#ifndef MURO_HTTP_STORE_H
#define MURO_HTTP_STORE_H

#include <stddef.h>
#include <stdint.h>

/* One SQLite connection. NULL means the database is down.
   The host asks for rows. It does not choose a response.
   ping: 1 up, 0 down.
   feed and the row encoders: 0 ok, -1 query failed, -2 the row
   does not fit in dst.
   create: 0 ok, -1 failed.
   read: 1 present, 0 missing, -1 failed, -2 the text does not fit.
   like: 1 inserted, 2 already present, 0 no such post, -1 failed. */

typedef struct Store Store;

Store *store_open(const char *path);
void store_close(Store *st);

int store_ping(Store *st);
int store_feed(Store *st, uint8_t *dst, size_t cap, size_t *out_n);
int store_create(Store *st, const uint8_t *text, size_t n, int64_t *id);
int store_read(Store *st, int64_t id, uint8_t *text, size_t cap, size_t *out_n);
int store_like(Store *st, int64_t id, const uint8_t *sub, size_t sub_n, int64_t *likes);

#endif
