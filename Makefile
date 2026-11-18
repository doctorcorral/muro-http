# Host objects and the emitted books. -include supplies muro__ALLOC.
CC ?= cc
CFLAGS ?= -Wall -Wextra -Werror -std=c11 -D_GNU_SOURCE -I c -I c/generated -include c/shim.h
LDFLAGS ?=

.PHONY: all c-check c-sanitize clean

SAN_CFLAGS = -Wall -Wextra -Werror -std=c11 -D_GNU_SOURCE -I c -I c/generated -include c/shim.h -g -fno-omit-frame-pointer -fsanitize=address,undefined
SAN_LDFLAGS = -fsanitize=address,undefined

all: c/muro-httpd c/test_classify c/test_conn c/test_sess c/test_render c/test_e2e

c/%.o: c/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

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

CORE = c/shim.o c/proto.o c/admit_glue.o c/session.o c/admit.o c/conn.o

c/test_classify: c/test_classify.o c/request.o c/shim.o
	$(CC) $(LDFLAGS) -o $@ c/test_classify.o c/request.o c/shim.o

c/test_conn: c/test_conn.o $(CORE)
	$(CC) $(LDFLAGS) -o $@ c/test_conn.o $(CORE)

c/test_sess: c/test_sess.o c/session.o c/shim.o
	$(CC) $(LDFLAGS) -o $@ c/test_sess.o c/session.o c/shim.o

c/test_render: c/test_render.o c/render.o c/tag_pin.o c/conn.o c/shim.o
	$(CC) $(LDFLAGS) -o $@ c/test_render.o c/render.o c/tag_pin.o c/conn.o c/shim.o

c/test_e2e: c/test_e2e.o
	$(CC) $(LDFLAGS) -o $@ c/test_e2e.o

c/muro-httpd: c/httpd.o c/host.o c/host_poll.o c/host_render.o c/render.o $(CORE)
	$(CC) $(LDFLAGS) -o $@ c/httpd.o c/host.o c/host_poll.o c/host_render.o c/render.o $(CORE)

c-check: c/test_classify c/test_conn c/test_sess c/test_render c/muro-httpd c/test_e2e
	./c/test_classify
	./c/test_conn
	./c/test_sess
	./c/test_render
	./c/test_e2e c/muro-httpd

c-sanitize:
	$(MAKE) clean
	$(MAKE) c-check CFLAGS="$(SAN_CFLAGS)" LDFLAGS="$(SAN_LDFLAGS)"

clean:
	rm -f c/*.o c/muro-httpd c/test_classify c/test_conn c/test_sess c/test_render c/test_e2e c/test_admit
