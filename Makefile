# Host objects and the emitted books. -include supplies muro__ALLOC.
CC ?= cc
OPT ?= -O2
CFLAGS ?= -Wall -Wextra -Werror -std=c11 $(OPT) -D_GNU_SOURCE -I c -I c/generated -include c/shim.h
LDFLAGS ?=
CHALLENGE_INC = -I examples/challenge

.PHONY: all c-check c-sanitize c-conformance clean fuzz

bench/loadgen: bench/loadgen.c
	$(CC) -Wall -Wextra -Werror -std=c11 -O2 -o $@ bench/loadgen.c -pthread

SAN_CFLAGS = -Wall -Wextra -Werror -std=c11 -D_GNU_SOURCE -I c -I c/generated -include c/shim.h -g -fno-omit-frame-pointer -fsanitize=address,undefined
SAN_LDFLAGS = -fsanitize=address,undefined

all: c/muro-httpd c/muro-hello c/muro-stream c/test_classify c/test_conn c/test_sess c/test_render c/test_e2e c/test_body c/test_jwt c/test_json c/test_store c/test_poll c/test_route c/test_tags c/test_hello c/test_serve c/test_workers c/test_http11 c/test_step

c/%.o: c/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

c/test_jwt.o c/test_body.o c/tag_pin.o c/test_store.o c/test_json.o c/test_e2e.o: c/%.o: c/%.c
	$(CC) $(CFLAGS) $(CHALLENGE_INC) -c -o $@ $<

# The book defines scan and crlfEnd. Rename those definitions so every
# call, including the ones in this file, goes through c/scan_cache.c.
# The generated text stays as muro wrote it.
c/conn.patched.c: c/generated/conn.c
	sed -e 's/^HSt scan(/HSt scan_uncached(/' \
	    -e 's/^uint64_t crlfEnd(/uint64_t crlfEnd_uncached(/' $< > $@
	grep -q '^HSt scan_uncached(muro__slice s)' $@
	grep -q '^uint64_t crlfEnd_uncached(muro__slice s)' $@

c/conn.o: c/conn.patched.c c/shim.h
	$(CC) $(CFLAGS) -c -o $@ c/conn.patched.c

c/session.o: c/generated/session.c c/shim.h
	$(CC) $(CFLAGS) -c -o $@ c/generated/session.c

c/admit.o: c/generated/admit.c c/shim.h
	$(CC) $(CFLAGS) -c -o $@ c/generated/admit.c

c/request.o: c/generated/request.c c/shim.h
	$(CC) $(CFLAGS) -c -o $@ c/generated/request.c

c/render.o: c/generated/render.c c/shim.h
	$(CC) $(CFLAGS) -c -o $@ c/generated/render.c

c/body.o: c/generated/body.c c/shim.h
	$(CC) $(CFLAGS) -c -o $@ c/generated/body.c

c/route.o: c/generated/route.c c/shim.h
	$(CC) $(CFLAGS) -c -o $@ c/generated/route.c

c/decide.o: c/generated/decide.c c/shim.h
	$(CC) $(CFLAGS) -c -o $@ c/generated/decide.c

c/store.o: examples/challenge/store.c
	$(CC) $(CFLAGS) $(CHALLENGE_INC) -c -o $@ examples/challenge/store.c

c/jwt.o: examples/challenge/jwt.c
	$(CC) $(CFLAGS) $(CHALLENGE_INC) -c -o $@ examples/challenge/jwt.c

c/json_text.o: examples/challenge/json_text.c
	$(CC) $(CFLAGS) $(CHALLENGE_INC) -c -o $@ examples/challenge/json_text.c

c/host_body.o: examples/challenge/host_body.c
	$(CC) $(CFLAGS) $(CHALLENGE_INC) -c -o $@ examples/challenge/host_body.c

c/httpd.o: examples/challenge/httpd.c
	$(CC) $(CFLAGS) $(CHALLENGE_INC) -c -o $@ examples/challenge/httpd.c

c/app.o: examples/challenge/app.c
	$(CC) $(CFLAGS) $(CHALLENGE_INC) -c -o $@ examples/challenge/app.c

c/route_glue.o: examples/challenge/route_glue.c
	$(CC) $(CFLAGS) $(CHALLENGE_INC) -c -o $@ examples/challenge/route_glue.c

c/hello.o: examples/hello/hello.c
	$(CC) $(CFLAGS) -c -o $@ examples/hello/hello.c

c/mem_adapter.o: examples/adapter/mem.c
	$(CC) $(CFLAGS) -c -o $@ examples/adapter/mem.c

c/stream.o: examples/stream/stream.c
	$(CC) $(CFLAGS) -c -o $@ examples/stream/stream.c

CORE = c/shim.o c/proto.o c/admit_glue.o c/session.o c/admit.o c/conn.o c/scan_cache.o
HOST = c/host.o c/step.o c/host_poll.o c/host_render.o c/host_date.o c/host_stream.o c/render.o $(CORE)

c/test_classify: c/test_classify.o c/request.o c/shim.o
	$(CC) $(LDFLAGS) -o $@ c/test_classify.o c/request.o c/shim.o

c/test_conn: c/test_conn.o $(CORE)
	$(CC) $(LDFLAGS) -o $@ c/test_conn.o $(CORE)

c/test_sess: c/test_sess.o c/session.o c/shim.o
	$(CC) $(LDFLAGS) -o $@ c/test_sess.o c/session.o c/shim.o

