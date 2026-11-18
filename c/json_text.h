#ifndef MURO_HTTP_JSON_TEXT_H
#define MURO_HTTP_JSON_TEXT_H

#include <stddef.h>
#include <stdint.h>

/* Host bytes for a JSON string interior and for one row.
   The body class itself is the emitted bodyClass. These functions
   do not choose a status. unescape returns 0 or -1. utf8 returns 1
   when the bytes are well-formed UTF-8. Append helpers return 0, or
   -1 when the destination cannot hold the next byte. On -1, *n is
   unchanged. */

int json_unescape(const uint8_t *src, size_t n, uint8_t *dst, size_t cap, size_t *out_n);
int json_utf8(const uint8_t *s, size_t n);

int json_append_raw(uint8_t *dst, size_t cap, size_t *n, const void *src, size_t sn);
int json_append_i64(uint8_t *dst, size_t cap, size_t *n, int64_t v);
int json_append_str(uint8_t *dst, size_t cap, size_t *n, const uint8_t *src, size_t sn);
int json_row_post(uint8_t *dst, size_t cap, size_t *n, int64_t id, const uint8_t *text,
                  size_t text_n);
int json_row_like(uint8_t *dst, size_t cap, size_t *n, int64_t id, int64_t likes);

#endif
