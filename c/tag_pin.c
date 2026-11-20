/* decision_tags.h is a hand copy of constructor order. decide.h's
   *From helpers are the emitted map. This file checks one against
   the other. It does not choose a response. decide.h cannot be
   included next to conn.h (each generated header defines the prelude). */

#include "shim.h"
#include "decide.h"
#include "decision_tags.h"

#include <stdio.h>

static int pin(const char *name, unsigned got, unsigned want) {
  if (got == want) return 1;
  fprintf(stderr, "FAIL tag %s: emitted %u header %u\n", name, got, want);
  return 0;
}

int emitted_tags_match(void) {
  int ok = 1;
  ok &= pin("VERB_GET", verbFrom(VERB_GET)->muro__field_tag, VERB_GET);
  ok &= pin("VERB_POST", verbFrom(VERB_POST)->muro__field_tag, VERB_POST);
  ok &= pin("VERB_OTHER", verbFrom(VERB_OTHER)->muro__field_tag, VERB_OTHER);
  ok &= pin("PATH_HEALTH", pathFrom(PATH_HEALTH)->muro__field_tag, PATH_HEALTH);
  ok &= pin("PATH_FEED", pathFrom(PATH_FEED)->muro__field_tag, PATH_FEED);
  ok &= pin("PATH_POSTS", pathFrom(PATH_POSTS)->muro__field_tag, PATH_POSTS);
  ok &= pin("PATH_POST_ID", pathFrom(PATH_POST_ID)->muro__field_tag, PATH_POST_ID);
  ok &= pin("PATH_POST_LIKE", pathFrom(PATH_POST_LIKE)->muro__field_tag, PATH_POST_LIKE);
  ok &= pin("PATH_OTHER", pathFrom(PATH_OTHER)->muro__field_tag, PATH_OTHER);
  ok &= pin("ID_NONE", idFrom(ID_NONE)->muro__field_tag, ID_NONE);
  ok &= pin("ID_BAD", idFrom(ID_BAD)->muro__field_tag, ID_BAD);
  ok &= pin("ID_UNKNOWN", idFrom(ID_UNKNOWN)->muro__field_tag, ID_UNKNOWN);
  ok &= pin("ID_MISSING", idFrom(ID_MISSING)->muro__field_tag, ID_MISSING);
  ok &= pin("ID_PRESENT", idFrom(ID_PRESENT)->muro__field_tag, ID_PRESENT);
  ok &= pin("AUTH_ABSENT", authFrom(AUTH_ABSENT)->muro__field_tag, AUTH_ABSENT);
  ok &= pin("AUTH_BAD_TOKEN", authFrom(AUTH_BAD_TOKEN)->muro__field_tag, AUTH_BAD_TOKEN);
  ok &= pin("AUTH_BAD_PAYLOAD", authFrom(AUTH_BAD_PAYLOAD)->muro__field_tag, AUTH_BAD_PAYLOAD);
  ok &= pin("AUTH_OK", authFrom(AUTH_OK)->muro__field_tag, AUTH_OK);
  ok &= pin("BODY_NONE", bodyFrom(BODY_NONE)->muro__field_tag, BODY_NONE);
  ok &= pin("BODY_MALFORMED", bodyFrom(BODY_MALFORMED)->muro__field_tag, BODY_MALFORMED);
  ok &= pin("BODY_REQUIRED", bodyFrom(BODY_REQUIRED)->muro__field_tag, BODY_REQUIRED);
  ok &= pin("BODY_TOO_LONG", bodyFrom(BODY_TOO_LONG)->muro__field_tag, BODY_TOO_LONG);
  ok &= pin("BODY_OK", bodyFrom(BODY_OK)->muro__field_tag, BODY_OK);
  ok &= pin("LIKE_NONE", likeFrom(LIKE_NONE)->muro__field_tag, LIKE_NONE);
  ok &= pin("LIKE_UNKNOWN", likeFrom(LIKE_UNKNOWN)->muro__field_tag, LIKE_UNKNOWN);
  ok &= pin("LIKE_NEW", likeFrom(LIKE_NEW)->muro__field_tag, LIKE_NEW);
  ok &= pin("LIKE_AGAIN", likeFrom(LIKE_AGAIN)->muro__field_tag, LIKE_AGAIN);
  ok &= pin("DB_UNKNOWN", dbFrom(DB_UNKNOWN)->muro__field_tag, DB_UNKNOWN);
  ok &= pin("DB_DOWN", dbFrom(DB_DOWN)->muro__field_tag, DB_DOWN);
  ok &= pin("DB_OK", dbFrom(DB_OK)->muro__field_tag, DB_OK);
  muro_http_reset();
  return ok;
}
