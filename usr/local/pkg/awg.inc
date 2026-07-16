<?php
/*
 * awg.inc
 * -----------------------------------------------------------------------
 * Backend-библиотека пакета AmneziaWG Client для pfSense 2.8.1.
 *
 * АРХИТЕКТУРА: полностью userspace (amneziawg-go + amneziawg-tools),
 * без кернел-модуля - подтверждено практическим тестированием на
 * реальной VM pfSense 2.8.1 с реальным AmneziaWG-сервером (15.07.2026).
 *
 * ОГРАНИЧЕНИЕ (клиентский режим): пакет работает ИСКЛЮЧИТЕЛЬНО как
 * клиент внешнего AmneziaWG-сервера. Каждый peer обязан иметь Endpoint.
 *
 * ИСПРАВЛЕНИЯ ПО ИТОГАМ РЕВЬЮ (16.07.2026):
 *   - awg_genkey(): убран bashism <<< (не работает в /bin/sh на
 *     FreeBSD - там ash), приватный ключ передаётся через stdin
 *     (proc_open), а не через argv (не виден в ps/proc).
 *   - awg_conf_path(): защита от path traversal - строгая валидация
 *     имени интерфейса внутри самой функции, а не только на GUI-форме.
 *   - awg_up()/awg_down(): маршруты, добавленные под AllowedIPs,
 *     теперь отслеживаются в *.routes и корректно удаляются в
 *     awg_down() - раньше накапливались при каждом перезапуске.
 *   - awg_down(): guard на отсутствие расширения posix.
 *   - awg_up(): setconf теперь с retry (защита от гонки, когда
 *     UAPI-сокет создан, но демон ещё не начал accept()).
 *   - awg_save_tunnels(): проверка результата write_config().
 *   - Санитайзинг пользовательских данных перед log_error() - защита
 *     от log injection через символы новой строки.
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
const AWG_UAPI_DIR     = '/var/run/amneziawg/';

/*
 * Убирает переводы строк из значений, которые попадут в log_error() -
 * защита от log injection (найдено в ревью 16.07.2026).
 */
function awg_log_safe(string $v): string
{
    return str_replace(["\r", "\n"], ' ', $v);
}

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

/*
 * ИСПРАВЛЕНО: проверяем результат write_config() - раньше ошибка
 * записи на диск (нет места, нет прав) проходила незамеченной, GUI
 * показывал успех, а изменения терялись при перезагрузке.
 */
function awg_save_tunnels(array $tunnels): bool
{
    awg_config_set_path(AWG_XML_ROOT . '/' . AWG_XML_SECTION . '/tunnels/tunnel', $tunnels);
    $ok = write_config('AmneziaWG Client: обновление конфигурации туннелей');
    if ($ok === false) {
        log_error('AmneziaWG Client: КРИТИЧНО - write_config() вернул ошибку, изменения могли не сохраниться на диск.');
        return false;
    }
    return true;
}

/*
 * ИСПРАВЛЕНО: убран bashism <<<, приватный ключ передаётся через
 * stdin процесса `awg pubkey`, а не через argv (не светится в ps/proc).
 */
function awg_genkey(): array
{
    $privkey = trim((string) shell_exec(escapeshellcmd(AWG_BIN) . ' genkey'));
    if ($privkey === '') {
        return ['error' => 'Не удалось сгенерировать приватный ключ (проверьте наличие ' . AWG_BIN . ')'];
    }

    $descriptors = [0 => ['pipe', 'r'], 1 => ['pipe', 'w'], 2 => ['pipe', 'w']];
    $proc = proc_open(escapeshellcmd(AWG_BIN) . ' pubkey', $descriptors, $pipes);
    if (!is_resource($proc)) {
        return ['error' => 'Не удалось запустить ' . AWG_BIN . ' pubkey'];
    }
    fwrite($pipes[0], $privkey . "\n");
    fclose($pipes[0]);
    $pubkey = trim((string) stream_get_contents($pipes[1]));
    fclose($pipes[1]);
    fclose($pipes[2]);
    proc_close($proc);

    if ($pubkey === '') {
        return ['error' => 'awg pubkey не вернул результат'];
    }

    return ['privkey' => $privkey, 'pubkey' => $pubkey];
}

/*
 * ИСПРАВЛЕНО: защита от path traversal - строгая валидация имени
 * интерфейса ВНУТРИ функции, независимо от того, что делает GUI-форма.
 */
