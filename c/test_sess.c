/* Drive the session stepper emitted from priv/muro/http/session.muro.

   With no arguments, check legal traces, refused off-protocol events,
   and a wrong key, a bad position, and an unknown name.

   With a script path, print one block per init, set, or move:

     status <n>
     done <0|1>
     step <label>
     expected <text>
     .

   `done` is muro__sess_done of the stored state. The state changes
   only when the status is MURO_SESS_OK. A position that is not a
   step leaves the result's own done field 0, so the host query is
   what this driver prints. */
#include "session.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

static void bad(const char *what) {
  fprintf(stderr, "FAIL %s\n", what);
  failures += 1;
}

static int parse_u(const char *s, unsigned long *out) {
  char *end = NULL;
  unsigned long n;
  if (!s || !*s) return 0;
  n = strtoul(s, &end, 10);
  if (!end || *end) return 0;
  *out = n;
  return 1;
}

static void emit_block(int status, int done, const char *step, const char *expected) {
  printf("status %d\n", status);
  printf("done %d\n", done);
  printf("step %s\n", step ? step : "");
  printf("expected %s\n", expected ? expected : "");
  printf(".\n");
}

static muro__sess_res do_step(const char *name, muro__sess_st *st, int dir, uint8_t tag,
                              unsigned bit, const char *ctor, uint64_t nat) {
  muro__sess_val v;
  muro__sess_res res;
  memset(&v, 0, sizeof v);
  v.tag = tag;
  v.bit = (uint8_t)bit;
  v.ctor = ctor;
  v.nat = nat;
  res = muro__sess_step(name, *st, dir, &v);
  if (res.status == MURO_SESS_OK) *st = res.state;
  return res;
}

static void query(const char *name, muro__sess_st st, int status) {
  char expected[1024];
  muro__sess_expected(name, st, expected, sizeof expected);
  emit_block(status, muro__sess_done(name, st), muro__sess_at(name, st), expected);
}

/* Returns 0 on a bad line. */
static int apply_line(char *line, char *name, int *have, muro__sess_st *st) {
  char *tok;
  char *dir;
  char *kind;
  char *arg;
  unsigned long n;
  int which;
  muro__sess_res res;
  muro__sess_st cur;

  tok = strtok(line, " \t");
  if (!tok) return 0;
  if (strcmp(tok, "name") == 0) {
    tok = strtok(NULL, " \t");
    if (!tok || strtok(NULL, " \t")) return 0;
    if (strlen(tok) >= 64) return 0;
    memcpy(name, tok, strlen(tok) + 1);
    return 1;
  }
  if (strcmp(tok, "init") == 0) {
    if (strtok(NULL, " \t")) return 0;
    res = muro__sess_init(name, NULL, 0);
    if (res.status == MURO_SESS_OK) {
      *st = res.state;
      *have = 1;
      emit_block(res.status, muro__sess_done(name, *st), res.step, res.expected);
    } else {
      *have = 0;
      emit_block(res.status, 0, res.step, res.expected);
    }
    return 1;
  }
  if (strcmp(tok, "set") == 0) {
    kind = strtok(NULL, " \t");
    arg = strtok(NULL, " \t");
    if (!kind || !arg || strtok(NULL, " \t")) return 0;
    if (!parse_u(arg, &n) || n > 65535) return 0;
    if (!*have) {
      memset(st, 0, sizeof *st);
      *have = 1;
    }
    if (strcmp(kind, "key") == 0) st->key = (uint16_t)n;
    else if (strcmp(kind, "at") == 0) st->at = (uint16_t)n;
    else return 0;
    query(name, *st, MURO_SESS_OK);
    return 1;
  }
  dir = tok;
  if (strcmp(dir, "send") == 0) which = MURO_SESS_SEND;
  else if (strcmp(dir, "recv") == 0) which = MURO_SESS_RECV;
  else return 0;
  kind = strtok(NULL, " \t");
  arg = strtok(NULL, " \t");
  if (!kind || !arg || strtok(NULL, " \t")) return 0;
  if (!*have) memset(&cur, 0, sizeof cur);
  else cur = *st;
  if (strcmp(kind, "pick") == 0) {
    if (!parse_u(arg, &n) || n > 255) return 0;
    res = do_step(name, &cur, which, MURO_SESS_PICK, (unsigned)n, NULL, 0);
  } else if (strcmp(kind, "data") == 0) {
    res = do_step(name, &cur, which, MURO_SESS_DATA, 0, arg, 0);
  } else if (strcmp(kind, "nat") == 0) {
    if (!parse_u(arg, &n)) return 0;
    res = do_step(name, &cur, which, MURO_SESS_NAT, 0, NULL, (uint64_t)n);
  } else {
    return 0;
  }
  if (res.status == MURO_SESS_OK) {
    *st = cur;
    *have = 1;
    emit_block(res.status, muro__sess_done(name, *st), res.step, res.expected);
  } else {
    emit_block(res.status, muro__sess_done(name, cur), res.step, res.expected);
  }
  return 1;
}

