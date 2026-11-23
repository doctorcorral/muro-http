/* muro-httpd. The challenge application: four routes, SQLite, and
   one HS256 key, on the reusable HTTP/1.1 server. */

#include "muro_http.h"
#include "app.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(FILE *out) {
  fputs(
      "muro-httpd — HTTP/1.1 challenge server\n"
      "\n"
      "  --bind HOST            IPv4 address. default 127.0.0.1\n"
      "  --port N               listen port (0 picks one). default 8080\n"
      "  --idle-ms N            idle tick. default 5000\n"
      "  --max-connections N    accept cap per worker. default 128, max 1024\n"
      "  --max-body N           body cap in bytes. default 8388608\n"
      "  --drain-ms N           SIGINT/SIGTERM drain. default 1000\n"
      "  --workers N            processes. default 1, max 64\n"
      "  --wait NAME            poll, kqueue, or epoll. default is native\n"
      "  --db PATH              SQLite file. Overrides MURO_HTTP_DB and SQLITE_PATH.\n"
      "                         Default :memory: is test-only and is not WAL.\n"
      "  --jwt-key SECRET       HS256 key, raw bytes, at most 256\n"
      "\n"
      "Environment, used when the flag is absent: MURO_HTTP_BIND,\n"
      "MURO_HTTP_PORT, MURO_HTTP_IDLE_MS, MURO_HTTP_MAX_CONNECTIONS,\n"
      "MURO_HTTP_MAX_BODY, MURO_HTTP_DRAIN_MS, MURO_HTTP_WORKERS,\n"
      "MURO_HTTP_WAIT, MURO_HTTP_JWT_KEY. The database path is --db,\n"
      "then MURO_HTTP_DB, then SQLITE_PATH, then :memory:. A file that\n"
      "is not wal stays down. The process still listens. An empty key\n"
      "checks no bearer.\n"
      "\n"
      "The default bind host is 127.0.0.1. One worker is one process.\n"
      "Above that, each process opens its own SQLite connection.\n"
      ":memory: is not shared. SIGINT and SIGTERM stop it.\n",
      out);
}

static int take_key(const char *s, uint8_t *key, size_t cap, size_t *key_n) {
  size_t n;
  if (!s) {
    *key_n = 0;
    return 0;
  }
  n = strlen(s);
  if (n > cap) {
    fprintf(stderr, "muro-httpd: jwt key is longer than 256 bytes\n");
    return -1;
  }
  memcpy(key, s, n);
  *key_n = n;
  return 0;
}

int main(int argc, char **argv) {
  MuroHttpConfig cfg;
  Challenge *app;
  uint8_t key[256];
  size_t key_n = 0;
  const char *db = ":memory:";
  const char *env_db;
  const char *env_key;
  int i;
  int rc;
  muro_http_config_init(&cfg);
  cfg.name = "muro-httpd";
  if (muro_http_config_env(&cfg) < 0) return 2;
  env_db = getenv("SQLITE_PATH");
  if (env_db) {
    if (!env_db[0]) {
      fprintf(stderr, "muro-httpd: bad SQLITE_PATH\n");
      return 2;
    }
    db = env_db;
  }
  env_db = getenv("MURO_HTTP_DB");
  if (env_db) {
    if (!env_db[0]) {
      fprintf(stderr, "muro-httpd: bad MURO_HTTP_DB\n");
      return 2;
    }
    db = env_db;
  }
  env_key = getenv("MURO_HTTP_JWT_KEY");
  if (env_key && take_key(env_key, key, sizeof key, &key_n) < 0) return 2;
  for (i = 1; i < argc; i++) {
    const char *a = argv[i];
    rc = muro_http_config_arg(&cfg, &i, argc, argv);
    if (rc == 2) {
      usage(stdout);
      return 0;
    }
    if (rc < 0) return 2;
    if (rc == 1) continue;
    if (strcmp(a, "--db") == 0 || strcmp(a, "--jwt-key") == 0) {
      const char *val;
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
        db = val;
      } else if (take_key(val, key, sizeof key, &key_n) < 0) {
        return 2;
      }
      continue;
    }
    fprintf(stderr, "muro-httpd: unknown argument %s\n", a);
    usage(stderr);
    return 2;
  }
  app = challenge_open(db, key, key_n);
  if (!app) return 1;
  rc = muro_http_serve(&cfg, challenge_handle, app);
  challenge_close(app);
  if (rc == -2) return 2;
  return rc == 0 ? 0 : 1;
}
