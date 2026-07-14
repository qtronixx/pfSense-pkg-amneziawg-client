# Makefile для сборки пакета pfSense-pkg-amneziawg-client
# Собирается как обычный FreeBSD pkg (pkg create), без портов-инфраструктуры,
# т.к. пакет состоит только из PHP/shell файлов (бинарный if_awg.ko поставляется отдельно).

PORTNAME=	pfSense-pkg-amneziawg-client
PORTVERSION=	1.0.0
CATEGORIES=	net-vpn
MASTER_SITES=	# локальная сборка, внешних исходников не требуется

MAINTAINER=	you@example.com
COMMENT=	AmneziaWG VPN Client integration for pfSense 2.8.1 (client-only)

# ВАЖНО: этот пакет НЕ содержит серверных фич AmneziaWG.
# Все GUI-формы и backend-функции урезаны до режима "Interface [Peer]" -
# то есть только клиентское подключение к внешнему AWG-серверу.

USES=		metaport
NO_BUILD=	yes
NO_ARCH=	yes

PLIST=		${.CURDIR}/pkg-plist
DESCR=		${.CURDIR}/pkg-descr

FILESDIR=	${.CURDIR}/files
STAGEDIR?=	${.CURDIR}/work/stage

do-install:
	@mkdir -p ${STAGEDIR}
	@(cd ${FILESDIR} && find . -type f) | while read f; do \
		install -D -m 0644 "${FILESDIR}/$$f" "${STAGEDIR}/$$f"; \
	done
	# Исполняемые скрипты - отдельно, с правом на выполнение
	chmod 555 ${STAGEDIR}/usr/local/etc/rc.d/awg.sh

package:
	pkg create -m ${STAGEDIR} -r ${STAGEDIR} -p ${PLIST} -o ${.CURDIR}/work

.include <bsd.port.mk>
