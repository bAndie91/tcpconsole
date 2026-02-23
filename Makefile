VERSION=0.3

# www.vanheusden.com

DEBUG = -g -D_DEBUG
ARCH_BITS = 32
override LDFLAGS += $(DEBUG)
override CFLAGS += -O2 -m$(ARCH_BITS) -Wall -Wextra -DVERSION=\"$(VERSION)\" $(DEBUG)

OBJS=error.o tc.o sources.o

all: tcpconsole


tc.o: sources.o

sources.o:
	elfrc -o sources.o -h sources.h -v < resources

tcpconsole: $(OBJS)
	$(CC) $(CFLAGS) -m$(ARCH_BITS) -Wall -W $(OBJS) $(LDFLAGS) -o tcpconsole

install: /sbin/tcpconsole install-inittab

/sbin/tcpconsole: tcpconsole
	install tcpconsole /sbin

install-inittab: /sbin/tcpconsole
	grep -E '^tc:' /etc/inittab || echo "tc:12345:respawn:/sbin/tcpconsole" >>/etc/inittab
	telinit q

uninstall: uninstall-inittab
	rm /sbin/tcpconsole

uninstall-inittab:
	sed -i /etc/inittab -e '/^tc:/d'
	telinit q

clean:
	rm -f $(OBJS) core tcpconsole

package: clean
	# source package
	rm -rf tcpconsole-$(VERSION)*
	mkdir tcpconsole-$(VERSION)
	cp *.c *.h Makefile* readme.txt tcpconsole-$(VERSION)
	tar cf - tcpconsole-$(VERSION) | gzip -9 > tcpconsole-$(VERSION).tgz
	rm -rf tcpconsole-$(VERSION)

.PHONY: all install install-inittab uninstall uninstall-inittab clean package
