# nascheck - portable Makefile for BSD make and GNU make.

PROG=		nascheck
VERSION=	0.1.0
PREFIX?=	/usr/local
CC?=		cc
CFLAGS?=	-O2 -pipe

# yyjson is built read-only: the same switches must reach every file
# that includes yyjson.h.
YYJSON=		-DYYJSON_DISABLE_WRITER=1 -DYYJSON_DISABLE_INCR_READER=1 \
		-DYYJSON_DISABLE_FILE=1 -DYYJSON_DISABLE_NON_STANDARD=1
NC_CPPFLAGS=	-D_POSIX_C_SOURCE=200809L -DNASCHECK_VERSION=\"${VERSION}\" \
		-Ivendor/yyjson ${YYJSON}
NC_CFLAGS=	-std=c11 -Wall -Wextra

SRCS=		src/main.c src/run.c src/smart.c
OBJS=		${SRCS:.c=.o} vendor/yyjson/yyjson.o

# Debug build with AddressSanitizer and UndefinedBehaviorSanitizer.
SANITIZE=	-O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined

all: ${PROG}

${PROG}: ${OBJS}
	${CC} ${CFLAGS} ${LDFLAGS} -o $@ ${OBJS}

.SUFFIXES: .c .o
.c.o:
	${CC} ${NC_CPPFLAGS} ${NC_CFLAGS} ${CPPFLAGS} ${CFLAGS} -c $< -o $@

${OBJS}: src/run.h src/smart.h vendor/yyjson/yyjson.h

test: ${PROG}
	sh tests/run.sh ./${PROG}

sanitize: clean
	${MAKE} CFLAGS="${SANITIZE}" LDFLAGS="${SANITIZE}" test

install: ${PROG}
	install -d ${DESTDIR}${PREFIX}/sbin
	install -m 0755 ${PROG} ${DESTDIR}${PREFIX}/sbin/${PROG}

uninstall:
	rm -f ${DESTDIR}${PREFIX}/sbin/${PROG}

clean:
	rm -f ${PROG} ${OBJS} tests/*.out

.PHONY: all test sanitize install uninstall clean
