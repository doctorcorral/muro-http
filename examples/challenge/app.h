#ifndef CHALLENGE_APP_H
#define CHALLENGE_APP_H

#include "muro_http.h"

#include <stddef.h>
#include <stdint.h>

/* The challenge routes on top of the HTTP/1.1 core. SQLite, the JWT
   key, and the JSON row live here. The handler returns a status, a
   header block, and a body. It does not frame the response. */

typedef struct Challenge Challenge;

Challenge *challenge_open(const char *db_path, const uint8_t *jwt_key, size_t jwt_key_len);
void challenge_close(Challenge *ch);
int challenge_handle(const MuroHttpRequest *req, MuroHttpResponse *resp, void *user);

#endif
