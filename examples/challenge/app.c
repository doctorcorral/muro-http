/* Challenge handler. One request becomes verb, path, id, auth, and
   body facts, then decide. An observe act runs one SQLite query and
   decide runs again. A third observe closes the connection. The
   status, the header block, and the body are the handler's. The
   core frames them. */

#include "shim.h"
#include "decide.h"
#include "app.h"
#include "route_glue.h"
#include "host_body.h"
#include "decision_tags.h"
#include "jwt.h"
#include "json_text.h"
#include "store.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum { ROW_CAP = 65536 };

static const char k_text[] = "Content-Type: text/plain; charset=utf-8\r\n";
static const char k_json[] = "Content-Type: application/json; charset=utf-8\r\n";

struct Challenge {
  Store *store;
  uint8_t key[256];
  size_t key_n;
  uint8_t *row;
  size_t row_cap;
  size_t row_len;
  int row_ok;
  int committed;
  int fail_admit;
  uint64_t verb;
  uint64_t path;
  uint64_t idc;
  int64_t idn;
  uint64_t auth;
  uint64_t body;
  uint64_t like;
  uint64_t db;
  uint8_t sub[128];
  size_t sub_n;
  uint8_t text[1024];
  size_t text_n;
  int text_ok;
};

static int env_fail_db_admit(void) {
  const char *e = getenv("MURO_TEST_FAIL_DB_ADMIT");
  char *end = NULL;
  unsigned long n;
  if (!e || !*e) return 0;
  errno = 0;
  n = strtoul(e, &end, 10);
  if (errno || end == e || *end || n > 1000ul) return 0;
  return (int)n;
}

Challenge *challenge_open(const char *db_path, const uint8_t *jwt_key, size_t jwt_key_len) {
  Challenge *ch;
  if (jwt_key_len > 256) return NULL;
  ch = calloc(1, sizeof *ch);
  if (!ch) return NULL;
  ch->row = malloc(ROW_CAP);
  if (!ch->row) {
    free(ch);
    return NULL;
  }
  ch->row_cap = ROW_CAP;
  ch->fail_admit = env_fail_db_admit();
  if (jwt_key && jwt_key_len) {
    memcpy(ch->key, jwt_key, jwt_key_len);
    ch->key_n = jwt_key_len;
  }
  ch->store = store_open(db_path && db_path[0] ? db_path : ":memory:");
  if (!ch->store) fprintf(stderr, "muro-httpd: database is unavailable\n");
  return ch;
}

void challenge_close(Challenge *ch) {
  if (!ch) return;
  store_close(ch->store);
  free(ch->row);
  free(ch);
}

static int take_request(Challenge *ch, const MuroHttpRequest *req) {
  MuroHttpSlice auth;
  ch->verb = challenge_verb(req->method.p, req->method.n);
  ch->path = challenge_path(req->path.p, req->path.n);
  ch->idc = challenge_id(req->path.p, req->path.n);
  ch->idn = challenge_idn(req->path.p, req->path.n);
  muro_http_reset();
  ch->auth = AUTH_ABSENT;
  ch->sub_n = 0;
  if (muro_http_header_find(req, "authorization", &auth)) {
    if (auth.n > 16384) {
      ch->auth = AUTH_BAD_TOKEN;
    } else if (auth.n == 0) {
      ch->auth = AUTH_ABSENT;
    } else {
      uint8_t buf[16384];
      JwtResult jr;
      memcpy(buf, auth.p, auth.n);
      muro_http_reset();
      jwt_classify(buf, auth.n, ch->key, ch->key_n, (int64_t)time(NULL), &jr);
      ch->auth = (uint64_t)jr.auth;
      ch->sub_n = jr.sub_n <= sizeof ch->sub ? jr.sub_n : 0;
      if (ch->sub_n) memcpy(ch->sub, jr.sub, ch->sub_n);
    }
  }
  muro_http_reset();
  ch->body = BODY_NONE;
  ch->text_ok = 0;
  ch->text_n = 0;
  if (ch->verb == VERB_POST && ch->path == PATH_POSTS) {
    uint64_t cls = host_body_class(req->body.p, req->body.n);
    if (cls == BODY_OK) {
      uint64_t off = host_text_off(req->body.p, req->body.n);
      uint64_t len = host_text_len(req->body.p, req->body.n);
      size_t tn = 0;
      if (off > (uint64_t)req->body.n || len > (uint64_t)req->body.n - off ||
          json_unescape(req->body.p + (size_t)off, (size_t)len, ch->text, sizeof ch->text, &tn) !=
              0 ||
          !json_utf8(ch->text, tn)) {
        cls = BODY_MALFORMED;
      } else {
        ch->text_n = tn;
        ch->text_ok = 1;
      }
    }
    ch->body = cls;
  }
  muro_http_reset();
  ch->like = LIKE_UNKNOWN;
  ch->db = DB_UNKNOWN;
  ch->row_ok = 0;
  ch->row_len = 0;
  ch->committed = 0;
  return 0;
}

