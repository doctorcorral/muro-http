/* muro-httpd. Sockets, buffers, the clock, SQLite, and the JWT key.
   The status line and the body come from httpRender. The move list
   comes from admitChoice. The image is conn.step. */

#include "host.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(FILE *out) {
  fputs(
      "muro-httpd — HTTP/1.1 on 127.0.0.1\n"
      "\n"
      "  --port N               listen port (0 picks one). default 8080\n"
      "  --idle-ms N            idle tick. default 5000\n"
      "  --max-connections N    accept cap. default 128, max 1024\n"
      "  --db PATH              SQLite file. default :memory:\n"
      "  --jwt-key SECRET       HS256 key, raw bytes, at most 256\n"
      "\n"
      "Environment, used when the flag is absent: MURO_HTTP_PORT,\n"
      "MURO_HTTP_IDLE_MS, MURO_HTTP_MAX_CONNECTIONS, MURO_HTTP_DB,\n"
      "MURO_HTTP_JWT_KEY.\n"
      "\n"
      "The process binds 127.0.0.1 only. SIGINT and SIGTERM stop it.\n"
      "A database that cannot be opened stays down and the process\n"
      "still listens. An empty key checks no bearer.\n",
      out);
}

static int parse_i(const char *s, long lo, long hi, int *out) {
  char *end = NULL;
  long v;
  if (!s || !*s) return -1;
  errno = 0;
  v = strtol(s, &end, 10);
  if (errno || !end || *end || v < lo || v > hi) return -1;
  *out = (int)v;
  return 0;
}

static int take_env(const char *name, long lo, long hi, int *slot) {
  const char *v = getenv(name);
  if (!v) return 0;
  if (parse_i(v, lo, hi, slot) < 0) {
    fprintf(stderr, "muro-httpd: bad %s\n", name);
    return -1;
  }
  return 0;
}

static int take_key(const char *s, HostConfig *cfg) {
  size_t n;
  if (!s) {
    cfg->jwt_key_len = 0;
    return 0;
  }
  n = strlen(s);
  if (n > sizeof cfg->jwt_key) {
    fprintf(stderr, "muro-httpd: jwt key is longer than 256 bytes\n");
    return -1;
  }
  memcpy(cfg->jwt_key, s, n);
  cfg->jwt_key_len = n;
  return 0;
}

int main(int argc, char **argv) {
  HostConfig cfg;
  const char *env_db;
  const char *env_key;
  int i;
  memset(&cfg, 0, sizeof cfg);
  cfg.port = 8080;
  cfg.idle_ms = 5000;
  cfg.max_conn = 128;
  cfg.db_path = ":memory:";
  if (take_env("MURO_HTTP_PORT", 0, 65535, &cfg.port) < 0) return 2;
  if (take_env("MURO_HTTP_IDLE_MS", 1, 86400000, &cfg.idle_ms) < 0) return 2;
  if (take_env("MURO_HTTP_MAX_CONNECTIONS", 1, 1024, &cfg.max_conn) < 0) return 2;
  env_db = getenv("MURO_HTTP_DB");
  if (env_db) {
    if (!env_db[0]) {
      fprintf(stderr, "muro-httpd: bad MURO_HTTP_DB\n");
      return 2;
    }
    cfg.db_path = env_db;
  }
  env_key = getenv("MURO_HTTP_JWT_KEY");
  if (env_key && take_key(env_key, &cfg) < 0) return 2;
  for (i = 1; i < argc; i++) {
    const char *a = argv[i];
    const char *val = NULL;
    int *slot = NULL;
    long lo = 0;
    long hi = 0;
    if (strcmp(a, "--db") == 0 || strcmp(a, "--jwt-key") == 0) {
      if (i + 1 >= argc) {
        fprintf(stderr, "muro-httpd: %s needs a value\n", a);
        return 2;
      }
      val = argv[++i];
      if (strcmp(a, "--db") == 0) {
        if (!val[0]) {
          fprintf(stderr, "muro-httpd: bad --db\n");
          return 2;
        }
        cfg.db_path = val;
      } else if (take_key(val, &cfg) < 0) {
        return 2;
      }
      continue;
    }
    if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
      usage(stdout);
      return 0;
    }
    if (strcmp(a, "--port") == 0) {
      slot = &cfg.port;
      lo = 0;
      hi = 65535;
    } else if (strcmp(a, "--idle-ms") == 0) {
      slot = &cfg.idle_ms;
      lo = 1;
      hi = 86400000;
    } else if (strcmp(a, "--max-connections") == 0) {
      slot = &cfg.max_conn;
      lo = 1;
      hi = 1024;
    } else {
      fprintf(stderr, "muro-httpd: unknown argument %s\n", a);
      usage(stderr);
      return 2;
    }
    if (i + 1 >= argc) {
      fprintf(stderr, "muro-httpd: %s needs a value\n", a);
      return 2;
    }
    val = argv[++i];
    if (parse_i(val, lo, hi, slot) < 0) {
      fprintf(stderr, "muro-httpd: bad %s %s\n", a, val);
      return 2;
    }
  }
  return host_run(&cfg) == 0 ? 0 : 1;
}
