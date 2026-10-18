# $OpenBSD$

REGRESS_TARGETS= reset watchdog

reset:
	sh ${.CURDIR}/reset.sh ${.CURDIR}/../../../../sys/dev/pci/if_ena.c

watchdog:
	sh ${.CURDIR}/watchdog.sh ${.CURDIR}/../../../../sys/dev/pci/if_ena.c

.include <bsd.regress.mk>
