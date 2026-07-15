<?php
/*
 * awg.inc
 * -----------------------------------------------------------------------
 * Backend-библиотека пакета AmneziaWG Client для pfSense 2.8.1.
 *
 * АРХИТЕКТУРА (итоговая, подтверждена практическим тестированием
 * 15 июля 2026 на реальной VM с pfSense 2.8.1 и реальным AmneziaWG-
 * сервером): полностью userspace, через демон amneziawg-go +
 * утилиту awg (amneziawg-tools). Кернел-модуль if_awg.ko НЕ
 * используется - исходники ядра pfSense для веток 2.8.x не
 * публикуются Netgate в открытом доступе, что делает сборку
 * ABI-совместимого модуля невозможной без риска kernel panic.
 *
 * ОГРАНИЧЕНИЕ (клиентский режим, по ТЗ): пакет работает ИСКЛЮЧИТЕЛЬНО
 * как клиент внешнего AmneziaWG-сервера. Каждый "туннель" - это один
 * локальный интерфейс awgN с одним или несколькими peer'ами, где
 * peer обязан иметь Endpoint (внешний сервер), а не ожидать входящих
 * подключений.
 * -----------------------------------------------------------------------
 */

declare(strict_types=1);

require_once('globals.inc');
require_once('util.inc');
require_once('interfaces.inc');
require_once('services.inc');

/* --- Константы --- */
const AWG_XML_ROOT     = 'installedpackages';
const AWG_XML_SECTION  = 'awgclient';
const AWG_IF_PREFIX    = 'awg';
const AWG_CONF_DIR     = '/usr/local/etc/amnezia/amneziawg/';
const AWG_BIN          = '/usr/local/bin/awg';
const AWG_GO_BIN       = '/usr/local/bin/amneziawg-go';
const AWG_RUN_DIR      = '/var/run/amneziawg/';
const AWG_UAPI_DIR     = '/var/run/amneziawg/';   // amneziawg-go кладёт awgN.sock именно сюда

/* --- Обёртки чтения/записи конфига (совместимость pfSense 2.8.x) --- */
function awg_config_get_path(string $path, $default = null)
{
    if (function_exists('config_get_path')) {
        return config_get_path($path, $default);
    }
    global $config;
    $ref = $config;
    foreach (explode('/', $path) as $key) {
        if (!is_array($ref) || !isset($ref[$key])) {
            return $default;
        }
        $ref = $ref[$key];
    }
    return $ref;
}

function awg_config_set_path(string $path, $value): void
{
    if (function_exists('config_set_path')) {
        config_set_path($path, $value);
        return;
    }
    global $config;
    $ref = &$config;
    $keys = explode('/', $path);
    $last = array_pop($keys);
    foreach ($keys as $key) {
        if (!isset($ref[$key]) || !is_array($ref[$key])) {
            $ref[$key] = [];
        }
        $ref = &$ref[$key];
    }
    $ref[$last] = $value;
}

/* --- Работа со списком туннелей в config.xml --- */
function awg_get_tunnels(): array
{
    $tunnels = awg_config_get_path(AWG_XML_ROOT . '/' . AWG_XML_SECTION . '/tunnels/tunnel', []);
    if (isset($tunnels['name'])) {
        $tunnels = [$tunnels];
    }
    return is_array($tunnels) ? $tunnels : [];
}

function awg_save_tunnels(array $tunnels): void
{
    awg_config_set_path(AWG_XML_ROOT . '/' . AWG_XML_SECTION . '/tunnels/tunnel', $tunnels);
    write_config('AmneziaWG Client: обновление конфигурации туннелей');
}

/* --- Генерация ключей --- */
function awg_genkey(): array
{
    $privkey = trim((string) shell_exec(escapeshellcmd(AWG_BIN) . ' genkey'));
    if ($privkey === '') {
        return ['error' => 'Не удалось сгенерировать приватный ключ (проверьте наличие ' . AWG_BIN . ')'];
    }
    $pubkey = trim((string) shell_exec(
        escapeshellcmd(AWG_BIN) . ' pubkey <<< ' . escapeshellarg($privkey)
    ));
    return ['privkey' => $privkey, 'pubkey' => $pubkey];
}

/* --- Пути к рабочим файлам --- */
function awg_conf_path(string $ifname): string
{
    return AWG_CONF_DIR . $ifname . '.conf';
}

