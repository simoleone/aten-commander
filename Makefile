CFLAGS  = -O2 -Wall -Wextra -arch arm64 -arch x86_64 -mmacosx-version-min=12.0
LDFLAGS = -framework IOKit -framework CoreFoundation
PREFIX ?= $(HOME)

atenswitch: atenswitch.c
	clang $(CFLAGS) -o $@ $< $(LDFLAGS)

install: atenswitch
	install -d $(PREFIX)/bin
	install -m 755 atenswitch $(PREFIX)/bin/atenswitch

clean:
	rm -f atenswitch

.PHONY: install clean
