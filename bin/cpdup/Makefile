PROG=	cpdup
SRCS=	cpdup.c hcproto.c hclink.c misc.c fsmid.c

.if defined(.FreeBSD)
CFLAGS += -D_ST_FLAGS_PRESENT_=1 -DWITH_LIBMD
WARNS?=	6
.endif

.if defined(BOOTSTRAPPING) || defined(RESCUE)
# For boostrapping buildworld the md5 functionality is not needed
CFLAGS+=-DNOMD5
.else
.if !defined(NOMD5)
SRCS+=	md5.c
CFLAGS+=	${PRIVATELIB_CFLAGS}
DPADD=		${LIBCRYPTO}
LDADD=		-lprivate_crypto
LDFLAGS+=	${PRIVATELIB_LDFLAGS}
.endif
.endif

.include <bsd.prog.mk>