static int run_script(const char *path) {
  FILE *fp = fopen(path, "r");
  char line[512];
  char name[64];
  muro__sess_st st;
  int have = 0;
  int n = 0;
  if (!fp) {
    fprintf(stderr, "FAIL cannot open %s\n", path);
    return 1;
  }
  name[0] = 0;
  memset(&st, 0, sizeof st);
  while (fgets(line, sizeof line, fp)) {
    char *p = line;
    size_t len = strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = 0;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == 0 || *p == '#') continue;
    n += 1;
    if (!apply_line(p, name, &have, &st)) {
      fprintf(stderr, "FAIL bad line %d\n", n);
      fclose(fp);
      return 1;
    }
  }
  fclose(fp);
  return 0;
}

struct Move {
  const char *dir;
  const char *kind;
  const char *arg;
};

static int play(const char *name, muro__sess_st *st, const struct Move *mv, int n) {
  int i;
  for (i = 0; i < n; i++) {
    int dir = strcmp(mv[i].dir, "send") == 0 ? MURO_SESS_SEND : MURO_SESS_RECV;
    muro__sess_res res;
    unsigned long bit = 0;
    if (strcmp(mv[i].kind, "pick") == 0) {
      if (!parse_u(mv[i].arg, &bit) || bit > 255) return 0;
      res = do_step(name, st, dir, MURO_SESS_PICK, (unsigned)bit, NULL, 0);
    } else if (strcmp(mv[i].kind, "data") == 0) {
      res = do_step(name, st, dir, MURO_SESS_DATA, 0, mv[i].arg, 0);
    } else {
      return 0;
    }
    if (res.status != MURO_SESS_OK) {
      fprintf(stderr, "FAIL %s move %d status %d expected %s\n", name, i, res.status,
              res.expected);
      return 0;
    }
  }
  return 1;
}

