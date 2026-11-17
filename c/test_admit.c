/* Drive proto_admit. The moves come from the emitted admitChoice.

   The script is HTTP events, not protocol moves. Each `admit` line is
   one image event. This driver prints the moves proto_admit offered.
   The Elixir test compares that list with SessionFsm.Conn.check_trace.

   Lines:

     begin
     shut
     set stuck <0|1>
     set key <n>
     admit <phase> <ev> <n> <status> <reply> <observe> <close>

   phase: read decide write idle closed
   ev: bytes eof progress done error tick db
   status: ok (0), bad (1), chunk (2), none (no tag), or a number.
           A tag other than 0 or 1 is a chunk, including one outside
           {0, 1, 2}.
   reply: ok bad none
   observe and close: 0 or 1

   `set` changes the live session before the next admit. The image
   does not. stuck and a key other than the conn end are states the
   stepper can already report.

   One block per admit:

     status <0|1|2>
     done <0|1>
     moves <k>

   status 0: the book refused. status 1: the offer was accepted.
   status 2: the book offered a move and the stepper refused it.
   The committed moves are empty in both refusal cases.
     send pick <bit>
     recv data <ctor>
     .
 */
#include "proto.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int parse_u(const char *s, unsigned long *out) {
  char *end = NULL;
  unsigned long n;
  if (!s || !*s) return 0;
  n = strtoul(s, &end, 10);
  if (!end || *end) return 0;
  *out = n;
  return 1;
}

static int parse_phase(const char *s, int *out) {
  if (strcmp(s, "read") == 0) *out = 0;
  else if (strcmp(s, "decide") == 0) *out = 1;
  else if (strcmp(s, "write") == 0) *out = 2;
  else if (strcmp(s, "idle") == 0) *out = 3;
  else if (strcmp(s, "closed") == 0) *out = 4;
  else return 0;
  return 1;
}

static int parse_ev(const char *s, int *out) {
  if (strcmp(s, "bytes") == 0) *out = 0;
  else if (strcmp(s, "eof") == 0) *out = 1;
  else if (strcmp(s, "progress") == 0) *out = 2;
  else if (strcmp(s, "done") == 0) *out = 3;
  else if (strcmp(s, "error") == 0) *out = 4;
  else if (strcmp(s, "tick") == 0) *out = 5;
  else if (strcmp(s, "db") == 0) *out = 6;
  else return 0;
  return 1;
}

static int parse_status(const char *s, int *out) {
  unsigned long n;
  if (strcmp(s, "ok") == 0) *out = 0;
  else if (strcmp(s, "bad") == 0) *out = 1;
  else if (strcmp(s, "chunk") == 0) *out = 2;
  else if (strcmp(s, "none") == 0) *out = -1;
  else if (parse_u(s, &n) && n <= 65535) *out = (int)n;
  else return 0;
  return 1;
}

static int parse_reply(const char *s, int *out) {
  if (strcmp(s, "ok") == 0) *out = 0;
  else if (strcmp(s, "bad") == 0) *out = 1;
  else if (strcmp(s, "none") == 0) *out = -1;
  else return 0;
  return 1;
}

static int parse_bit(const char *s, int *out) {
  if (strcmp(s, "0") == 0) *out = 0;
  else if (strcmp(s, "1") == 0) *out = 1;
  else return 0;
  return 1;
}

static void print_admit(const Proto *p, int ok) {
  int i;
  int n = proto_move_count(p);
  printf("status %d\n", ok > 0 ? 1 : (ok < 0 ? 2 : 0));
  printf("done %d\n", proto_done(p) ? 1 : 0);
  printf("moves %d\n", n);
  for (i = 0; i < n; i++) {
    ProtoMove m;
    if (!proto_move_at(p, i, &m)) {
      printf("move ?\n");
      continue;
    }
    if (m.kind == PROTO_PICK)
      printf("%s pick %u\n", m.dir == PROTO_SEND ? "send" : "recv", m.bit);
    else
      printf("%s data %s\n", m.dir == PROTO_SEND ? "send" : "recv", m.ctor ? m.ctor : "");
  }
  printf(".\n");
}

