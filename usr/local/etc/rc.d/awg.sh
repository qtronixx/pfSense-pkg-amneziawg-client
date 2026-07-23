#!/bin/sh
#
# PROVIDE: awg
# REQUIRE: NETWORKING SERVERS
# KEYWORD: shutdown
#
# rc.d-скрипт службы AmneziaWG Client для pfSense 2.8.1.
# Управляет userspace-процессами amneziawg-go через функции awg_up()/
# awg_down()/awg_sync_all() из awg.inc.
#
# ОБЯЗАТЕЛЬНО: chmod 555 /usr/local/etc/rc.d/awg.sh после установки.

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
    echo "$(date): awg_start() invoked" >> /var/log/awg-boot.log
    if [ ! -x /usr/local/bin/amneziawg-go ] || [ ! -x /usr/local/bin/awg ]; then
        echo "$(date): бинарники не найдены" >> /var/log/awg-boot.log
        return 1
    fi
    # Запускаем PHP с туннелями
    ( sleep 10 && ${PHP} -r "require_once('${AWGINC}'); awg_sync_all();" >> /var/log/awg-boot.log 2>&1 ) &
    # Мониторинг: пишем состояние каждые 5 секунд, 36 итераций = 3 минуты
    ( for i in $(seq 1 36); do
        sleep 5
        echo "$(date): check $i" >> /var/log/awg-boot.log
        ifconfig tun9000 >> /var/log/awg-boot.log 2>&1
        ifconfig tun9001 >> /var/log/awg-boot.log 2>&1
        ps aux | grep "[a]mneziawg-go" >> /var/log/awg-boot.log 2>&1
      done ) &
}

awg_stop()
{
    echo "Остановка AmneziaWG Client (опускаем все туннели)..."
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