static void self_check(void) {
  muro__sess_res res;
  muro__sess_st st;
  muro__sess_st saved;
  char expected[64];
  const struct Move two[] = {
      {"send", "pick", "0"}, {"recv", "pick", "0"}, {"recv", "data", "piece-req"},
      {"send", "data", "reply-ok"}, {"send", "pick", "0"}, {"send", "pick", "0"},
      {"recv", "pick", "0"}, {"recv", "data", "piece_req"}, {"send", "data", "reply-ok"},
      {"send", "pick", "1"}};
  const struct Move peer[] = {{"recv", "pick", "0"},
                              {"send", "pick", "0"},
                              {"send", "data", "piece-req"},
                              {"recv", "data", "reply-ok"},
                              {"recv", "pick", "1"}};
  const struct Move bad_req[] = {{"send", "pick", "0"},
                                 {"recv", "pick", "0"},
                                 {"recv", "data", "piece-bad"},
                                 {"send", "data", "reply-bad"}};

  res = muro__sess_conn_init();
  if (res.status != MURO_SESS_OK) bad("conn init");
  st = res.state;
  muro__sess_conn_expected(st, expected, sizeof expected);
  if (strcmp(expected, "send_pick") != 0) bad("conn init expected");
  if (muro__sess_conn_done(st)) bad("conn init done");
  if (!strstr(res.step, "first choose")) bad("conn init step");

  if (!play("conn", &st, two, 10)) bad("two requests");
  if (!muro__sess_conn_done(st)) bad("two requests not done");
  saved = st;
  res = do_step("conn", &st, MURO_SESS_RECV, MURO_SESS_DATA, 0, "piece-req", 0);
  if (res.status != MURO_SESS_UNEXPECTED) bad("piece after end status");
  if (st.key != saved.key || st.at != saved.at) bad("piece after end moved");
  if (!muro__sess_conn_done(st)) bad("piece after end done");
  if (res.done != 1) bad("end refusal result done");

  res = muro__sess_conn_init();
  st = res.state;
  saved = st;
  res = do_step("conn", &st, MURO_SESS_RECV, MURO_SESS_PICK, 0, NULL, 0);
  if (res.status != MURO_SESS_UNEXPECTED) bad("early recv status");
  if (st.key != saved.key || st.at != saved.at) bad("early recv moved");
  res = do_step("conn", &st, MURO_SESS_SEND, MURO_SESS_DATA, 0, "reply-ok", 0);
  if (res.status != MURO_SESS_UNEXPECTED) bad("early reply status");
  res = do_step("conn", &st, MURO_SESS_SEND, MURO_SESS_PICK, 2, NULL, 0);
  if (res.status != MURO_SESS_UNEXPECTED) bad("pick 2 status");
  res = do_step("conn", &st, MURO_SESS_SEND, MURO_SESS_NAT, 0, NULL, 0);
  if (res.status != MURO_SESS_UNEXPECTED) bad("nat at choose status");
  if (!play("conn", &st, bad_req, 4)) bad("after refusal");
  if (!muro__sess_conn_done(st)) bad("malformed not done");

  res = muro__sess_peer_init();
  if (res.status != MURO_SESS_OK) bad("peer init");
  st = res.state;
  if (!play("peer", &st, peer, 5)) bad("peer trace");
  if (!muro__sess_peer_done(st)) bad("peer not done");

  res = muro__sess_conn_init();
  st = res.state;
  st.key = 99;
  res = do_step("conn", &st, MURO_SESS_SEND, MURO_SESS_PICK, 0, NULL, 0);
  if (res.status != MURO_SESS_UNEXPECTED) bad("bad key status");
  res = muro__sess_conn_init();
  st = res.state;
  st.at = 40000;
  saved = st;
  res = do_step("conn", &st, MURO_SESS_SEND, MURO_SESS_PICK, 0, NULL, 0);
  if (res.status != MURO_SESS_UNEXPECTED) bad("bad at status");
  if (muro__sess_conn_done(saved)) bad("bad at done");
  if (res.done != 0) bad("bad at result done");

  res = muro__sess_init("missing", NULL, 0);
  if (res.status != MURO_SESS_INIT) bad("unknown init status");
  memset(&st, 0, sizeof st);
  res = do_step("missing", &st, MURO_SESS_SEND, MURO_SESS_PICK, 0, NULL, 0);
  if (res.status != MURO_SESS_UNEXPECTED) bad("unknown step status");
  if (!res.step || res.step[0] != 0) bad("unknown step label");
}

int main(int argc, char **argv) {
  if (argc == 1) {
    self_check();
    if (failures != 0) {
      fprintf(stderr, "%d failures\n", failures);
      return 1;
    }
    printf("OK\n");
    return 0;
  }
  if (argc == 2) return run_script(argv[1]);
  fprintf(stderr, "usage: %s [script]\n", argv[0]);
  return 1;
}