function awg_pidfile(string $ifname): string
{
    return AWG_RUN_DIR . $ifname . '.pid';
}

function awg_uapi_sock(string $ifname): string
{
    return AWG_UAPI_DIR . $ifname . '.sock';
}

/*
 * Валидация обфускационных параметров AmneziaWG 2.0.
 * Подтверждено практическим тестом: диапазоны H1-H4 с min >= max
 * ломают весь setconf невнятной ошибкой "Invalid argument" - эта
 * валидация ловит такие опечатки до отправки в awg.
 */
function awg_validate_obfuscation(array $t): array
{
    $errors = [];

    $jc   = (int)($t['jc']   ?? 0);
    $jmin = (int)($t['jmin'] ?? 0);
    $jmax = (int)($t['jmax'] ?? 0);

    if ($jc < 1 || $jc > 128) {
        $errors[] = 'Jc должен быть в диапазоне 1-128.';
    }
    if ($jmin < 0 || $jmin >= $jmax) {
        $errors[] = 'Jmin должен быть меньше Jmax и не отрицательным.';
    }
    if ($jmax > 1280) {
        $errors[] = 'Jmax не должен превышать 1280.';
    }

    foreach (['s1', 's2', 's3', 's4'] as $key) {
        $val = (int)($t[$key] ?? 0);
        if ($val < 0 || $val > 1280) {
            $errors[] = strtoupper($key) . ' вне допустимого диапазона (0-1280).';
        }
    }

    $ranges = [];
    foreach (['h1', 'h2', 'h3', 'h4'] as $key) {
        $raw = trim((string)($t[$key] ?? ''));
        if ($raw === '') {
            $errors[] = strtoupper($key) . ' обязателен.';
            continue;
        }
        if (strpos($raw, '-') !== false) {
            [$min, $max] = array_map('trim', explode('-', $raw, 2));
            if (!ctype_digit($min) || !ctype_digit($max)) {
                $errors[] = strtoupper($key) . ': диапазон должен содержать только числа (формат min-max).';
                continue;
            }
            $min = (int)$min;
            $max = (int)$max;
            if ($min >= $max) {
                $errors[] = strtoupper($key) . ": первое число диапазона ({$min}) должно быть МЕНЬШЕ второго ({$max}). " .
                            "Проверьте, не перепутаны ли значения при копировании.";
                continue;
            }
            if ($max > 2147483647) {
                $errors[] = strtoupper($key) . ': верхняя граница диапазона превышает 2147483647.';
                continue;
            }
            $ranges[$key] = [$min, $max];
        } else {
            if (!ctype_digit($raw)) {
                $errors[] = strtoupper($key) . ': должно быть числом или диапазоном "min-max".';
                continue;
            }
            $ranges[$key] = [(int)$raw, (int)$raw];
        }
    }

    $keys = array_keys($ranges);
    for ($i = 0; $i < count($keys); $i++) {
        for ($j = $i + 1; $j < count($keys); $j++) {
            [$aMin, $aMax] = $ranges[$keys[$i]];
            [$bMin, $bMax] = $ranges[$keys[$j]];
            if ($aMin <= $bMax && $bMin <= $aMax) {
                $errors[] = strtoupper($keys[$i]) . ' и ' . strtoupper($keys[$j]) . ' пересекаются - диапазоны должны быть уникальны.';
            }
        }
    }

    foreach (['i1', 'i2', 'i3', 'i4', 'i5'] as $key) {
        $val = trim((string)($t[$key] ?? ''));
        if ($val === '') {
            continue;
        }
        if (!preg_match('/^(<[a-z]+[^>]*>)+$/i', $val)) {
            $errors[] = strtoupper($key) . ': не похоже на корректный CPS-формат (ожидаются теги вида <b 0x...>, <r N>, <t>...).';
        }
    }

    return $errors;
}

/*
 * Генерация конфиг-файла. I1-I5 пишутся ТОЛЬКО если непустые -
 * подтверждённый баг парсера awg setconf: "Line unrecognized: `I2='"
 * при пустом значении с "=".
 */
