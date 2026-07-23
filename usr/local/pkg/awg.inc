<?php
/*
 * awg.inc
 * -----------------------------------------------------------------------
 * Backend-библиотека пакета AmneziaWG Client для pfSense 2.8.1.
 *
 * ИТОГОВАЯ АРХИТЕКТУРА (после всех практических находок):
 *   - Полностью userspace (amneziawg-go + amneziawg-tools), без
 *     кернел-модуля - исходники ядра pfSense 2.8.x не публикуются.
 *   - Установка/регистрация меню - через install.sh + awg_install(),
 *     без FreeBSD pkg и без install_package_xml()/info.xml.
 *   - Список ТУННЕЛЕЙ хранится в СОБСТВЕННОМ JSON-файле
 *     (AWG_TUNNELS_FILE), а НЕ в config.xml - обнаружен воспроизводимый
 *     баг: write_config()/config_write_file() в этой сборке pfSense
 *     не сериализует вложенную структуру installedpackages/awgclient/
 *     tunnels/tunnel ни через config_set_path(), ни через прямое
 *     присвоение $GLOBALS['config'] - при этом простые записи
 *     (installedpackages/menu, installedpackages/package) сохраняются
 *     нормально, поэтому ИХ по-прежнему пишем через config_set_path().
 *   - Адрес/MTU/gateway интерфейса НЕ назначаются этим кодом напрямую
 *     через ifconfig inet - это конфликтовало с конфигурацией самого
 *     pfSense (Interfaces -> AWGCLIENT -> Static IPv4) и ломало
 *     NAT/gateway monitoring. Вместо этого после создания tun9NNN
 *     вызывается штатная interface_configure() pfSense, которая
 *     применяет уже сохранённую в pfSense конфигурацию интерфейса.
 *   - НЕ подключаем services.inc - на 2.8.1 может требовать
 *     services_dhcp.inc, которого нет без пакета DHCP.
 *   - Добавлен парсер файла для автоматической подставновки параметров
 *
 * ОГРАНИЧЕНИЕ: пакет работает ИСКЛЮЧИТЕЛЬНО как клиент внешнего
 * AmneziaWG-сервера. Каждый peer обязан иметь Endpoint.
 * -----------------------------------------------------------------------
 */

declare(strict_types=1);

require_once('globals.inc');
require_once('util.inc');
require_once('interfaces.inc');

/* --- Константы --- */
const AWG_XML_ROOT     = 'installedpackages';
const AWG_XML_SECTION  = 'awgclient';
const AWG_IF_PREFIX    = 'tun';
const AWG_IF_NUM_BASE = 9000; // отступ, чтобы не пересекаться с обычными tunN
const AWG_CONF_DIR     = '/usr/local/etc/amnezia/amneziawg/';
const AWG_BIN          = '/usr/local/bin/awg';
const AWG_GO_BIN       = '/usr/local/bin/amneziawg-go';
const AWG_RUN_DIR      = '/var/run/amneziawg/';
const AWG_UAPI_DIR     = '/var/run/amneziawg/';
const AWG_TUNNELS_FILE = '/usr/local/etc/amnezia/amneziawg/tunnels.json';
// ИСПРАВЛЕНО: была AWG_DEBUG, но awg_debug_enabled() ссылался на
// AWG_DEBUG_FLAG_FILE - несовпадение имени константы. Переименовано
// в AWG_DEBUG_FLAG_FILE везде для консистентности.
const AWG_DEBUG_FLAG_FILE = '/usr/local/etc/amnezia/amneziawg/debug.enabled';
const AWG_SETCONF_RETRY_COUNT    = 3;
const AWG_SETCONF_RETRY_DELAY_US = 300000;

/* --- Логирование/отладка --- */
function awg_log_safe(string $v): string
{
    return str_replace(["\r", "\n"], ' ', $v);
}

function awg_debug_enabled(): bool
{
    return file_exists(AWG_DEBUG_FLAG_FILE);
}

