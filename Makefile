# Makefile для сборки пакета pfSense-pkg-amneziawg-client
# Собирается как обычный FreeBSD pkg (pkg create), без портов-инфраструктуры.
#
# ВАЖНО: этот пакет НЕ содержит серверных фич AmneziaWG.
# Все GUI-формы и backend-функции урезаны до режима "Interface [Peer]" -
# то есть только клиентское подключение к внешнему AWG-серверу.
#
# АРХИТЕКТУРА: полностью userspace (amneziawg-go + amneziawg-tools).
# Кернел-модуль не используется - см. README.md за историей решения.

PORTNAME=	pfSense-pkg-amneziawg-client
PORTVERSION=	1.0.0
CATEGORIES=	net-vpn
MASTER_SITES=	# локальная сборка, внешних исходников не требуется

MAINTAINER=	you@example.com
COMMENT=	AmneziaWG VPN Client integration for pfSense 2.8.1 (client-only, userspace)

USES=		metaport
NO_BUILD=	yes
NO_ARCH=	yes

PLIST=		${.CURDIR}/pkg-plist
DESCR=		${.CURDIR}/pkg-descr

# ИЗМЕНЕНО: источник файлов - корень репозитория напрямую (etc/, usr/),
# без промежуточного каталога files/, т.к. в текущей структуре
# репозитория его нет, а заводить его только ради Makefile избыточно.
FILESDIR=	${.CURDIR}
STAGEDIR?=	${.CURDIR}/work/stage

do-install:
	@mkdir -p ${STAGEDIR}
	@for d in etc usr; do \
		(cd ${FILESDIR} && find $$d -type f) | while read f; do \
			install -D -m 0644 "${FILESDIR}/$$f" "${STAGEDIR}/$$f"; \
		done; \
	done
	chmod 555 ${STAGEDIR}/usr/local/etc/rc.d/awg.sh

package:
	pkg create -m ${STAGEDIR} -r ${STAGEDIR} -p ${PLIST} -o ${.CURDIR}/work

.include <bsd.port.mk>