c/test_render: c/test_render.o c/host_render.o c/host_date.o c/render.o c/shim.o
	$(CC) $(LDFLAGS) -o $@ c/test_render.o c/host_render.o c/host_date.o c/render.o c/shim.o

c/test_e2e: c/test_e2e.o c/jwt.o
	$(CC) $(LDFLAGS) -o $@ c/test_e2e.o c/jwt.o

c/test_body: c/test_body.o c/body.o c/shim.o
	$(CC) $(LDFLAGS) -o $@ c/test_body.o c/body.o c/shim.o

c/test_jwt: c/test_jwt.o c/jwt.o
	$(CC) $(LDFLAGS) -o $@ c/test_jwt.o c/jwt.o

c/test_json: c/test_json.o c/json_text.o
	$(CC) $(LDFLAGS) -o $@ c/test_json.o c/json_text.o

c/test_store: c/test_store.o c/store.o c/json_text.o
	$(CC) $(LDFLAGS) -o $@ c/test_store.o c/store.o c/json_text.o -lsqlite3

c/test_poll: c/test_poll.o c/host_poll.o
	$(CC) $(LDFLAGS) -o $@ c/test_poll.o c/host_poll.o

c/test_route: c/test_route.o c/route.o c/shim.o
	$(CC) $(LDFLAGS) -o $@ c/test_route.o c/route.o c/shim.o

c/test_tags: c/test_tags.o c/tag_pin.o c/decide.o c/shim.o
	$(CC) $(LDFLAGS) -o $@ c/test_tags.o c/tag_pin.o c/decide.o c/shim.o

c/test_hello: c/test_hello.o
	$(CC) $(LDFLAGS) -o $@ c/test_hello.o

c/test_serve: c/test_serve.o $(HOST)
	$(CC) $(LDFLAGS) -o $@ c/test_serve.o $(HOST)

c/test_workers: c/test_workers.o $(HOST)
	$(CC) $(LDFLAGS) -o $@ c/test_workers.o $(HOST)

c/muro-httpd: c/httpd.o c/app.o c/route_glue.o c/host_body.o c/body.o c/decide.o c/route.o c/jwt.o c/json_text.o c/store.o $(HOST)
	$(CC) $(LDFLAGS) -o $@ c/httpd.o c/app.o c/route_glue.o c/host_body.o c/body.o c/decide.o c/route.o c/jwt.o c/json_text.o c/store.o $(HOST) -lsqlite3

c/muro-hello: c/hello.o $(HOST)
	$(CC) $(LDFLAGS) -o $@ c/hello.o $(HOST)

c/muro-stream: c/stream.o $(HOST)
	$(CC) $(LDFLAGS) -o $@ c/stream.o $(HOST)

c/test_http11: c/test_http11.o
	$(CC) $(LDFLAGS) -o $@ c/test_http11.o

c/test_step: c/mem_adapter.o $(HOST)
	$(CC) $(LDFLAGS) -o $@ c/mem_adapter.o $(HOST)

c-conformance: c/muro-hello
	PYTHONDONTWRITEBYTECODE=1 python3 c/test_conformance.py c/muro-hello

c-check: bench/loadgen c/test_classify c/test_conn c/test_sess c/test_render c/test_body c/test_jwt c/test_json c/test_store c/test_poll c/test_route c/test_tags c/test_serve c/test_workers c/muro-httpd c/muro-hello c/muro-stream c/test_e2e c/test_hello c/test_http11 c/test_step
	./c/test_classify
	./c/test_conn
	./c/test_sess
	./c/test_render
	./c/test_body
	./c/test_jwt
	./c/test_json
	./c/test_store
	./c/test_poll
	./c/test_route
	./c/test_tags
	./c/test_serve
	./c/test_workers
	./c/test_hello
	./c/test_e2e c/muro-httpd
	./c/test_http11
	sh c/check_step_pure.sh
	./c/test_step

c-sanitize:
	$(MAKE) clean
	$(MAKE) c-check CFLAGS="$(SAN_CFLAGS)" LDFLAGS="$(SAN_LDFLAGS)"

clean:
	rm -f c/*.o c/conn.patched.c c/muro-httpd c/muro-hello c/muro-stream c/test_classify c/test_conn c/test_sess c/test_render c/test_e2e c/test_admit c/test_body c/test_jwt c/test_json c/test_store c/test_poll c/test_route c/test_tags c/test_hello c/test_serve c/test_workers c/test_http11 c/test_step bench/loadgen fuzz/step_fuzz

# libFuzzer + ASan + UBSan. clang is fixed here so CC, c-check, and
# c-sanitize keep using $(CC). Objects are not written into c/.
FUZZ_CC = clang
FUZZ_SAN = -fsanitize=fuzzer,address,undefined
FUZZ_CFLAGS = -std=c11 -g -O1 -fno-omit-frame-pointer -fno-sanitize-recover=undefined $(FUZZ_SAN) -Wall -Wextra -Werror -D_GNU_SOURCE -I c -I c/generated -include c/shim.h
# conn.c already emits the request classifiers. Linking request.c
# twice defines those symbols twice. render.c is separate.
FUZZ_SRCS = fuzz/step_fuzz.c c/step.c c/shim.c c/proto.c c/admit_glue.c c/host_render.c c/host_stream.c c/generated/conn.c c/generated/session.c c/generated/admit.c c/generated/render.c

fuzz/step_fuzz: $(FUZZ_SRCS) c/step.h c/shim.h c/host_stream.h c/host_render.h
	$(FUZZ_CC) $(FUZZ_CFLAGS) -o $@ $(FUZZ_SRCS)

fuzz: fuzz/step_fuzz
