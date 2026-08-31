MAKEOBJDIR?=	${.CURDIR}/obj

.PATH:	${.CURDIR}/sys/net

KMOD=	if_veb
SRCS=	if_veb.c opt_inet.h opt_inet6.h opt_carp.h

CFLAGS+=	-I${.CURDIR}/sys

.if !defined(KERNBUILDDIR)
CFLAGS+=	-DVIMAGE
.if !defined(WITHOUT_INVARIANTS)
CFLAGS+=	-DINVARIANTS -DINVARIANT_SUPPORT
.endif
.endif

# Userland test harness, built into the object directory
vebctl: ${.CURDIR}/vebctl.c ${.CURDIR}/sys/net/if_vebvar.h
	${CC} -O2 -pipe -g -Wall -Wextra -I${.CURDIR}/sys \
	    -o ${.TARGET} ${.CURDIR}/vebctl.c
CLEANFILES+=	vebctl


.include <bsd.kmod.mk>