/*
 * Включает/выключает отладочное логирование через флаг-файл.
 * Используется чекбоксом на странице VPN -> AmneziaWG -> Туннели.
 */
function awg_set_debug(bool $enable): bool
{
    if ($enable) {
        $dir = dirname(AWG_DEBUG_FLAG_FILE);
        if (!is_dir($dir)) {
            @mkdir($dir, 0700, true);
        }
        return @touch(AWG_DEBUG_FLAG_FILE) !== false;
    }
    if (file_exists(AWG_DEBUG_FLAG_FILE)) {
        return (bool) @unlink(AWG_DEBUG_FLAG_FILE);
    }
    return true;
}

function awg_debug(string $msg): void
{
    if (awg_debug_enabled()) {
        log_error('AmneziaWG DEBUG: ' . awg_log_safe($msg));
    }
}

/*
 * Возвращает реальное состояние соединения, а не просто факт
 * существования tun-интерфейса. 'stopped' - выключен/не создан,
 * 'connecting' - интерфейс есть, но handshake ещё не было или устарел,
 * 'connected' - недавний handshake хотя бы с одним peer.
 */
function awg_connection_state(string $ifname): string
{
    if (!does_interface_exist($ifname)) {
        return 'stopped';
    }

    $out = [];
    exec(escapeshellcmd(AWG_BIN) . ' show ' . escapeshellarg($ifname) . ' latest-handshakes 2>/dev/null', $out);
    if (empty($out)) {
        return 'connecting';
    }

    $freshest = 0;
    foreach ($out as $line) {
        $parts = preg_split('/\s+/', trim($line));
        if (count($parts) >= 2 && ctype_digit($parts[1])) {
            $freshest = max($freshest, (int)$parts[1]);
        }
    }

    // "Свежий" handshake - в пределах ~3 интервалов keepalive (по умолчанию 25с -> 180с с запасом)
    if ($freshest > 0 && (time() - $freshest) < 180) {
        return 'connected';
    }
    return 'connecting';
}

/* --- Кодирование CPS-строк (I1-I5) для безопасного хранения --- */
function awg_encode_cps(string $v): string
{
    return $v === '' ? '' : base64_encode($v);
}

function awg_decode_cps(string $v): string
{
    return $v === '' ? '' : (string) base64_decode($v);
}

/* --- Обёртки для installedpackages/menu и installedpackages/package
       (ЭТИ узлы по-прежнему пишутся через config.xml - подтверждено
       рабочим; используются ТОЛЬКО в awg_install()/awg_deinstall()) --- */
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

/*
 * Миграция имён интерфейсов awgN -> tunNNNN (найдено критично: pfSense
 * не пропускает "awgN" через is_interface_mismatch() при загрузке -
 * имя интерфейса должно совпадать с regex в этой функции, включающим
 * ^tun/^wg/^ovpn и т.д. Без этого интерфейс считается "физически
 * отсутствующим" при ребуте, что приводит к экрану Interface Mismatch
 * и требует ручного переназначения интерфейсов - критично для
 * headless/удалённых инсталляций.
 */
