# nascheck - portable Makefile for BSD make and GNU make.

PROG=		nascheck
VERSION=	0.1.0
PREFIX?=	/usr/local
SYSCONFDIR?=	${PREFIX}/etc
CC?=		cc
CFLAGS?=	-O2 -pipe

# yyjson is built read-only: the same switches must reach every file
# that includes yyjson.h.
YYJSON=		-DYYJSON_DISABLE_WRITER=1 -DYYJSON_DISABLE_INCR_READER=1 \
		-DYYJSON_DISABLE_FILE=1 -DYYJSON_DISABLE_NON_STANDARD=1
NC_CPPFLAGS=	-D_XOPEN_SOURCE=700 -DNASCHECK_VERSION=\"${VERSION}\" \
		-DNASCHECK_CONF=\"${SYSCONFDIR}/nascheck.conf\" \
		-Ivendor/yyjson ${YYJSON} ${SQLITE_CPPFLAGS}
NC_CFLAGS=	-std=c11 -Wall -Wextra

# SQLite from the system: pkg install sqlite3, or libsqlite3-dev.
LOCALBASE?=	/usr/local
SQLITE_CPPFLAGS=	-I${LOCALBASE}/include
SQLITE_LIBS=	-L${LOCALBASE}/lib -lsqlite3

# ncurses with wide characters (UTF-8, Braille graphs): in the FreeBSD
# base system, libncurses-dev on Debian and Ubuntu.
CURSES_LIBS=	-lncursesw

SRCS=		src/main.c src/config.c src/daemon.c src/data.c src/db.c \
		src/health.c src/json.c src/run.c src/smart.c src/zfs.c \
		src/tui.c src/page_disks.c src/page_packages.c src/page_system.c \
		src/pkgs.c src/sysinfo.c
OBJS=		${SRCS:.c=.o} vendor/yyjson/yyjson.o

# Debug build with AddressSanitizer and UndefinedBehaviorSanitizer.
SANITIZE=	-O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined

all: ${PROG}

${PROG}: ${OBJS}
	${CC} ${CFLAGS} ${LDFLAGS} -o $@ ${OBJS} ${SQLITE_LIBS} ${CURSES_LIBS}

.SUFFIXES: .c .o
.c.o:
	${CC} ${NC_CPPFLAGS} ${NC_CFLAGS} ${CPPFLAGS} ${CFLAGS} -c $< -o $@

${OBJS}: src/config.h src/daemon.h src/data.h src/db.h src/health.h \
		src/json.h src/pkgs.h src/run.h src/smart.h src/sysinfo.h \
		src/tui.h src/wcurses.h src/zfs.h vendor/yyjson/yyjson.h

test: ${PROG}
	sh tests/run.sh ./${PROG}

sanitize: clean
	${MAKE} CFLAGS="${SANITIZE}" LDFLAGS="${SANITIZE}" test

# The service: an rc.d script on FreeBSD, a systemd unit on Linux.
SUBST=		sed -e 's|%%PREFIX%%|${PREFIX}|g' \
		    -e 's|%%SYSCONFDIR%%|${SYSCONFDIR}|g'
SYSTEMDDIR?=	/etc/systemd/system

install: ${PROG}
	install -d ${DESTDIR}${PREFIX}/sbin
	install -m 0755 ${PROG} ${DESTDIR}${PREFIX}/sbin/${PROG}
	install -d ${DESTDIR}${SYSCONFDIR}
	install -m 0644 nascheck.conf.sample \
	    ${DESTDIR}${SYSCONFDIR}/nascheck.conf.sample
	if [ "$$(uname -s)" = FreeBSD ]; then \
		install -d ${DESTDIR}${SYSCONFDIR}/rc.d; \
		${SUBST} service/nascheck.in > ${DESTDIR}${SYSCONFDIR}/rc.d/nascheck; \
		chmod 0555 ${DESTDIR}${SYSCONFDIR}/rc.d/nascheck; \
	elif [ -d /run/systemd/system ]; then \
		install -d ${DESTDIR}${SYSTEMDDIR}; \
		${SUBST} service/nascheck.service.in \
		    > ${DESTDIR}${SYSTEMDDIR}/nascheck.service; \
	fi

uninstall:
	rm -f ${DESTDIR}${PREFIX}/sbin/${PROG} \
	    ${DESTDIR}${SYSCONFDIR}/nascheck.conf.sample \
	    ${DESTDIR}${SYSCONFDIR}/rc.d/nascheck \
	    ${DESTDIR}${SYSTEMDDIR}/nascheck.service

clean:
	rm -f ${PROG} ${OBJS} tests/*.out

.PHONY: all test sanitize install uninstall clean
