/* SQLite rows for the challenge routes. A failed open is a NULL
   store, and every query then reports down. This file does not
   choose a status. */

#include "store.h"
#include "json_text.h"

#include <sqlite3.h>
#include <stdlib.h>
#include <string.h>

struct Store {
  sqlite3 *db;
};

static const char *schema =
    "CREATE TABLE IF NOT EXISTS posts ("
    "id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "text TEXT NOT NULL);"
    "CREATE TABLE IF NOT EXISTS likes ("
    "post_id INTEGER NOT NULL,"
    "sub TEXT NOT NULL,"
    "PRIMARY KEY (post_id, sub));";

Store *store_open(const char *path) {
  sqlite3 *db = NULL;
  Store *st;
  int rc;
  if (!path || !path[0]) return NULL;
  rc = sqlite3_open_v2(path, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX,
                       NULL);
  if (rc != SQLITE_OK) {
    if (db) sqlite3_close(db);
    return NULL;
  }
  rc = sqlite3_exec(db, schema, NULL, NULL, NULL);
  if (rc != SQLITE_OK) {
    sqlite3_close(db);
    return NULL;
  }
  st = calloc(1, sizeof *st);
  if (!st) {
    sqlite3_close(db);
    return NULL;
  }
  st->db = db;
  return st;
}

void store_close(Store *st) {
  if (!st) return;
  sqlite3_close(st->db);
  free(st);
}

int store_ping(Store *st) {
  sqlite3_stmt *q = NULL;
  int rc;
  if (!st) return 0;
  if (sqlite3_prepare_v2(st->db, "SELECT 1", -1, &q, NULL) != SQLITE_OK) return 0;
  rc = sqlite3_step(q);
  sqlite3_finalize(q);
  return rc == SQLITE_ROW;
}

int store_feed(Store *st, uint8_t *dst, size_t cap, size_t *out_n) {
  sqlite3_stmt *q = NULL;
  size_t n = 0;
  int first = 1;
  int rc;
  if (!out_n) return -1;
  *out_n = 0;
  if (!st) return -1;
  if (sqlite3_prepare_v2(st->db, "SELECT id, text FROM posts ORDER BY id ASC", -1, &q, NULL) !=
      SQLITE_OK)
    return -1;
  if (json_append_raw(dst, cap, &n, "[", 1) < 0) {
    sqlite3_finalize(q);
    return -2;
  }
  for (;;) {
    int64_t id;
    const uint8_t *text;
    int tn;
    rc = sqlite3_step(q);
    if (rc == SQLITE_DONE) break;
    if (rc != SQLITE_ROW) {
      sqlite3_finalize(q);
      return -1;
    }
    id = sqlite3_column_int64(q, 0);
    text = sqlite3_column_blob(q, 1);
    tn = sqlite3_column_bytes(q, 1);
    if (tn < 0) {
      sqlite3_finalize(q);
      return -1;
    }
    if (!first && json_append_raw(dst, cap, &n, ",", 1) < 0) {
      sqlite3_finalize(q);
      return -2;
    }
    first = 0;
    if (json_row_post(dst, cap, &n, id, text, (size_t)tn) < 0) {
      sqlite3_finalize(q);
      return -2;
    }
  }
  sqlite3_finalize(q);
  if (json_append_raw(dst, cap, &n, "]", 1) < 0) return -2;
  *out_n = n;
  return 0;
}

int store_create(Store *st, const uint8_t *text, size_t n, int64_t *id) {
  sqlite3_stmt *q = NULL;
  int rc;
  if (!st || !id || !text || n > 2147483647u) return -1;
  if (sqlite3_prepare_v2(st->db, "INSERT INTO posts(text) VALUES (?1)", -1, &q, NULL) != SQLITE_OK)
    return -1;
  if (sqlite3_bind_text(q, 1, (const char *)text, (int)n, SQLITE_TRANSIENT) != SQLITE_OK) {
    sqlite3_finalize(q);
    return -1;
  }
  rc = sqlite3_step(q);
  sqlite3_finalize(q);
  if (rc != SQLITE_DONE) return -1;
  *id = sqlite3_last_insert_rowid(st->db);
  return 0;
}