function awg_build_conf(array $t): string
{
    $lines   = [];
    $lines[] = '# Автоматически сгенерировано пакетом AmneziaWG Client для pfSense';
    $lines[] = '';
    $lines[] = '[Interface]';
    $lines[] = 'PrivateKey = ' . trim((string)($t['privkey'] ?? ''));

    if (!empty($t['listenport'])) {
        $lines[] = 'ListenPort = ' . (int)$t['listenport'];
    }

    $lines[] = 'Jc = '   . (int)($t['jc']   ?? 4);
    $lines[] = 'Jmin = ' . (int)($t['jmin'] ?? 10);
    $lines[] = 'Jmax = ' . (int)($t['jmax'] ?? 50);

    foreach (['s1', 's2', 's3', 's4'] as $key) {
        $lines[] = strtoupper($key) . ' = ' . (int)($t[$key] ?? 0);
    }
    foreach (['h1', 'h2', 'h3', 'h4'] as $key) {
        $val = trim((string)($t[$key] ?? '1'));
        $lines[] = strtoupper($key) . ' = ' . $val;
    }
    foreach (['i1', 'i2', 'i3', 'i4', 'i5'] as $key) {
        $val = trim((string)($t[$key] ?? ''));
        if ($val !== '') {
            $lines[] = strtoupper($key) . ' = ' . $val;
        }
    }

    if (!empty($t['mtu'])) {
        $lines[] = 'MTU = ' . (int)$t['mtu'];
    }
    $lines[] = '';

    $peers = $t['peer'] ?? [];
    if (isset($peers['pubkey'])) {
        $peers = [$peers];
    }
    foreach ($peers as $peer) {
        if (empty($peer['endpoint'])) {
            log_error('AmneziaWG: peer без Endpoint пропущен - клиентский режим требует внешний адрес сервера.');
            continue;
        }
        $lines[] = '[Peer]';
        $lines[] = 'PublicKey = ' . trim((string)$peer['pubkey']);
        if (!empty($peer['presharedkey'])) {
            $lines[] = 'PresharedKey = ' . trim((string)$peer['presharedkey']);
        }
        $lines[] = 'Endpoint = ' . trim((string)$peer['endpoint']) . ':' . (int)$peer['endpointport'];
        $lines[] = 'AllowedIPs = ' . trim((string)($peer['allowedips'] ?? '0.0.0.0/0, ::/0'));
        if (!empty($peer['keepalive'])) {
            $lines[] = 'PersistentKeepalive = ' . (int)$peer['keepalive'];
        }
        $lines[] = '';
    }

    return implode("\n", $lines) . "\n";
}

function awg_write_conf(array $t): bool
{
    if (!is_dir(AWG_CONF_DIR)) {
        @mkdir(AWG_CONF_DIR, 0700, true);
    }
    $path = awg_conf_path($t['name']);
    $ok = (bool) @file_put_contents($path, awg_build_conf($t));
    if ($ok) {
        @chmod($path, 0600);
        @chown($path, 'root');
    }
    return $ok;
}

/*
 * awg_up() - поднимает туннель через userspace-демон amneziawg-go,
 * применяет конфиг через awg setconf, назначает адрес и ПРОКЛАДЫВАЕТ
 * МАРШРУТЫ под AllowedIPs (подтверждённая практикой находка: без
 * этого handshake проходит, но трафик никуда не идёт).
 */
function awg_up(array $t): array
{
    $ifname = $t['name'];
    $conf   = awg_conf_path($ifname);

    awg_down($t, true);
    awg_write_conf($t);

    if (!is_dir(AWG_RUN_DIR)) {
        @mkdir(AWG_RUN_DIR, 0700, true);
    }

    $log = AWG_RUN_DIR . $ifname . '.log';
    $cmd = sprintf(
        'daemon -f -p %s %s %s >> %s 2>&1',
        escapeshellarg(awg_pidfile($ifname)),
        escapeshellarg(AWG_GO_BIN),
        escapeshellarg($ifname),
        escapeshellarg($log)
    );
    mwexec($cmd, true);

    $sock = awg_uapi_sock($ifname);
    $waited = 0;
    while (!file_exists($sock) && $waited < 5000000) {
        usleep(100000);
        $waited += 100000;
    }
    if (!file_exists($sock)) {
        return ['ok' => false, 'error' => "amneziawg-go не поднял UAPI-сокет для {$ifname}, см. {$log}"];
    }

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

    $peers = $t['peer'] ?? [];
    if (isset($peers['pubkey'])) {
        $peers = [$peers];
    }
    foreach ($peers as $peer) {
        foreach (explode(',', (string)($peer['allowedips'] ?? '')) as $cidr) {
            $cidr = trim($cidr);
            if ($cidr === '') {
                continue;
            }
            if ($cidr === '0.0.0.0/0' || $cidr === '::/0') {
                log_error("AmneziaWG: {$ifname} peer имеет full-tunnel AllowedIPs ({$cidr}) - " .
                           "настройте gateway и policy routing штатно через GUI pfSense " .
                           "(Interfaces -> Assignments, System -> Routing -> Gateways). " .
                           "Автоматический route add для 0.0.0.0/0 пакет намеренно не выполняет.");
                continue;
            }
            $isv6 = strpos($cidr, ':') !== false;
            $famFlag = $isv6 ? '-inet6' : '-inet';
            mwexec("/sbin/route add {$famFlag} -net " . escapeshellarg($cidr) . ' -interface ' . escapeshellarg($ifname), true);
        }
    }

    if (!empty($t['dns'])) {
        awg_apply_dns($ifname, $t['dns']);
    }

    return ['ok' => true];
}

