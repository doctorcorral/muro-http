# Host objects and the emitted books. -include supplies muro__ALLOC.
CC ?= cc
OPT ?= -O2
CFLAGS ?= -Wall -Wextra -Werror -std=c11 $(OPT) -D_GNU_SOURCE -I c -I c/generated -include c/shim.h
LDFLAGS ?=
CHALLENGE_INC = -I examples/challenge

.PHONY: all c-check c-sanitize clean

bench/loadgen: bench/loadgen.c
	$(CC) -Wall -Wextra -Werror -std=c11 -O2 -o $@ bench/loadgen.c -pthread

SAN_CFLAGS = -Wall -Wextra -Werror -std=c11 -D_GNU_SOURCE -I c -I c/generated -include c/shim.h -g -fno-omit-frame-pointer -fsanitize=address,undefined
SAN_LDFLAGS = -fsanitize=address,undefined

all: c/muro-httpd c/muro-hello c/muro-stream c/test_classify c/test_conn c/test_sess c/test_render c/test_e2e c/test_body c/test_jwt c/test_json c/test_store c/test_poll c/test_route c/test_tags c/test_hello c/test_serve c/test_workers c/test_http11

c/%.o: c/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

c/test_jwt.o c/test_body.o c/tag_pin.o c/test_store.o c/test_json.o c/test_e2e.o: c/%.o: c/%.c
	$(CC) $(CFLAGS) $(CHALLENGE_INC) -c -o $@ $<

c/conn.o: c/generated/conn.c c/shim.h
	$(CC) $(CFLAGS) -c -o $@ c/generated/conn.c

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

c/stream.o: examples/stream/stream.c
	$(CC) $(CFLAGS) -c -o $@ examples/stream/stream.c

CORE = c/shim.o c/proto.o c/admit_glue.o c/session.o c/admit.o c/conn.o
HOST = c/host.o c/host_poll.o c/host_render.o c/host_date.o c/host_stream.o c/render.o $(CORE)

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

c-check: c/test_classify c/test_conn c/test_sess c/test_render c/test_body c/test_jwt c/test_json c/test_store c/test_poll c/test_route c/test_tags c/test_serve c/test_workers c/muro-httpd c/muro-hello c/muro-stream c/test_e2e c/test_hello c/test_http11
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

c-sanitize:
	$(MAKE) clean
	$(MAKE) c-check CFLAGS="$(SAN_CFLAGS)" LDFLAGS="$(SAN_LDFLAGS)"

clean:
	rm -f c/*.o c/muro-httpd c/muro-hello c/muro-stream c/test_classify c/test_conn c/test_sess c/test_render c/test_e2e c/test_admit c/test_body c/test_jwt c/test_json c/test_store c/test_poll c/test_route c/test_tags c/test_hello c/test_serve c/test_workers c/test_http11
