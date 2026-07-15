#!/bin/sh
# install_binaries.sh
# -----------------------------------------------------------------------
# Копирует собранные на build-VM бинарники amneziawg-go и awg на pfSense.
# Запускать НА pfSense-машине, предварительно передав файлы по scp
# в /tmp/ (см. README_BUILD.md за инструкцией сборки на build-VM).
#
# ВАЖНО (подтверждено практикой 15.07.2026):
#   - awg собирается через `gmake` (НЕ штатный `make` - Makefile
#     использует GNU-специфичный синтаксис, штатный BSD make его не
#     поймёт вообще, см. историю ошибок в диалоге разработки).
#   - `gmake install` на тестовой build-VM положил бинарь размером
#     0 байт (причина не установлена окончательно - возможно, обрыв
#     процесса копирования) - НАДЁЖНЕЕ копировать src/wg вручную,
#     что и делает этот скрипт.
# -----------------------------------------------------------------------

set -e

SRC_DIR="/tmp"
DST_DIR="/usr/local/bin"

if [ ! -f "${SRC_DIR}/amneziawg-go" ] || [ ! -f "${SRC_DIR}/awg" ]; then
    echo "Ошибка: не найдены ${SRC_DIR}/amneziawg-go и/или ${SRC_DIR}/awg"
    echo "Скопируйте их с build-VM: scp amneziawg-go awg admin@pfsense:/tmp/"
    exit 1
fi

install -m 755 -o root -g wheel "${SRC_DIR}/amneziawg-go" "${DST_DIR}/amneziawg-go"
install -m 755 -o root -g wheel "${SRC_DIR}/awg"          "${DST_DIR}/awg"

echo "Проверка версий:"
"${DST_DIR}/awg" --version || true
file "${DST_DIR}/amneziawg-go"

echo "Готово. Бинарники установлены в ${DST_DIR}/"