function awg_conf_path(string $ifname): string
{
    if (!preg_match('/^awg[0-9]{1,3}$/', $ifname)) {
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
 * Валидация обфускационных параметров AmneziaWG 2.0.
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
    if (empty($t['name'])) {
        log_error('AmneziaWG: awg_write_conf() вызван без имени интерфейса - отменено.');
        return false;
    }
    if (!is_dir(AWG_CONF_DIR)) {
        @mkdir(AWG_CONF_DIR, 0700, true);
    }
    $path = awg_conf_path($t['name']); // бросит исключение при недопустимом имени
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
 * awg_up() - поднимает туннель. ИСПРАВЛЕНО: setconf теперь с retry
 * (защита от гонки после ожидания UAPI-сокета), маршруты записываются
 * в *.routes для последующей корректной очистки в awg_down().
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

    // ИСПРАВЛЕНО: retry на случай, если сокет создан, но демон ещё
    // не начал accept() (гонка, найденная в ревью).
    $out = [];
    $rc  = 1;
    for ($attempt = 0; $attempt < 3 && $rc !== 0; $attempt++) {
        if ($attempt > 0) {
            usleep(300000);
        }
        $out = [];
        exec(escapeshellcmd(AWG_BIN) . ' setconf ' . escapeshellarg($ifname) . ' ' . escapeshellarg($conf) . ' 2>&1', $out, $rc);
    }
    if ($rc !== 0) {
        return ['ok' => false, 'error' => 'awg setconf (после 3 попыток): ' . implode("\n", $out)];
    }

    if (!empty($t['address'])) {
        mwexec('/sbin/ifconfig ' . escapeshellarg($ifname) . ' inet ' . escapeshellarg($t['address']), true);
    }
    if (!empty($t['address6'])) {
        mwexec('/sbin/ifconfig ' . escapeshellarg($ifname) . ' inet6 ' . escapeshellarg($t['address6']), true);
    }
    mwexec('/sbin/ifconfig ' . escapeshellarg($ifname) . ' up', true);

    // Маршруты под AllowedIPs - ИСПРАВЛЕНО: теперь записываем список
    // добавленных маршрутов в *.routes, чтобы awg_down() мог их удалить.
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
                log_error("AmneziaWG: {$ifname} peer имеет full-tunnel AllowedIPs ({$cidr}) - " .
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
        file_put_contents(awg_routesfile($ifname), implode("\n", $routes_added));
    }

    if (!empty($t['dns'])) {
        awg_apply_dns($ifname, $t['dns']);
    }

    return ['ok' => true];
}

/*
 * awg_down() - ИСПРАВЛЕНО: удаляет маршруты, добавленные в awg_up()
 * (раньше накапливались при каждом перезапуске туннеля). Добавлен
 * guard на отсутствие расширения posix.
 */
function awg_down(array $t, bool $silent = false): void
{
    $ifname  = $t['name'];
    $pidfile = awg_pidfile($ifname);

    if (file_exists($pidfile)) {
        $pid = (int) trim((string) file_get_contents($pidfile));
        if ($pid > 0) {
            if (function_exists('posix_kill')) {
                if (posix_kill($pid, 0)) {
                    posix_kill($pid, SIGTERM);
                    usleep(1000000);
                    if (posix_kill($pid, 0)) {
                        posix_kill($pid, SIGKILL);
                    }
                }
            } else {
                // Guard из ревью: posix-расширение может отсутствовать -
                // используем kill(1) как fallback вместо фатальной ошибки.
                mwexec('/bin/kill ' . escapeshellarg((string)$pid), true);
            }
        }
        @unlink($pidfile);
    }

    // Удаляем маршруты, добавленные при последнем awg_up() для этого туннеля.
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
    foreach (awg_get_tunnels() as $t) {
        if (empty($t['enabled'])) {
            awg_down($t, true);
            continue;
        }
        $result = awg_up($t);
        if (empty($result['ok'])) {
            log_error('AmneziaWG: ошибка поднятия туннеля ' . awg_log_safe($t['name']) . ': ' . awg_log_safe($result['error'] ?? 'неизвестная ошибка'));
        }
    }
}

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
                   implode(', ', $missing) . '. Скопируйте их в /usr/local/bin/ (scripts/install_binaries.sh).');
    } else {
        log_error('AmneziaWG Client: пакет установлен, бинарники найдены, каталоги конфигурации созданы.');
    }
}

function awg_deinstall_hook(string $stage = 'post'): void
{
    foreach (awg_get_tunnels() as $t) {
        awg_down($t, true);
    }
    log_error('AmneziaWG Client: пакет удалён, все туннели awgN остановлены. Бинарники в /usr/local/bin/ НЕ удалены автоматически.');
}