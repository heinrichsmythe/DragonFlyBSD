# $FreeBSD: src/usr.sbin/mailwrapper/Makefile,v 1.4.2.5 2003/04/16 11:01:51 ru Exp $

PROG=	mailwrapper
MAN=	mailwrapper.8

DPADD=	${LIBUTIL}
LDADD=	-lutil

SYMLINKS= 	mailwrapper ${BINDIR}/sendmail  \
		mailwrapper ${BINDIR}/hoststat  \
		mailwrapper ${BINDIR}/purgestat \
		../sbin/mailwrapper /usr/bin/newaliases \
		../sbin/mailwrapper /usr/bin/mailq

afterinstall:
.if !exists(${DESTDIR}/etc/mail/mailer.conf)
	${INSTALL} -o ${BINOWN} -g ${BINGRP} -m 644 \
	    ${.CURDIR}/../../etc/mail/mailer.conf ${DESTDIR}/etc/mail
.endif

.include <bsd.prog.mk>