function awg_migrate_interface_names(): void
{
    $tunnels = awg_get_tunnels();
    $changed = false;
    $used_nums = [];

    foreach ($tunnels as $t) {
        if (preg_match('/^tun9([0-9]{3})$/', $t['name'] ?? '', $m)) {
            $used_nums[(int)$m[1]] = true;
        }
    }

    foreach ($tunnels as &$t) {
        if (preg_match('/^awg([0-9]{1,3})$/', $t['name'] ?? '')) {
            $old_name = $t['name'];
            $n = 0;
            while (isset($used_nums[$n])) {
                $n++;
            }
            $new_name = 'tun9' . str_pad((string)$n, 3, '0', STR_PAD_LEFT);
            $used_nums[$n] = true;

            log_error("AmneziaWG: миграция имени интерфейса {$old_name} -> {$new_name} " .
                       "(старый формат несовместим с проверкой pfSense при загрузке).");

            // Переносим .conf-файл, если существует
            $old_conf = AWG_CONF_DIR . $old_name . '.conf';
            if (file_exists($old_conf)) {
                @rename($old_conf, AWG_CONF_DIR . $new_name . '.conf');
            }

            $t['name'] = $new_name;
            $changed = true;
        }
    }
    unset($t);

    if ($changed) {
        awg_save_tunnels($tunnels);
        log_error('AmneziaWG: миграция имён интерфейсов завершена. ' .
                   'ВАЖНО: перейдите в Interfaces -> Assignments и назначьте новые tunN-интерфейсы заново, ' .
                   'старые записи awgN нужно удалить вручную из Assignments.');
    }
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

/* --- Хранение туннелей в собственном JSON-файле --- */
function awg_get_tunnels(): array
{
    if (!file_exists(AWG_TUNNELS_FILE)) {
        return [];
    }
    $raw = file_get_contents(AWG_TUNNELS_FILE);
    if ($raw === false || trim($raw) === '') {
        return [];
    }
    $data = json_decode($raw, true);
    if (json_last_error() !== JSON_ERROR_NONE) {
        log_error('AmneziaWG Client: КРИТИЧНО - tunnels.json повреждён (' . json_last_error_msg() . '). ' .
                   'Возвращён пустой список для GUI, но файл на диске НЕ был перезаписан. ' .
                   'Проверьте ' . AWG_TUNNELS_FILE . ' и ' . AWG_TUNNELS_FILE . '.bak вручную.');
        return [];
    }
    return is_array($data) ? $data : [];
}

function awg_save_tunnels(array $tunnels): bool
{
    if (!is_dir(dirname(AWG_TUNNELS_FILE))) {
        mkdir(dirname(AWG_TUNNELS_FILE), 0700, true);
    }

    if (file_exists(AWG_TUNNELS_FILE)) {
        @copy(AWG_TUNNELS_FILE, AWG_TUNNELS_FILE . '.bak');
    }

    $json = json_encode($tunnels, JSON_PRETTY_PRINT | JSON_UNESCAPED_SLASHES);
    if ($json === false) {
        log_error('AmneziaWG Client: КРИТИЧНО - не удалось сериализовать туннели в JSON: ' . json_last_error_msg());
        return false;
    }

    $lock = fopen(AWG_TUNNELS_FILE . '.lock', 'c');
    if ($lock === false) {
        log_error('AmneziaWG Client: КРИТИЧНО - не удалось создать lock-файл для записи туннелей.');
        return false;
    }
    flock($lock, LOCK_EX);

    $tmp = AWG_TUNNELS_FILE . '.' . getmypid();
    $write_ok = @file_put_contents($tmp, $json) !== false;
    if ($write_ok) {
        @chmod($tmp, 0600);
        $write_ok = rename($tmp, AWG_TUNNELS_FILE);
    }

    flock($lock, LOCK_UN);
    fclose($lock);

    if (!$write_ok) {
        @unlink($tmp);
        log_error('AmneziaWG Client: КРИТИЧНО - не удалось записать tunnels.json.');
        return false;
    }
    return true;
}

/*
 * Генерация ключей через stdin (не argv - не светится в ps/proc) и
 * без bashism <<< (не работает в /bin/sh на FreeBSD - там ash).
 *
 * ИСПРАВЛЕНО: было "if (if ($proc === false))" - двойной if,
 * синтаксическая ошибка. Также убран дублирующийся блок чтения
 * $pipes[1]/$pipes[2] - раньше пайпы читались и закрывались дважды
 * (второе чтение уже закрытых дескрипторов).
 */
function awg_genkey(): array
{
    $privkey = trim((string) shell_exec(escapeshellcmd(AWG_BIN) . ' genkey'));
    if ($privkey === '') {
        return ['error' => 'Не удалось сгенерировать приватный ключ (проверьте наличие ' . AWG_BIN . ')'];
    }

    $descriptors = [0 => ['pipe', 'r'], 1 => ['pipe', 'w'], 2 => ['pipe', 'w']];
    $proc = proc_open(escapeshellcmd(AWG_BIN) . ' pubkey', $descriptors, $pipes);
    if ($proc === false) {
        return ['error' => 'Не удалось запустить ' . AWG_BIN . ' pubkey'];
    }
    fwrite($pipes[0], $privkey . "\n");
    fclose($pipes[0]);

    $pubkey = trim((string) stream_get_contents($pipes[1]));
    fclose($pipes[1]);
    $stderr = trim((string) stream_get_contents($pipes[2]));
    fclose($pipes[2]);
    proc_close($proc);

    if ($pubkey === '') {
        return ['error' => 'awg pubkey не вернул результат' . ($stderr !== '' ? (': ' . $stderr) : '')];
    }

    return ['privkey' => $privkey, 'pubkey' => $pubkey];
}

/* --- Пути, с защитой от path traversal --- */
function awg_conf_path(string $ifname): string
{
    if (!preg_match('/^tun9[0-9]{3}$/', $ifname)) {
        throw new InvalidArgumentException("AmneziaWG: недопустимое имя интерфейса '{$ifname}'");
    }
    return AWG_CONF_DIR . $ifname . '.conf';
}

function awg_pidfile(string $ifname): string
{
    return AWG_RUN_DIR . $ifname . '.pid';
}

function awg_routesfile(string $ifname): string
{
    return AWG_RUN_DIR . $ifname . '.routes';
}

function awg_uapi_sock(string $ifname): string
{
    return AWG_UAPI_DIR . $ifname . '.sock';
}

/*
 * Ищет "дружественное" имя интерфейса (opt1, wan, lan...) в
 * конфигурации pfSense по физическому имени устройства (tun9NNN) - нужно
 * для interface_configure(), которая принимает дружественное имя.
 */
function awg_find_pfsense_interface_name(string $ifname): ?string
{
    $interfaces = awg_config_get_path('interfaces', []);
    foreach ($interfaces as $friendly => $entry) {
        if (($entry['if'] ?? '') === $ifname) {
            return $friendly;
        }
    }
    return null;
}

/* --- Валидация обфускации AmneziaWG 2.0 --- */
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
                $errors[] = strtoupper($key) . ": первое число диапазона ({$min}) должно быть МЕНЬШЕ второго ({$max}).";
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
            $errors[] = strtoupper($key) . ': не похоже на корректный CPS-формат.';
        }
    }

    return $errors;
}

