.include "../Makefile.langs"
.include "../../Makefile.cco"

PROG=	cc
LINKS=	${BINDIR}/cc ${BINDIR}/gcc
MANPAGEVER= 47
MFILE=	gcc${MANPAGEVER}.1
MAN=	${MFILE}

SRCS=	${GCC_SRCS} gccspec.c
SRCS+=	${EXTRA_GCC_SRCS}

CFLAGS+= -DCONFIGURE_SPECS="\"\""

${MFILE}: ${GCCDIR}/gcc/doc/gcc.1
	cp ${.ALLSRC} ${.TARGET}

CLEANFILES+=	${MFILE}

MLINKS+= ${MFILE} cc${MANPAGEVER}.1
MLINKS+= ${MFILE} CC${MANPAGEVER}.1
MLINKS+= ${MFILE} c++${MANPAGEVER}.1
MLINKS+= ${MFILE} g++${MANPAGEVER}.1

.if defined(IS_PRIMARY)
MLINKS+= ${MFILE} cc.1
MLINKS+= ${MFILE} CC.1
MLINKS+= ${MFILE} gcc.1
MLINKS+= ${MFILE} c++.1
MLINKS+= ${MFILE} g++.1
.endif

.include <bsd.prog.mk>
