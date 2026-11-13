#ifndef DECISION_TAGS_H
#define DECISION_TAGS_H

/* Finite request-decision tags.
   Constructor order is the Muro data declaration order in src/decide.muro.
   The C emitter numbers constructors from 0 in that order.
   decide_hand.c and the generated core must use these same numbers.
   Reorder a data type only by editing decide.muro and this header together. */

#include <stdint.h>

enum {
  VERB_GET = 0,
  VERB_POST = 1,
  VERB_OTHER = 2,
  VERB_COUNT = 3
};

enum {
  PATH_HEALTH = 0,
  PATH_FEED = 1,
  PATH_POSTS = 2,
  PATH_POST_ID = 3,
  PATH_POST_LIKE = 4,
  PATH_OTHER = 5,
  PATH_COUNT = 6
};

enum {
  ID_NONE = 0,
  ID_BAD = 1,
  ID_UNKNOWN = 2,
  ID_MISSING = 3,
  ID_PRESENT = 4,
  ID_COUNT = 5
};

enum {
  AUTH_ABSENT = 0,
  AUTH_BAD_TOKEN = 1,
  AUTH_BAD_PAYLOAD = 2,
  AUTH_OK = 3,
  AUTH_COUNT = 4
};

enum {
  BODY_NONE = 0,
  BODY_MALFORMED = 1,
  BODY_REQUIRED = 2,
  BODY_TOO_LONG = 3,
  BODY_OK = 4,
  BODY_COUNT = 5
};

enum {
  LIKE_NONE = 0,
  LIKE_UNKNOWN = 1,
  LIKE_NEW = 2,
  LIKE_AGAIN = 3,
  LIKE_COUNT = 4
};

enum {
  DB_UNKNOWN = 0,
  DB_DOWN = 1,
  DB_OK = 2,
  DB_COUNT = 3
};

enum {
  ST_200 = 0,
  ST_201 = 1,
  ST_400 = 2,
  ST_401 = 3,
  ST_404 = 4,
  ST_500 = 5,
  ST_503 = 6,
  ST_OBSERVE = 7,
  ST_COUNT = 8
};

enum {
  ERR_NONE = 0,
  ERR_NOT_FOUND = 1,
  ERR_INVALID_ID = 2,
  ERR_POST_MISSING = 3,
  ERR_MISSING_BEARER = 4,
  ERR_BAD_TOKEN = 5,
  ERR_BAD_PAYLOAD = 6,
  ERR_MALFORMED = 7,
  ERR_BODY_REQUIRED = 8,
  ERR_BODY_LONG = 9,
  ERR_INTERNAL = 10,
  ERR_DB = 11,
  ERR_OBSERVE = 12,
  ERR_COUNT = 13
};

enum {
  ACT_HEALTH = 0,
  ACT_FEED = 1,
  ACT_READ = 2,
  ACT_CREATE = 3,
  ACT_LIKE_NEW = 4,
  ACT_LIKE_AGAIN = 5,
  ACT_ERROR = 6,
  ACT_OBSERVE = 7,
  ACT_COUNT = 8
};

/* In-range domain size: 3*6*5*4*5*4*3 = 21600. */
#define DECISION_COMBINATIONS 21600

typedef struct HostDecision {
  uint8_t status;
  uint8_t err;
  uint8_t action;
} HostDecision;

HostDecision decide_hand(int verb, int path, int idc, int auth, int body, int like, int db);
HostDecision decide_muro(int verb, int path, int idc, int auth, int body, int like, int db);
HostDecision host_decide(int verb, int path, int idc, int auth, int body, int like, int db);

#endif