/* One query after decide returns act-observe. The lookup and the like
   write are one step, so id-present is not paired with db-down.
   Create and like render the row inside the store transaction and
   commit only when that row fits. A fit failure rolls back and this
   function reports db-down, so decide answers 500.
   Returns -1 when a read row does not fit, or when the route has
   nothing left to learn. That closes the socket. It is not a status.
   POST /posts reaches here only for body-ok, which stored the text.
   A missing text is a host bug and closes the socket. */
static int query_facts(Challenge *ch) {
  int rc;
  ch->row_ok = 0;
  ch->row_len = 0;
  if (ch->verb == VERB_GET && ch->path == PATH_HEALTH) {
    ch->db = store_ping(ch->store) ? DB_OK : DB_DOWN;
    return 0;
  }
  if (ch->verb == VERB_GET && ch->path == PATH_FEED) {
    rc = store_feed(ch->store, ch->row, ch->row_cap, &ch->row_len);
    if (rc == -2) return -1;
    if (rc != 0) {
      ch->db = DB_DOWN;
      return 0;
    }
    ch->db = DB_OK;
    ch->row_ok = 1;
    return 0;
  }
  if (ch->verb == VERB_POST && ch->path == PATH_POSTS) {
    int64_t id = 0;
    size_t n = 0;
    if (!ch->text_ok) {
      fprintf(stderr, "muro-httpd: create observed without text\n");
      return -1;
    }
    rc = store_create(ch->store, ch->text, ch->text_n, &id, ch->row, ch->row_cap, &n);
    if (rc != 0) {
      ch->db = DB_DOWN;
      return 0;
    }
    ch->row_len = n;
    ch->row_ok = 1;
    ch->db = DB_OK;
    ch->committed = 1;
    return 0;
  }
  if (ch->verb == VERB_GET && ch->path == PATH_POST_ID) {
    uint8_t text[1024];
    size_t tn = 0;
    size_t n = 0;
    if (ch->idc != ID_UNKNOWN) return -1;
    rc = store_read(ch->store, ch->idn, text, sizeof text, &tn);
    if (rc == -2) return -1;
    if (rc < 0) {
      ch->db = DB_DOWN;
      return 0;
    }
    if (rc == 0) {
      ch->db = DB_OK;
      ch->idc = ID_MISSING;
      return 0;
    }
    if (json_row_post(ch->row, ch->row_cap, &n, ch->idn, text, tn) != 0) return -1;
    ch->row_len = n;
    ch->row_ok = 1;
    ch->db = DB_OK;
    ch->idc = ID_PRESENT;
    return 0;
  }
  if (ch->verb == VERB_POST && ch->path == PATH_POST_LIKE) {
    int64_t likes = 0;
    size_t n = 0;
    if (ch->idc != ID_UNKNOWN || ch->auth != AUTH_OK || ch->sub_n == 0) return -1;
    rc = store_like(ch->store, ch->idn, ch->sub, ch->sub_n, &likes, ch->row, ch->row_cap, &n);
    if (rc < 0) {
      ch->db = DB_DOWN;
      return 0;
    }
    if (rc == 0) {
      ch->db = DB_OK;
      ch->idc = ID_MISSING;
      return 0;
    }
    ch->row_len = n;
    ch->row_ok = 1;
    ch->db = DB_OK;
    ch->idc = ID_PRESENT;
    ch->like = rc == 1 ? LIKE_NEW : LIKE_AGAIN;
    ch->committed = 1;
    return 0;
  }
  return -1;
}

