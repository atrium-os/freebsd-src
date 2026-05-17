PROG=	pingpong
NO_MAN=
WARNS?=	3

# Two binaries from one Makefile via the standard FreeBSD trick.
PROGS=	pingpong wakelat
LDADD.pingpong=
LDADD.wakelat=
SRCS.pingpong=	pingpong.c
SRCS.wakelat=	wakelat.c

SCRIPTS=	bench_all.sh bench_throughput.sh bench_fair.sh bench_skew.sh \
		bench_burst.sh bench_ipc.sh bench_latency.sh

.include <bsd.progs.mk>