/* `admit` is already consumed. The rest of the line is still in strtok. */
static int admit_rest(Proto *p) {
  char *phase_s = strtok(NULL, " \t");
  char *ev_s = strtok(NULL, " \t");
  char *n_s = strtok(NULL, " \t");
  char *status_s = strtok(NULL, " \t");
  char *reply_s = strtok(NULL, " \t");
  char *observe_s = strtok(NULL, " \t");
  char *close_s = strtok(NULL, " \t");
  int phase, ev, status, reply, observe, close_after, ok;
  unsigned long n;
  if (!phase_s || !ev_s || !n_s || !status_s || !reply_s || !observe_s || !close_s) return 0;
  if (strtok(NULL, " \t")) return 0;
  if (!parse_phase(phase_s, &phase) || !parse_ev(ev_s, &ev)) return 0;
  if (!parse_u(n_s, &n)) return 0;
  if (!parse_status(status_s, &status) || !parse_reply(reply_s, &reply)) return 0;
  if (!parse_bit(observe_s, &observe) || !parse_bit(close_s, &close_after)) return 0;
  ok = proto_admit(p, phase, ev, (size_t)n, status, reply, observe, close_after);
  print_admit(p, ok);
  return 1;
}

static int run_script(const char *path) {
  FILE *fp = fopen(path, "r");
  char line[512];
  Proto *proto = NULL;
  int nline = 0;
  int rc = 0;
  if (!fp) {
    fprintf(stderr, "FAIL cannot open %s\n", path);
    return 1;
  }
  while (fgets(line, sizeof line, fp)) {
    char *p = line;
    size_t len = strlen(line);
    char *tok;
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = 0;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == 0 || *p == '#') continue;
    nline += 1;
    tok = strtok(p, " \t");
    if (!tok) {
      rc = 1;
      break;
    }
    if (strcmp(tok, "begin") == 0) {
      if (strtok(NULL, " \t")) {
        rc = 1;
        break;
      }
      proto_free(proto);
      proto = proto_begin();
      if (!proto) {
        fprintf(stderr, "FAIL begin returned NULL\n");
        rc = 1;
        break;
      }
    } else if (strcmp(tok, "shut") == 0) {
      if (strtok(NULL, " \t")) {
        rc = 1;
        break;
      }
      proto_free(proto);
      proto = proto_shut();
      if (!proto) {
        fprintf(stderr, "FAIL shut returned NULL\n");
        rc = 1;
        break;
      }
    } else if (strcmp(tok, "set") == 0) {
      char *kind = strtok(NULL, " \t");
      char *arg = strtok(NULL, " \t");
      unsigned long n;
      if (!proto || !kind || !arg || strtok(NULL, " \t")) {
        rc = 1;
        break;
      }
      if (!parse_u(arg, &n)) {
        rc = 1;
        break;
      }
      if (strcmp(kind, "stuck") == 0) {
        if (n > 1) {
          rc = 1;
          break;
        }
        proto_force_stuck(proto, (int)n);
      } else if (strcmp(kind, "key") == 0) {
        if (n > 65535) {
          rc = 1;
          break;
        }
        proto_force_key(proto, (unsigned)n);
      } else {
        rc = 1;
        break;
      }
    } else if (strcmp(tok, "admit") == 0) {
      if (!proto) {
        fprintf(stderr, "FAIL admit before begin\n");
        rc = 1;
        break;
      }
      if (!admit_rest(proto)) {
        rc = 1;
        break;
      }
    } else {
      rc = 1;
      break;
    }
  }
  if (rc) fprintf(stderr, "FAIL bad line %d\n", nline);
  proto_free(proto);
  fclose(fp);
  return rc;
}

int main(int argc, char **argv) {
  if (argc != 2) {
    fprintf(stderr, "usage: %s script\n", argv[0]);
    return 1;
  }
  return run_script(argv[1]);
}