static int run_decide(Challenge *ch, int *code, int *err, int *act) {
  Decision *d = decide(verbFrom(ch->verb), pathFrom(ch->path), idFrom(ch->idc), authFrom(ch->auth),
                       bodyFrom(ch->body), likeFrom(ch->like), dbFrom(ch->db));
  if (!d || d->muro__field_tag != 0 || !d->muro__as.decided.muro__f_0 ||
      !d->muro__as.decided.muro__f_1 || !d->muro__as.decided.muro__f_2) {
    muro_http_reset();
    return -1;
  }
  *code = d->muro__as.decided.muro__f_0->muro__field_tag;
  *err = d->muro__as.decided.muro__f_1->muro__field_tag;
  *act = d->muro__as.decided.muro__f_2->muro__field_tag;
  muro_http_reset();
  return 0;
}

static void set_status(MuroHttpResponse *resp, int code) {
  switch (code) {
  case ST_200:
    resp->status = 200;
    resp->reason = "OK";
    break;
  case ST_201:
    resp->status = 201;
    resp->reason = "Created";
    break;
  case ST_400:
    resp->status = 400;
    resp->reason = "Bad Request";
    break;
  case ST_401:
    resp->status = 401;
    resp->reason = "Unauthorized";
    break;
  case ST_404:
    resp->status = 404;
    resp->reason = "Not Found";
    break;
  case ST_500:
    resp->status = 500;
    resp->reason = "Internal Server Error";
    break;
  case ST_503:
    resp->status = 503;
    resp->reason = "Service Unavailable";
    break;
  default:
    resp->status = 500;
    resp->reason = "Internal Server Error";
    break;
  }
  resp->reason_len = strlen(resp->reason);
}

static const char *err_text(int err) {
  switch (err) {
  case ERR_NOT_FOUND: return "not-found";
  case ERR_INVALID_ID: return "invalid-id";
  case ERR_POST_MISSING: return "post-missing";
  case ERR_MISSING_BEARER: return "missing-bearer";
  case ERR_BAD_TOKEN: return "bad-token";
  case ERR_BAD_PAYLOAD: return "bad-payload";
  case ERR_MALFORMED: return "malformed";
  case ERR_BODY_REQUIRED: return "body-required";
  case ERR_BODY_LONG: return "body-long";
  case ERR_INTERNAL: return "internal";
  case ERR_DB: return "db";
  default: return "internal";
  }
}

static void plain(MuroHttpResponse *resp, const char *text) {
  resp->headers = k_text;
  resp->headers_len = sizeof k_text - 1;
  resp->body = (const uint8_t *)text;
  resp->body_len = strlen(text);
}

static int fill_response(Challenge *ch, MuroHttpResponse *resp, int code, int err, int act) {
  if (ch->fail_admit > 0 && ch->committed) {
    ch->fail_admit -= 1;
    fprintf(stderr, "muro-httpd: test hook refused db admit\n");
    set_status(resp, ST_500);
    plain(resp, "internal");
    resp->close_after = 1;
    resp->owe_status = 1;
    return 0;
  }
  set_status(resp, code);
  if (act == ACT_HEALTH) {
    plain(resp, "ok");
  } else if (act >= ACT_FEED && act <= ACT_LIKE_AGAIN) {
    if (!ch->row_ok || !ch->row) return -1;
    resp->headers = k_json;
    resp->headers_len = sizeof k_json - 1;
    resp->body = ch->row;
    resp->body_len = ch->row_len;
  } else if (act == ACT_ERROR) {
    plain(resp, err_text(err));
  } else {
    return -1;
  }
  if (ch->committed) resp->owe_status = 1;
  return 0;
}

int challenge_handle(const MuroHttpRequest *req, MuroHttpResponse *resp, void *user) {
  Challenge *ch = user;
  int code = 0;
  int err = 0;
  int act = 0;
  int queried = 0;
  if (!ch || !req || !resp) return -1;
  if (take_request(ch, req) < 0) return -1;
  for (;;) {
    if (run_decide(ch, &code, &err, &act) < 0) return -1;
    if (act != ACT_OBSERVE) break;
    if (queried) return -1;
    queried = 1;
    if (query_facts(ch) < 0) {
      fprintf(stderr, "muro-httpd: query closed the socket\n");
      return -1;
    }
  }
  return fill_response(ch, resp, code, err, act);
}
