#!/bin/sh
#
# install.sh - установка пакета AmneziaWG Client для pfSense 2.8.1.
#
# Запускается прямо на pfSense из склонированного репозитория:
#   git clone https://github.com/qtronixx/pfSense-pkg-amneziawg-client.git
#   cd pfSense-pkg-amneziawg-client
#   sh install.sh
#
# Команды:
#   install    Полная установка (по умолчанию)
#   update     Копирует файлы заново, перезапускает туннели
#   uninstall  Останавливает туннели, чистит config.xml, удаляет файлы
#
# АРХИТЕКТУРА (по образцу pfSense-pkg-xray): не используется FreeBSD
# pkg/.pkg/manifest.ucl вообще - файлы копируются напрямую, а
# регистрация в pfSense выполняется собственной PHP-функцией
# awg_install()/awg_deinstall() из awg.inc, без зависимости от
# install_package_xml()/info.xml (хрупкий legacy-путь, с которым мы
# намучились при первой попытке).

set -e
set -u

COMMAND="${1:-install}"

info() { echo "==> $*"; }
ok()   { echo "  [OK] $*"; }
die()  { echo "[ERROR] $*" >&2; exit 1; }

REPO_ROOT="$(cd "$(dirname "$0")" && pwd)"

if [ ! -f /etc/inc/config.inc ]; then
    die "Этот скрипт нужно запускать на pfSense (не найден /etc/inc/config.inc)."
fi

cmd_deploy_files() {
    info "Копирование файлов пакета..."

    mkdir -p /usr/local/pkg
    mkdir -p /usr/local/www/widgets/widgets
    mkdir -p /usr/local/etc/rc.d
    mkdir -p /etc/inc/priv
    mkdir -p /usr/local/bin

    cp "${REPO_ROOT}/usr/local/pkg/awg.inc"           /usr/local/pkg/awg.inc
    cp "${REPO_ROOT}/usr/local/pkg/awg_validate.inc"  /usr/local/pkg/awg_validate.inc
    cp "${REPO_ROOT}/usr/local/pkg/awg.xml"           /usr/local/pkg/awg.xml

    cp "${REPO_ROOT}/usr/local/www/vpn_awg_tunnels.php" /usr/local/www/vpn_awg_tunnels.php
    cp "${REPO_ROOT}/usr/local/www/vpn_awg_edit.php"    /usr/local/www/vpn_awg_edit.php
    cp "${REPO_ROOT}/usr/local/www/vpn_awg_status.php"  /usr/local/www/vpn_awg_status.php
    cp "${REPO_ROOT}/usr/local/www/widgets/widgets/awg_status.widget.php" /usr/local/www/widgets/widgets/awg_status.widget.php
    cp "${REPO_ROOT}/usr/local/www/widgets/widgets/awg_status.xml"       /usr/local/www/widgets/widgets/awg_status.xml

    cp "${REPO_ROOT}/etc/inc/priv/awg.priv.inc" /etc/inc/priv/awg.priv.inc

    cp "${REPO_ROOT}/usr/local/etc/rc.d/awg.sh" /usr/local/etc/rc.d/awg
    chmod 555 /usr/local/etc/rc.d/awg

    # НОВОЕ: бинарники теперь входят в репозиторий (локально пропатченная
    # сборка awg - см. историю фикса MAX_AWG_STRING_LEN - официального
    # релиза с этим патчем не существует, поэтому GitHub Releases
    # апстрима не подходит как источник).
    if [ -f "${REPO_ROOT}/bin/amneziawg-go" ] && [ -f "${REPO_ROOT}/bin/awg" ]; then
        cp "${REPO_ROOT}/bin/amneziawg-go" /usr/local/bin/amneziawg-go
        cp "${REPO_ROOT}/bin/awg"          /usr/local/bin/awg
        chmod 755 /usr/local/bin/amneziawg-go /usr/local/bin/awg
        ok "Бинарники amneziawg-go/awg установлены из репозитория"
    else
        echo "  [WARN] bin/amneziawg-go или bin/awg не найдены в репозитории - установите их вручную в /usr/local/bin/"
    fi

    ok "Файлы скопированы"
}