int store_read(Store *st, int64_t id, uint8_t *text, size_t cap, size_t *out_n) {
  sqlite3_stmt *q = NULL;
  int rc;
  int tn;
  const uint8_t *p;
  if (!out_n) return -1;
  *out_n = 0;
  if (!st) return -1;
  if (sqlite3_prepare_v2(st->db, "SELECT text FROM posts WHERE id = ?1", -1, &q, NULL) != SQLITE_OK)
    return -1;
  if (sqlite3_bind_int64(q, 1, id) != SQLITE_OK) {
    sqlite3_finalize(q);
    return -1;
  }
  rc = sqlite3_step(q);
  if (rc == SQLITE_DONE) {
    sqlite3_finalize(q);
    return 0;
  }
  if (rc != SQLITE_ROW) {
    sqlite3_finalize(q);
    return -1;
  }
  tn = sqlite3_column_bytes(q, 0);
  p = sqlite3_column_blob(q, 0);
  if (tn < 0) {
    sqlite3_finalize(q);
    return -1;
  }
  if ((size_t)tn > cap) {
    sqlite3_finalize(q);
    return -2;
  }
  if (tn && p && text) memcpy(text, p, (size_t)tn);
  *out_n = (size_t)tn;
  sqlite3_finalize(q);
  return 1;
}

static void rollback(sqlite3 *db) { sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL); }

int store_like(Store *st, int64_t id, const uint8_t *sub, size_t sub_n, int64_t *likes) {
  sqlite3_stmt *sel = NULL;
  sqlite3_stmt *ins = NULL;
  sqlite3_stmt *cnt = NULL;
  int rc;
  int again = 0;
  if (!st || !likes || !sub || sub_n == 0 || sub_n > 2147483647u) return -1;
  if (sqlite3_exec(st->db, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK) return -1;
  if (sqlite3_prepare_v2(st->db, "SELECT 1 FROM posts WHERE id = ?1", -1, &sel, NULL) != SQLITE_OK)
    goto down;
  if (sqlite3_bind_int64(sel, 1, id) != SQLITE_OK) goto down;
  rc = sqlite3_step(sel);
  sqlite3_finalize(sel);
  sel = NULL;
  if (rc == SQLITE_DONE) {
    rollback(st->db);
    return 0;
  }
  if (rc != SQLITE_ROW) goto down;
  if (sqlite3_prepare_v2(st->db, "INSERT INTO likes(post_id, sub) VALUES (?1, ?2)", -1, &ins,
                         NULL) != SQLITE_OK)
    goto down;
  if (sqlite3_bind_int64(ins, 1, id) != SQLITE_OK) goto down;
  if (sqlite3_bind_text(ins, 2, (const char *)sub, (int)sub_n, SQLITE_TRANSIENT) != SQLITE_OK)
    goto down;
  rc = sqlite3_step(ins);
  sqlite3_finalize(ins);
  ins = NULL;
  if (rc != SQLITE_DONE) {
    int err = sqlite3_errcode(st->db) & 0xff;
    if (rc != SQLITE_CONSTRAINT && err != SQLITE_CONSTRAINT) goto down;
    again = 1;
  }
  if (sqlite3_prepare_v2(st->db, "SELECT COUNT(*) FROM likes WHERE post_id = ?1", -1, &cnt, NULL) !=
      SQLITE_OK)
    goto down;
  if (sqlite3_bind_int64(cnt, 1, id) != SQLITE_OK) goto down;
  rc = sqlite3_step(cnt);
  if (rc != SQLITE_ROW) goto down;
  *likes = sqlite3_column_int64(cnt, 0);
  sqlite3_finalize(cnt);
  cnt = NULL;
  if (sqlite3_exec(st->db, "COMMIT", NULL, NULL, NULL) != SQLITE_OK) goto down;
  return again ? 2 : 1;
down:
  if (sel) sqlite3_finalize(sel);
  if (ins) sqlite3_finalize(ins);
  if (cnt) sqlite3_finalize(cnt);
  rollback(st->db);
  return -1;
}
