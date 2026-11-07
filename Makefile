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
NC_CPPFLAGS=	-D_POSIX_C_SOURCE=200809L -DNASCHECK_VERSION=\"${VERSION}\" \
		-DNASCHECK_CONF=\"${SYSCONFDIR}/nascheck.conf\" \
		-Ivendor/yyjson ${YYJSON} ${SQLITE_CPPFLAGS}
NC_CFLAGS=	-std=c11 -Wall -Wextra

# SQLite from the system: pkg install sqlite3, or libsqlite3-dev.
LOCALBASE?=	/usr/local
SQLITE_CPPFLAGS=	-I${LOCALBASE}/include
SQLITE_LIBS=	-L${LOCALBASE}/lib -lsqlite3

SRCS=		src/main.c src/config.c src/daemon.c src/db.c src/health.c \
		src/run.c src/smart.c
OBJS=		${SRCS:.c=.o} vendor/yyjson/yyjson.o

# Debug build with AddressSanitizer and UndefinedBehaviorSanitizer.
SANITIZE=	-O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined

all: ${PROG}

${PROG}: ${OBJS}
	${CC} ${CFLAGS} ${LDFLAGS} -o $@ ${OBJS} ${SQLITE_LIBS}

.SUFFIXES: .c .o
.c.o:
	${CC} ${NC_CPPFLAGS} ${NC_CFLAGS} ${CPPFLAGS} ${CFLAGS} -c $< -o $@

${OBJS}: src/config.h src/daemon.h src/db.h src/health.h src/run.h \
		src/smart.h \
		vendor/yyjson/yyjson.h

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
