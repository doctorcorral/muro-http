#ifndef MURO_HTTP_STORE_H
#define MURO_HTTP_STORE_H

#include <stddef.h>
#include <stdint.h>

/* The store is the swap boundary. decide never sees SQLite.
   The host asks for rows. It does not choose a response.
   open sets journal_mode=WAL and synchronous=NORMAL (1), reads
   both back, and sets a 5000 ms busy timeout. A file database
   whose journal mode is not wal, or whose synchronous value is
   below NORMAL, is not opened. :memory: cannot be WAL. It stays
   journal mode memory and is for tests. It is not durable.
   With WAL and synchronous=NORMAL, a commit survives a process
   crash and may be lost on power loss. The reopen tests prove a
   clean close and reopen, not crash or power-loss durability.
   A write returns only after that commit: create returns 0, like
   returns 1 or 2. The row JSON is rendered inside that transaction.
   If the row does not fit, the store rolls back and returns -2.
   ping: 1 up, 0 down.
   feed: 0 ok, -1 query failed, -2 the row does not fit.
   create: 0 ok, -1 failed, -2 the row does not fit.
   read: 1 present, 0 missing, -1 failed, -2 the text does not fit.
   like: 1 inserted, 2 already present, 0 no such post, -1 failed,
   -2 the row does not fit.
   store_durability reads journal_mode, synchronous, and the busy
   timeout on this connection. It is not a fact for decide.
   It returns 0, or -1. */

typedef struct Store Store;

Store *store_open(const char *path);
void store_close(Store *st);
int store_durability(Store *st, char *journal, size_t cap, int *sync_out, int *busy_ms);

int store_ping(Store *st);
int store_feed(Store *st, uint8_t *dst, size_t cap, size_t *out_n);
int store_create(Store *st, const uint8_t *text, size_t n, int64_t *id, uint8_t *row,
                 size_t row_cap, size_t *row_n);
int store_read(Store *st, int64_t id, uint8_t *text, size_t cap, size_t *out_n);
int store_like(Store *st, int64_t id, const uint8_t *sub, size_t sub_n, int64_t *likes,
               uint8_t *row, size_t row_cap, size_t *row_n);

#endif