/*
 * awg_down() - останавливает процесс amneziawg-go по pid-файлу,
 * подчищает tun-интерфейс и UAPI-сокет.
 */
function awg_down(array $t, bool $silent = false): void
{
    $ifname  = $t['name'];
    $pidfile = awg_pidfile($ifname);

    if (file_exists($pidfile)) {
        $pid = (int) trim((string) file_get_contents($pidfile));
        if ($pid > 0 && posix_kill($pid, 0)) {
            posix_kill($pid, SIGTERM);
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

/*
 * DNS: не перезаписываем системный resolv.conf автоматически - только
 * предупреждаем в логе, что нужно настроить DNS Resolver вручную.
 * Осознанное решение: авто-перезапись resolv.conf может сломать
 * разрешение имён для остальных служб pfSense.
 */
function awg_apply_dns(string $ifname, string $dns_csv): void
{
    log_error("AmneziaWG: {$ifname} - DNS-серверы ({$dns_csv}) получены из конфига. " .
               "Настройте System -> General Setup -> DNS Servers или DNS Resolver " .
               "Forwarding вручную - автоматическая перезапись resolv.conf в этом " .
               "пакете намеренно не выполняется.");
}

/*
 * Синхронизирует ВСЕ настроенные туннели - используется службой при
 * старте (rc.d), а также кнопкой "Apply changes" в GUI.
 */
function awg_sync_all(): void
{
    foreach (awg_get_tunnels() as $t) {
        if (empty($t['enabled'])) {
            awg_down($t, true);
            continue;
        }
        $result = awg_up($t);
        if (empty($result['ok'])) {
            log_error('AmneziaWG: ошибка поднятия туннеля ' . $t['name'] . ': ' . ($result['error'] ?? 'неизвестная ошибка'));
        }
    }
}

/*
 * install/deinstall хуки пакета. Проверяют наличие обязательных
 * бинарников amneziawg-go/awg - без них служба не заработает, но
 * pkg install не должен падать целиком из-за этого (можно донести
 * бинарники позже).
 */
function awg_install_hook(string $stage = 'post'): void
{
    if (!is_dir(AWG_CONF_DIR)) {
        mkdir(AWG_CONF_DIR, 0700, true);
    }
    if (!is_dir(AWG_RUN_DIR)) {
        mkdir(AWG_RUN_DIR, 0700, true);
    }

    $missing = [];
    if (!file_exists(AWG_GO_BIN) || !is_executable(AWG_GO_BIN)) {
        $missing[] = AWG_GO_BIN;
    }
    if (!file_exists(AWG_BIN) || !is_executable(AWG_BIN)) {
        $missing[] = AWG_BIN;
    }
    if (!empty($missing)) {
        log_error('AmneziaWG Client: ВНИМАНИЕ - не найдены обязательные бинарники: ' .
                   implode(', ', $missing) . '. Пакет установлен, но НЕ будет работать, ' .
                   'пока вы не скопируете amneziawg-go и awg в /usr/local/bin/ ' .
                   '(см. scripts/install_binaries.sh и README.md).');
    } else {
        log_error('AmneziaWG Client: пакет установлен, бинарники найдены, каталоги конфигурации созданы.');
    }
}

function awg_deinstall_hook(string $stage = 'post'): void
{
    foreach (awg_get_tunnels() as $t) {
        awg_down($t, true);
    }
    log_error('AmneziaWG Client: пакет удалён, все туннели awgN остановлены. ' .
               'Бинарники amneziawg-go/awg в /usr/local/bin/ НЕ удалены автоматически.');
}