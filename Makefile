CC ?= gcc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -I/usr/include/python3.13
LDFLAGS ?= -lpython3.13 -lsqlite3

all: libast.a pycool bin/pycoolctl

libast.a: ast.o traverse.o subscriber/class.o index.o observer.o registry.o ignore.o
	ar rcs $@ ast.o traverse.o subscriber/class.o index.o observer.o registry.o ignore.o

ast.o: ast.c ast.h
	$(CC) $(CFLAGS) -c ast.c

traverse.o: traverse.c traverse.h ast.h
	$(CC) $(CFLAGS) -c traverse.c

subscriber/class.o: subscriber/class.c subscriber/class.h traverse.h ast.h
	$(CC) $(CFLAGS) -I. -c subscriber/class.c -o subscriber/class.o

index.o: index.c index.h
	$(CC) $(CFLAGS) -c index.c

observer.o: observer.c observer.h
	$(CC) $(CFLAGS) -c observer.c

registry.o: registry.c registry.h
	$(CC) $(CFLAGS) -c registry.c

info.o: info.c info.h
	$(CC) $(CFLAGS) -I. -c info.c

ignore.o: ignore.c ignore.h
	$(CC) $(CFLAGS) -c ignore.c

pycool: pycool.c libast.a
	$(CC) $(CFLAGS) -I. -o $@ pycool.c libast.a $(LDFLAGS)

bin/pycoolctl: pycoolctl/main.c pycoolctl/list.c pycoolctl/info.c pycoolctl/class.c pycoolctl/stop.c pycoolctl/clean.c pycoolctl/reindex.c registry.o
	mkdir -p bin
	$(CC) $(CFLAGS) -I. -Ipycoolctl -o $@ pycoolctl/main.c pycoolctl/list.c pycoolctl/info.c pycoolctl/class.c pycoolctl/stop.c pycoolctl/clean.c pycoolctl/reindex.c registry.o -lsqlite3

test_ast: test_ast.c ast.o
	$(CC) $(CFLAGS) -o $@ test_ast.c ast.o $(LDFLAGS)

test_traverse: test_traverse.c ast.o traverse.o
	$(CC) $(CFLAGS) -o $@ test_traverse.c ast.o traverse.o $(LDFLAGS)

test_subscriber: test_subscriber.c ast.o traverse.o subscriber/class.o
	$(CC) $(CFLAGS) -I. -o $@ test_subscriber.c ast.o traverse.o subscriber/class.o $(LDFLAGS)

test_index: test_index.c index.o
	$(CC) $(CFLAGS) -I. -o $@ test_index.c index.o -lsqlite3

clean:
	rm -f *.o *.a test_ast test_traverse test_subscriber test_index pycool
	rm -rf bin

PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
DESTDIR ?=

release:
	$(MAKE) clean
	$(MAKE) CFLAGS="-std=c11 -O3 -Wall -Wextra -I/usr/include/python3.13" all

install: release
	install -d $(DESTDIR)$(BINDIR)
	install -m 0755 pycool $(DESTDIR)$(BINDIR)/pycool
	install -m 0755 bin/pycoolctl $(DESTDIR)$(BINDIR)/pycoolctl

.PHONY: all clean release install
