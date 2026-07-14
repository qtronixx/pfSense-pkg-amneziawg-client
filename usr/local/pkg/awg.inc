/*
 * --- ИЗМЕНЕНИЕ АРХИТЕКТУРЫ: userspace-режим через amneziawg-go ---
 * Причина: кастомизированные исходники ядра pfSense для веток 2.8.x
 * Netgate не публикует в открытом доступе (подтверждено на форуме
 * Netgate), поэтому сборка ABI-совместимого if_awg.ko против
 * актуального билда pfSense 2.8.1 сейчас невозможна без риска
 * kernel panic от несовпадения структур ядра. Userspace-режим
 * этого риска лишён полностью: amneziawg-go работает поверх штатного
 * /dev/tun, который уже есть в базовой системе pfSense.
 */

const AWG_GO_BIN   = '/usr/local/bin/amneziawg-go';
const AWG_RUN_DIR  = '/var/run/amneziawg/';   // pid-файлы + UAPI-сокеты
const AWG_UAPI_DIR = '/var/run/wireguard/';   // amneziawg-go кладёт сюда awgN.sock

function awg_pidfile(string $ifname): string
{
    return AWG_RUN_DIR . $ifname . '.pid';
}

function awg_uapi_sock(string $ifname): string
{
    return AWG_UAPI_DIR . $ifname . '.sock';
}

/*
 * Поднимает туннель через userspace-демон amneziawg-go.
 * Заменяет прежний awg_up(), работавший через ifconfig create + kldload.
 */
function awg_up(array $t): array
{
    $ifname = $t['name'];
    $conf   = awg_conf_path($ifname);

    awg_down($t, true); // на всякий случай гасим старый процесс/интерфейс

    if (!file_exists($conf)) {
        awg_write_conf($t);
    }
    if (!is_dir(AWG_RUN_DIR)) {
        @mkdir(AWG_RUN_DIR, 0700, true);
    }

    /*
     * Запускаем amneziawg-go в фоне. WG_LOG_LEVEL=error - чтобы не
     * забивать журнал pfSense debug-выводом на каждый junk-пакет.
     * LOG-файл пишем отдельно на туннель для диагностики.
     */
    $log = AWG_RUN_DIR . $ifname . '.log';
    $cmd = sprintf(
        'daemon -f -p %s /usr/bin/env WG_LOG_LEVEL=error %s %s >> %s 2>&1',
        escapeshellarg(awg_pidfile($ifname)),
        escapeshellarg(AWG_GO_BIN),
        escapeshellarg($ifname),
        escapeshellarg($log)
    );
    mwexec($cmd, true);

    // Ждём появления UAPI-сокета - демону нужно немного времени на инициализацию tun.
    $sock = awg_uapi_sock($ifname);
    $waited = 0;
    while (!file_exists($sock) && $waited < 5000000) {
        usleep(100000);
        $waited += 100000;
    }
    if (!file_exists($sock)) {
        return ['ok' => false, 'error' => "amneziawg-go не поднял UAPI-сокет для {$ifname} за 5 секунд, см. {$log}"];
    }

    // Конфигурируем через штатный awg (userspace tools собираются тем же crossport-репо)
    $out = [];
    $rc  = 0;
    exec(escapeshellcmd(AWG_BIN) . ' setconf ' . escapeshellarg($ifname) . ' ' . escapeshellarg($conf) . ' 2>&1', $out, $rc);
    if ($rc !== 0) {
        return ['ok' => false, 'error' => 'awg setconf: ' . implode("\n", $out)];
    }

    if (!empty($t['address'])) {
        mwexec('/sbin/ifconfig ' . escapeshellarg($ifname) . ' inet ' . escapeshellarg($t['address']), true);
    }
    if (!empty($t['address6'])) {
        mwexec('/sbin/ifconfig ' . escapeshellarg($ifname) . ' inet6 ' . escapeshellarg($t['address6']), true);
    }
    mwexec('/sbin/ifconfig ' . escapeshellarg($ifname) . ' up', true);

    return ['ok' => true];
}

/*
 * Останавливает процесс amneziawg-go по pid-файлу и подчищает
 * tun-интерфейс, если он остался висеть после завершения процесса
 * (на некоторых версиях FreeBSD tun-интерфейс не удаляется
 * автоматически при завершении владеющего процесса).
 */
function awg_down(array $t, bool $silent = false): void
{
    $ifname = $t['name'];
    $pidfile = awg_pidfile($ifname);

    if (file_exists($pidfile)) {
        $pid = (int) trim((string) file_get_contents($pidfile));
        if ($pid > 0 && posix_kill($pid, 0)) {
            posix_kill($pid, SIGTERM);
            // Даём процессу секунду на грациозное завершение
            usleep(1000000);
            if (posix_kill($pid, 0)) {
                posix_kill($pid, SIGKILL);
            }
        }
        @unlink($pidfile);
    }

    if (does_interface_exist($ifname)) {
        mwexec('/sbin/ifconfig ' . escapeshellarg($ifname) . ' destroy', $silent);
    }
    @unlink(awg_uapi_sock($ifname));
}