/*
 * Генерация .conf-файла для awg setconf.
 * ВАЖНО: MTU здесь НЕ пишется - awg не понимает это поле в файле
 * ("Line unrecognized: `MTU=1420'"), MTU применяется отдельно через
 * pfSense (Interfaces -> AWGCLIENT -> MTU) + interface_configure().
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
        $val = awg_decode_cps(trim((string)($t[$key] ?? '')));
        if ($val !== '') {
            $lines[] = strtoupper($key) . ' = ' . $val;
        }
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
    if (empty($t['name'])) {
        log_error('AmneziaWG: awg_write_conf() вызван без имени интерфейса - отменено.');
        return false;
    }
    if (!is_dir(AWG_CONF_DIR)) {
        @mkdir(AWG_CONF_DIR, 0700, true);
    }
    $path = awg_conf_path($t['name']);
    $ok = (bool) @file_put_contents($path, awg_build_conf($t));
    if ($ok) {
        @chmod($path, 0600);
        @chown($path, 'root');
    } else {
        log_error("AmneziaWG: не удалось записать конфиг {$path} - проверьте права и свободное место на диске.");
    }
    return $ok;
}

/*
 * awg_up() - поднимает туннель. Адрес/MTU/gateway - НЕ отсюда, а через
 * interface_configure() (см. шапку файла).
 */
