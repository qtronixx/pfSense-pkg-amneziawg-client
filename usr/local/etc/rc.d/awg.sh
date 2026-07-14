#!/bin/sh
#
# PROVIDE: awg
# REQUIRE: NETWORKING SERVERS
# KEYWORD: shutdown
#
# rc.d-скрипт службы AmneziaWG Client для pfSense 2.8.1.
#
# Заменяет собой ручной Shellcmd-хак из оригинальной инструкции
# track2 (Step 11): там из-за отсутствия нормального rc.d-скрипта
# автозапуск приходилось городить через сторонний пакет Shellcmd
# с earlyshellcmd. Здесь мы регистрируем полноценный rc.d-сервис,
# который pfSense подхватывает штатно на этапе NETWORKING - до того,
# как происходит назначение интерфейсов, что и требовалось.
#
# обязательно сделать - chmod 555 /usr/local/etc/rc.d/awg.sh

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
    echo "Запуск AmneziaWG Client (синхронизация всех туннелей)..."
    ${PHP} -f ${AWGINC} -- --sync-all 2>/dev/null
    ${PHP} -r "require_once('${AWGINC}'); awg_sync_all();"
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
}

load_rc_config $name
: ${awg_enable:="YES"}

run_rc_command "$1"
