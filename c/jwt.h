#ifndef MURO_HTTP_JWT_H
#define MURO_HTTP_JWT_H

#include <stddef.h>
#include <stdint.h>

/* HS256 check of one Authorization header value.
   auth is AUTH_ABSENT, AUTH_BAD_TOKEN, AUTH_BAD_PAYLOAD, or AUTH_OK
   from decision_tags.h. An empty value is absent. A presented value
   that is not a Bearer token, or whose signature does not match, is
   bad-token. The key is the raw bytes, not base64. A missing key
   (key_n == 0) rejects every presented token and still reports an
   empty value as absent. now is unix seconds; a negative now rejects
   a token that carries exp or nbf. sub is the decoded subject, at
   most 128 bytes, and only when auth is AUTH_OK. */

typedef struct JwtResult {
  int auth;
  uint8_t sub[128];
  size_t sub_n;
} JwtResult;

void jwt_sha256(const uint8_t *msg, size_t n, uint8_t out[32]);
void jwt_hmac_sha256(const uint8_t *key, size_t key_n, const uint8_t *msg, size_t msg_n,
                     uint8_t out[32]);
void jwt_classify(const uint8_t *val, size_t n, const uint8_t *key, size_t key_n, int64_t now,
                  JwtResult *out);

#endif
