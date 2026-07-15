#!/bin/sh
#
# PROVIDE: awg
# REQUIRE: NETWORKING SERVERS
# KEYWORD: shutdown
#
# rc.d-скрипт AmneziaWG Client для pfSense 2.8.1 (userspace-архитектура,
# amneziawg-go + awg-tools). Проверено практически на реальном сервере,
# 15 июля 2026: handshake, обфускация v2.0 (S1-S4, диапазоны H1-H4, I1),
# маршрутизация - всё подтверждено рабочим.

. /etc/rc.subr

name="awg"
rcvar="awg_enable"
start_cmd="awg_start"
stop_cmd="awg_stop"
status_cmd="awg_status_cmd"

PHP="/usr/local/bin/php"
AWGINC="/usr/local/pkg/awg.inc"

awg_start()
{
    if [ ! -x /usr/local/bin/amneziawg-go ] || [ ! -x /usr/local/bin/awg ]; then
        echo "AmneziaWG Client: бинарники не найдены в /usr/local/bin/, служба не запущена."
        exit 1
    fi
    echo "Запуск AmneziaWG Client (userspace, amneziawg-go)..."
    ${PHP} -r "require_once('${AWGINC}'); awg_sync_all();"
}

awg_stop()
{
    echo "Остановка AmneziaWG Client..."
    ${PHP} -r "
        require_once('${AWGINC}');
        foreach (awg_get_tunnels() as \$t) { awg_down(\$t, true); }
    "
}

awg_status_cmd()
{
    /usr/local/bin/awg show all
    echo '--- запущенные процессы amneziawg-go ---'
    pgrep -lf amneziawg-go
}

load_rc_config $name
: ${awg_enable:="YES"}

run_rc_command "$1"