cmd_register() {
    info "Регистрация пакета в pfSense..."

    # Защита на случай, если services_dhcp.inc отсутствует (DHCP вынесен
    # в отдельный пакет на некоторых инсталляциях 2.8.1) и что-то в
    # цепочке require config.inc/globals.inc всё же на него сошлётся -
    # тот же приём, что и в pfSense-pkg-xray, подтверждённый рабочим.
    STUB_DIR="/tmp/awg-inc-stub-$$"
    mkdir -p "${STUB_DIR}"
    printf '<?php\n' > "${STUB_DIR}/services_dhcp.inc"

    PHP_SCRIPT="/tmp/awg-register-$$.php"
    cat > "${PHP_SCRIPT}" << 'PHPEOF'
<?php
set_include_path('/etc/inc' . PATH_SEPARATOR . '/usr/local/share/pear' . PATH_SEPARATOR . ini_get('include_path'));
require_once('globals.inc');
require_once('config.inc');
require_once('/usr/local/pkg/awg.inc');
awg_install();
echo "done" . PHP_EOL;
PHPEOF

    if php -d "include_path=${STUB_DIR}:/etc/inc:/usr/local/share/pear" "${PHP_SCRIPT}"; then
        rm -f "${PHP_SCRIPT}"
        rm -rf "${STUB_DIR}"
    else
        rm -f "${PHP_SCRIPT}"
        rm -rf "${STUB_DIR}"
        die "Не удалось зарегистрировать пакет"
    fi

    ok "Пакет зарегистрирован (меню VPN -> AmneziaWG добавлено)"
}

cmd_deregister() {
    info "Удаление пакета из конфигурации pfSense..."

    STUB_DIR="/tmp/awg-inc-stub-$$"
    mkdir -p "${STUB_DIR}"
    printf '<?php\n' > "${STUB_DIR}/services_dhcp.inc"

    PHP_SCRIPT="/tmp/awg-deregister-$$.php"
    cat > "${PHP_SCRIPT}" << 'PHPEOF'
<?php
set_include_path('/etc/inc' . PATH_SEPARATOR . '/usr/local/share/pear' . PATH_SEPARATOR . ini_get('include_path'));
require_once('globals.inc');
require_once('config.inc');
require_once('/usr/local/pkg/awg.inc');
awg_deinstall();
echo "done" . PHP_EOL;
PHPEOF

    php -d "include_path=${STUB_DIR}:/etc/inc:/usr/local/share/pear" "${PHP_SCRIPT}" || true
    rm -f "${PHP_SCRIPT}"
    rm -rf "${STUB_DIR}"

    ok "Пакет деинсталлирован из config.xml"
}

cmd_remove_files() {
    info "Удаление файлов пакета..."
    rm -f /usr/local/pkg/awg.inc /usr/local/pkg/awg_validate.inc /usr/local/pkg/awg.xml
    rm -f /usr/local/www/vpn_awg_tunnels.php /usr/local/www/vpn_awg_edit.php /usr/local/www/vpn_awg_status.php
    rm -f /usr/local/www/widgets/widgets/awg_status.widget.php /usr/local/www/widgets/widgets/awg_status.xml
    rm -f /etc/inc/priv/awg.priv.inc
    rm -f /usr/local/etc/rc.d/awg.sh
    ok "Файлы удалены"
}

case "${COMMAND}" in
    install)
        info "Установка AmneziaWG Client..."
        cmd_deploy_files
        cmd_register
        echo ""
        info "Установка завершена!"
        echo "  1. Убедитесь, что /usr/local/bin/amneziawg-go и /usr/local/bin/awg на месте"
        echo "     (scripts/install_binaries.sh, если ещё не установлены)"
        echo "  2. Откройте VPN -> AmneziaWG в веб-интерфейсе"
        ;;
    update)
        info "Обновление AmneziaWG Client..."
        service awg stop 2>/dev/null || true
        cmd_deploy_files
        service awg restart 2>/dev/null || true
        ok "Обновление завершено"
    uninstall)
        info "Удаление AmneziaWG Client..."
        service awg stop 2>/dev/null || true
        cmd_deregister
        cmd_remove_files
        ok "Удаление завершено"
        ;;
    *)
        die "Неизвестная команда: ${COMMAND} (используйте install/update/uninstall)"
        ;;
esac