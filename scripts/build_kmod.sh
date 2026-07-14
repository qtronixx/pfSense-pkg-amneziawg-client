#!/bin/sh
# build_kmod.sh
# -----------------------------------------------------------------------
# Инструкция/скрипт сборки if_awg.ko для pfSense 2.8.1.
# Выполняется НЕ на самом pfSense, а на отдельной build-машине
# (рекомендуется: чистая FreeBSD той же версии, что использует ядро
# pfSense 2.8.1, либо official pfSense build tools / poudriere-окружение).
# -----------------------------------------------------------------------

set -e

# 1. Узнать точную версию ядра pfSense 2.8.1, под которую собираем модуль:
#    выполнить на самой pfSense-машине:
#       uname -a
#    Пример вывода:
#       FreeBSD pfSense.local 15.0-CURRENT FreeBSD 15.0-CURRENT
#       amd64 pfSense-CE-2.8.1 ...
#    Дальше все шаги ниже выполняются на build-машине с ИДЕНТИЧНЫМ kernel source tree.

# 2. Получить исходники pfSense kernel source tree (freebsd-src форк Netgate)
#    и исходники amneziawg-freebsd-crossport (kernel module + amneziawg-tools):
git clone --branch RELENG_2_8_1 https://github.com/pfsense/FreeBSD-src.git /usr/src
git clone https://github.com/amnezia-vpn/amneziawg-freebsd-crossport.git /usr/home/build/awg-src

# 3. Собрать сам кернел-модуль штатным механизмом FreeBSD (bsd.kmod.mk),
#    указав явно путь до .../sys заголовков ядра pfSense:
cd /usr/home/build/awg-src/src
make -j"$(sysctl -n hw.ncpu)" SYSDIR=/usr/src/sys

# В результате появится if_awg.ko в текущей директории.

# 4. Собрать userspace-инструменты (аналог wireguard-tools):
cd /usr/home/build/awg-src/src/tools
make -j"$(sysctl -n hw.ncpu)"
# На выходе - бинарь `awg`.

# 5. (Опционально, только если нужен userspace-режим без модуля ядра)
#    Собрать amneziawg-go:
git clone https://github.com/amnezia-vpn/amneziawg-go.git /usr/home/build/amneziawg-go
cd /usr/home/build/amneziawg-go
gmake

# 6. Скопировать результаты на pfSense-машину:
#    if_awg.ko    -> /boot/modules/   (chmod 555)
#    awg          -> /usr/local/bin/  (chmod 755)
#    amneziawg-go -> /usr/local/bin/  (chmod 755, только для userspace-режима)

echo "Готово. Проверьте контрольные суммы бинарников на build- и target-машине."