function awg_up(array $t): array
{
    $ifname = $t['name'];
    $conf   = awg_conf_path($ifname);

    awg_down($t, true);
    awg_write_conf($t);

    if (!is_dir(AWG_RUN_DIR)) {
        mkdir(AWG_RUN_DIR, 0700, true);
    }

    $log = AWG_RUN_DIR . $ifname . '.log';
    $cmd = sprintf(
    // ИСПРАВЛЕНО: убран флаг -f - он означает "перенаправить stdio
    // дочернего процесса в /dev/null" (см. man daemon(8)), а НЕ
    // "форк"/detach (daemon(8) отделяется от управляющего терминала
    // всегда, без флагов). С -f мы теряли ВСЕ реальные логи и ошибки
    // amneziawg-go, включая критичные крэши - именно поэтому .log
    // файлы демона всегда были пустыми на протяжении всей отладки.
    'daemon -p %s %s %s >> %s 2>&1',
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
    $rc  = 1;
    for ($attempt = 0; $attempt < AWG_SETCONF_RETRY_COUNT && $rc !== 0; $attempt++) {
        if ($attempt > 0) {
            usleep(AWG_SETCONF_RETRY_DELAY_US);
        }
        $out = [];
        exec(escapeshellcmd(AWG_BIN) . ' setconf ' . escapeshellarg($ifname) . ' ' . escapeshellarg($conf) . ' 2>&1', $out, $rc);
    }
    if ($rc !== 0) {
        return ['ok' => false, 'error' => 'awg setconf (после ' . AWG_SETCONF_RETRY_COUNT . ' попыток): ' . implode("\n", $out)];
    }

    mwexec('/sbin/ifconfig ' . escapeshellarg($ifname) . ' up', true);

    // Адрес/MTU/gateway - через штатный pfSense, единый источник истины.
    $friendly = awg_find_pfsense_interface_name($ifname);
    if ($friendly !== null) {
        if (function_exists('interface_configure')) {
            interface_configure($friendly, true);
            awg_debug("interface_configure({$friendly}) вызван для применения адреса/gateway из настроек pfSense.");
        } else {
            log_error("AmneziaWG: функция interface_configure() недоступна - адрес нужно назначить вручную.");
        }
    } else {
        log_error("AmneziaWG: интерфейс {$ifname} ещё не назначен в Interfaces -> Assignments - адрес не будет применён автоматически.");
    }

    // Маршруты под AllowedIPs (только split-tunnel, не 0.0.0.0/0 - см. шапку).
    $peers = $t['peer'] ?? [];
    if (isset($peers['pubkey'])) {
        $peers = [$peers];
    }
    $routes_added = [];
    foreach ($peers as $peer) {
        foreach (explode(',', (string)($peer['allowedips'] ?? '')) as $cidr) {
            $cidr = trim($cidr);
            if ($cidr === '') {
                continue;
            }
            if ($cidr === '0.0.0.0/0' || $cidr === '::/0') {
                log_error("AmneziaWG: " . awg_log_safe($ifname) . " peer имеет full-tunnel AllowedIPs (" . awg_log_safe($cidr) . ") - " .
                            "настройте gateway и policy routing штатно через GUI pfSense.");
                continue;
            }
            $isv6 = strpos($cidr, ':') !== false;
            $famFlag = $isv6 ? '-inet6' : '-inet';
            mwexec("/sbin/route add {$famFlag} -net " . escapeshellarg($cidr) . ' -interface ' . escapeshellarg($ifname), true);
            $routes_added[] = ($isv6 ? '6' : '4') . ':' . $cidr;
        }
    }
    if (!empty($routes_added)) {
        $routes_written = @file_put_contents(awg_routesfile($ifname), implode("\n", $routes_added));
        if ($routes_written === false) {
            log_error("AmneziaWG: КРИТИЧНО - не удалось записать {$ifname}.routes - при следующем awg_down() " .
                       "маршруты НЕ будут удалены автоматически, потребуется ручная очистка через route delete.");
        }
    }

    if (!empty($t['dns'])) {
        awg_apply_dns($ifname, $t['dns']);
    }

    return ['ok' => true];
}

/*
 * awg_down() - с fallback через pgrep на случай потери pid-файла
 * (найденная на практике проблема с "зомби"-процессами).
 */
function awg_down(array $t, bool $silent = false): void
{
    $ifname  = $t['name'];
    $pidfile = awg_pidfile($ifname);
    $killed  = false;

    if (file_exists($pidfile)) {
        $pid = (int) trim((string) file_get_contents($pidfile));
        if ($pid > 0 && function_exists('posix_kill') && posix_kill($pid, 0)) {
            posix_kill($pid, SIGTERM);
            usleep(1000000);
            if (posix_kill($pid, 0)) {
                posix_kill($pid, SIGKILL);
            }
            $killed = true;
        }
        @unlink($pidfile);
    }

    if (!$killed) {
        $out = [];
        exec('/bin/pgrep -f ' . escapeshellarg('amneziawg-go ' . $ifname), $out);
        foreach ($out as $pid) {
            $pid = (int) trim($pid);
            if ($pid > 0) {
                mwexec('/bin/kill ' . escapeshellarg((string)$pid), true);
            }
        }
        if (!empty($out)) {
            usleep(1000000);
            $still = [];
            exec('/bin/pgrep -f ' . escapeshellarg('amneziawg-go ' . $ifname), $still);
            foreach ($still as $pid) {
                $pid = (int) trim($pid);
                if ($pid > 0) {
                    mwexec('/bin/kill -9 ' . escapeshellarg((string)$pid), true);
                }
            }
        }
    }

    $routes_file = awg_routesfile($ifname);
    if (file_exists($routes_file)) {
        $lines = file($routes_file, FILE_IGNORE_NEW_LINES | FILE_SKIP_EMPTY_LINES);
        foreach ($lines as $line) {
            $parts = explode(':', $line, 2);
            if (count($parts) !== 2) {
                continue;
            }
            [$fam, $cidr] = $parts;
            $famFlag = $fam === '6' ? '-inet6' : '-inet';
            mwexec("/sbin/route delete {$famFlag} -net " . escapeshellarg($cidr), true);
        }
        @unlink($routes_file);
    }

    if (does_interface_exist($ifname)) {
        mwexec('/sbin/ifconfig ' . escapeshellarg($ifname) . ' destroy', $silent);
    }
    @unlink(awg_uapi_sock($ifname));
}

function awg_apply_dns(string $ifname, string $dns_csv): void
{
    log_error("AmneziaWG: " . awg_log_safe($ifname) . " - DNS-серверы (" . awg_log_safe($dns_csv) . ") получены из конфига. " .
               "Настройте System -> General Setup -> DNS Servers или DNS Resolver Forwarding вручную.");
}

function awg_sync_all(): void
{
    awg_migrate_interface_names();
    foreach (awg_get_tunnels() as $t) {
        if (empty($t['enabled'])) {
            try {
                awg_down($t, true);
            } catch (Throwable $e) {
                log_error('AmneziaWG: ошибка awg_down() для ' . awg_log_safe($t['name'] ?? '?') . ': ' . awg_log_safe($e->getMessage()));
            }
            continue;
        }
        try {
            $result = awg_up($t);
            if (empty($result['ok'])) {
                log_error('AmneziaWG: ошибка поднятия туннеля ' . awg_log_safe($t['name']) . ': ' . awg_log_safe($result['error'] ?? 'неизвестная ошибка'));
            }
        } catch (Throwable $e) {
            log_error('AmneziaWG: КРИТИЧНО - исключение при поднятии туннеля ' . awg_log_safe($t['name'] ?? '?') .
                       ': ' . awg_log_safe($e->getMessage()) . ' - остальные туннели продолжают обрабатываться.');
        }
    }
}

/*
 * awg_install()/awg_deinstall() - самодостаточная регистрация ТОЛЬКО
 * менюшки и записи о пакете (это работает через config.xml надёжно).
 * Туннели сюда не относятся - они в JSON.
 */
function awg_install(): void
{
    if (!is_dir(AWG_CONF_DIR)) {
        mkdir(AWG_CONF_DIR, 0700, true);
    }
    if (!is_dir(AWG_RUN_DIR)) {
        mkdir(AWG_RUN_DIR, 0700, true);
    }

    $menu = awg_config_get_path('installedpackages/menu', []);
    $already = false;
    foreach ($menu as $entry) {
        if (($entry['name'] ?? '') === 'AmneziaWG') {
            $already = true;
            break;
        }
    }
    if (!$already) {
        $menu[] = [
            'name'        => 'AmneziaWG',
            'tooltiptext' => 'Настройка клиентских туннелей AmneziaWG',
            'section'     => 'VPN',
            'url'         => '/vpn_awg_tunnels.php',
        ];
        awg_config_set_path('installedpackages/menu', $menu);
    }

    $pkg_id = 'AmneziaWG-client';
    $installed = awg_config_get_path('installedpackages/package', []);
    $found = false;
    foreach ($installed as $p) {
        if (($p['name'] ?? '') === $pkg_id) {
            $found = true;
            break;
        }
    }
    if (!$found) {
        $installed[] = [
            'name'    => $pkg_id,
            'descr'   => 'AmneziaWG VPN Client (client-only) for pfSense 2.8.1',
            'version' => '1.0.0',
            'website' => 'https://github.com/qtronixx/pfSense-pkg-amneziawg-client',
        ];
        awg_config_set_path('installedpackages/package', $installed);
    }

    $ok = write_config('AmneziaWG Client: пакет установлен, меню зарегистрировано.');
    if ($ok === false) {
        log_error('AmneziaWG Client: КРИТИЧНО - не удалось записать config.xml при установке.');
    }

    $missing = [];
    if (!file_exists(AWG_GO_BIN) || !is_executable(AWG_GO_BIN)) {
        $missing[] = AWG_GO_BIN;
    }
    if (!file_exists(AWG_BIN) || !is_executable(AWG_BIN)) {
        $missing[] = AWG_BIN;
    }
    if (!empty($missing)) {
        log_error('AmneziaWG Client: ВНИМАНИЕ - не найдены обязательные бинарники: ' . implode(', ', $missing));
    } else {
        log_error('AmneziaWG Client: установка завершена, бинарники найдены, меню зарегистрировано.');
    }
}

function awg_deinstall(): void
{
    foreach (awg_get_tunnels() as $t) {
        awg_down($t, true);
    }

    $menu = awg_config_get_path('installedpackages/menu', []);
    $menu = array_values(array_filter($menu, fn($e) => ($e['name'] ?? '') !== 'AmneziaWG'));
    awg_config_set_path('installedpackages/menu', $menu);

    $installed = awg_config_get_path('installedpackages/package', []);
    $installed = array_values(array_filter($installed, fn($p) => ($p['name'] ?? '') !== 'AmneziaWG-client'));
    awg_config_set_path('installedpackages/package', $installed);

    write_config('AmneziaWG Client: пакет удалён, меню и туннели очищены.');
    log_error('AmneziaWG Client: деинсталляция завершена.');
}

/*
 * awg_parse_conf() - парсер нативного .conf-файла AmneziaWG (обратная
 * функция к awg_build_conf()). Используется для импорта готового
 * конфига через форму редактирования - пользователь вставляет текст
 * или загружает файл, мы раскладываем его по полям формы.
 *
 * ВАЖНО: возвращает I1-I5 ЧИТАЕМЫМ текстом (та же конвенция, что и у
 * $pconfig везде в форме) - кодирование в base64 происходит один раз,
 * непосредственно при формировании $entry перед awg_save_tunnels().
 */
function awg_parse_conf(string $text): array
{
    $result = [
        'privkey'    => '',
        'address'    => '',
        'address6'   => '',
        'listenport' => '',
        'dns'        => '',
        'jc' => '', 'jmin' => '', 'jmax' => '',
        's1' => '0', 's2' => '0', 's3' => '0', 's4' => '0',
        'h1' => '', 'h2' => '', 'h3' => '', 'h4' => '',
        'i1' => '', 'i2' => '', 'i3' => '', 'i4' => '', 'i5' => '',
        'peer' => [],
    ];

    $section = null;
    $current_peer = null;
    $peers = [];

    $lines = preg_split('/\r\n|\r|\n/', $text);
    foreach ($lines as $line) {
        $line = trim($line);
        if ($line === '' || $line[0] === '#' || $line[0] === ';') {
            continue;
        }

        if (preg_match('/^\[(\w+)\]$/', $line, $m)) {
            $section = strtolower($m[1]);
            if ($section === 'peer') {
                if ($current_peer !== null) {
                    $peers[] = $current_peer;
                }
                $current_peer = [
                    'pubkey' => '', 'presharedkey' => '', 'endpoint' => '',
                    'endpointport' => '51820', 'allowedips' => '0.0.0.0/0, ::/0',
                    'keepalive' => '25',
                ];
            }
            continue;
        }

        $eq = strpos($line, '=');
        if ($eq === false) {
            continue;
        }
        $key = strtolower(trim(substr($line, 0, $eq)));
        $val = trim(substr($line, $eq + 1));

        if ($section === 'interface') {
            switch ($key) {
                case 'privatekey':   $result['privkey']    = $val; break;
                case 'address':      $result['address']    = $val; break;
                case 'dns':          $result['dns']        = $val; break;
                case 'listenport':   $result['listenport'] = $val; break;
                case 'jc':           $result['jc']   = $val; break;
                case 'jmin':         $result['jmin'] = $val; break;
                case 'jmax':         $result['jmax'] = $val; break;
                case 's1': case 's2': case 's3': case 's4':
                    $result[$key] = $val;
                    break;
                case 'h1': case 'h2': case 'h3': case 'h4':
                    $result[$key] = $val;
                    break;
                case 'i1': case 'i2': case 'i3': case 'i4': case 'i5':
                    $result[$key] = $val;
                    break;
                // mtu - намеренно игнорируем, см. комментарий в awg_build_conf()
            }
        } elseif ($section === 'peer' && $current_peer !== null) {
            switch ($key) {
                case 'publickey':
                    $current_peer['pubkey'] = $val;
                    break;
                case 'presharedkey':
                    $current_peer['presharedkey'] = $val;
                    break;
                case 'endpoint':
                    $val_trim = trim($val);
                    $last_colon = strrpos($val_trim, ':');
                    if ($last_colon !== false && ctype_digit(substr($val_trim, $last_colon + 1))) {
                        $current_peer['endpoint']     = substr($val_trim, 0, $last_colon);
                        $current_peer['endpointport'] = substr($val_trim, $last_colon + 1);
                    } else {
                        // Порт не указан явно в Endpoint - оставляем текущий/дефолтный,
                        // весь Endpoint трактуем как хост (защита от бага: раньше IP
                        // без порта становился портом).
                        $current_peer['endpoint']     = $val_trim;
                        $current_peer['endpointport'] = $current_peer['endpointport'] ?: '51820';
                    }
                    break;
                case 'allowedips':
                    $current_peer['allowedips'] = $val;
                    break;
                case 'persistentkeepalive':
                    $current_peer['keepalive'] = $val;
                    break;
            }
        }
    }
    if ($current_peer !== null) {
        $peers[] = $current_peer;
    }

    if (!empty($peers)) {
        $result['peer'] = $peers;
    }

    return $result;
}