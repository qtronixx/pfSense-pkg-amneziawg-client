#!/bin/sh
#
# PROVIDE: awg
# REQUIRE: NETWORKING SERVERS
# KEYWORD: shutdown
#
# ИЗМЕНЕНО: управляет userspace-процессами amneziawg-go, а не
# кернел-интерфейсами. Логика старта/стопа вынесена в awg.inc
# (awg_up/awg_down), rc.d-скрипт только вызывает awg_sync_all()/
# опускает все туннели - как и раньше.

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
    # daemon(8) уже есть в базовой системе FreeBSD/pfSense - используется
    # для форка amneziawg-go с pid-файлом, отдельной утилиты ставить не надо.
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
