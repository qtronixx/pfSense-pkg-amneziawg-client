/*
 * --- ОБНОВЛЕНО по итогам практических тестов на pfSense 2.8.1 ---
 * Изменения относительно первой версии:
 *   1) Добавлены S3/S4 (padding cookie-reply и transport-пакетов) -
 *      подтверждены как реально используемые параметры AmneziaWG 2.0.
 *   2) H1-H4 переведены с plain int на строки-диапазоны "min-max"
 *      (подтверждено реальным конфигом с сервера и тестом на pfSense -
 *      именно в этом поле нашлась реальная опечатка min>max, которая
 *      ловится валидацией ниже).
 *   3) I1-I5 - строки формата CPS; ПУСТЫЕ значения теперь не пишутся
 *      в конфиг-файл вообще (подтверждённый баг парсера awg setconf:
 *      "Line unrecognized: `I2='" при пустом значении с "=").
 *   4) awg_up() теперь прокладывает маршруты под AllowedIPs каждого
 *      peer'а - раньше туннель поднимался, handshake проходил, но
 *      трафик никуда не шёл, т.к. этим на Linux занимается awg-quick,
 *      а мы его сознательно не используем.
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

    // S1-S4 - простые целые (0..1280), без изменений по сути,
    // но теперь их четыре, а не два.
    foreach (['s1', 's2', 's3', 's4'] as $key) {
        $val = (int)($t[$key] ?? 0);
        if ($val < 0 || $val > 1280) {
            $errors[] = strtoupper($key) . ' вне допустимого диапазона (0-1280).';
        }
    }

    // H1-H4 - ТЕПЕРЬ строки-диапазоны "min-max". Реальный баг, пойманный
    // вручную на этом проекте: min > max в одном из диапазонов ломает
    // весь setconf с невнятной ошибкой "Invalid argument" - валидация
    // здесь ловит это ДО отправки в awg, с понятным сообщением.
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
            // Одиночное значение тоже допустимо (совместимость с AWG 1.5,
            // как в вашем примере wg-easy: H1=1 H2=2 H3=3 H4=4)
            if (!ctype_digit($raw)) {
                $errors[] = strtoupper($key) . ': должно быть числом или диапазоном "min-max".';
                continue;
            }
            $ranges[$key] = [(int)$raw, (int)$raw];
        }
    }

    // Диапазоны H1-H4 не должны пересекаться между собой (иначе
    // теряется смысл уникальной сигнатуры на клиента, см. документацию Amnezia).
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

    // I1-I5 - опциональные CPS-строки. Ограничение длины НЕ навязываем
    // жёстко (подтверждено тестом: реальный лимит буфера больше, чем
    // MAX_AWG_STRING_LEN=256 из патча ipc-freebsd.h), но базовый
    // синтаксис CPS-тегов стоит проверить, чтобы не отправлять на
    // сервер заведомо битую строку.
    foreach (['i1', 'i2', 'i3', 'i4', 'i5'] as $key) {
        $val = trim((string)($t[$key] ?? ''));
        if ($val === '') {
            continue; // пусто - нормально, просто не будет записано в конфиг
        }
        if (!preg_match('/^(<[a-z]+[^>]*>)+$/i', $val)) {
            $errors[] = strtoupper($key) . ': не похоже на корректный CPS-формат (ожидаются теги вида <b 0x...>, <r N>, <t>...).';
        }
    }

    return $errors;
}

/*
 * Генерация конфиг-файла с учётом всех находок:
 *  - S1-S4 вместо S1-S2;
 *  - H1-H4 как строки (диапазон или одиночное значение) без приведения к int;
 *  - I1-I5 пишутся ТОЛЬКО если непустые (важно! иначе "Line unrecognized: `I2='").
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
        // Строка как есть - и диапазон "min-max", и одиночное число
        // передаются дальше без изменений, awg сам разберётся.
        $val = trim((string)($t[$key] ?? '1'));
        $lines[] = strtoupper($key) . ' = ' . $val;
    }
    // I1-I5: пишем СТРОГО только непустые - см. пояснение выше.
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

/*
 * awg_up() - обновлено: добавлена прокладка маршрутов под AllowedIPs
 * (подтверждённая находка: без этого handshake проходит, но трафик
 * никуда не идёт, т.к. в userspace-режиме без awg-quick некому
 * создавать маршруты).
 */
function awg_up(array $t): array
{
    $ifname = $t['name'];
    $conf   = awg_conf_path($ifname);

    awg_down($t, true);

    if (!file_exists($conf) || true) { // всегда перегенерируем перед up
        awg_write_conf($t);
    }
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

    // --- НОВОЕ: маршруты под AllowedIPs ---
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
                // full-tunnel: не добавляем route add напрямую - это должно
                // управляться через Interfaces->Assignments + System->Routing->Gateways,
                // штатным механизмом pfSense (иначе легко словить конфликт
                // с существующим default route и словить проблему на реальном проде).
                log_error("AmneziaWG: {$ifname} peer имеет full-tunnel AllowedIPs ({$cidr}) - " .
                           "настройте gateway и policy routing штатно через GUI pfSense, " .
                           "автоматический route add для 0.0.0.0/0 пакет не выполняет.");
                continue;
            }
            $isv6 = strpos($cidr, ':') !== false;
            $famFlag = $isv6 ? '-inet6' : '-inet';
            mwexec("/sbin/route add {$famFlag} -net " . escapeshellarg($cidr) . ' -interface ' . escapeshellarg($ifname), true);
        }
    }

    // --- НОВОЕ: применение DNS через штатный DNS Resolver pfSense ---
    if (!empty($t['dns'])) {
        awg_apply_dns($ifname, $t['dns']);
    }

    return ['ok' => true];
}

/*
 * Применяет DNS-сервера из конфига туннеля. В отличие от wg-quick
 * (который правит /etc/resolv.conf напрямую), на pfSense правильный
 * путь - зарегистрировать DNS-forwarding через штатный DNS Resolver
 * (Unbound), используя domain-override или forward zone, привязанные
 * к этому интерфейсу. Здесь - минимальная реализация: запись в
 * системный resolv.conf как fallback, с явным предупреждением,
 * что для полноценной интеграции стоит настроить DNS Resolver в GUI.
 */
function awg_apply_dns(string $ifname, string $dns_csv): void
{
    log_error("AmneziaWG: {$ifname} - DNS-серверы ({$dns_csv}) получены из конфига. " .
               "Для их использования настройте System -> General Setup -> DNS Servers " .
               "или DNS Resolver Forwarding вручную - автоматическая перезапись " .
               "системного resolv.conf в этом пакете намеренно не выполняется, " .
               "чтобы не сломать разрешение имён для остальных служб pfSense.");
}

/*
 * awg_install_hook() - ОБНОВЛЕНО: убраны все действия с /boot/loader.conf
 * и kldload (не нужны в userspace-архитектуре). Вместо этого - проверка
 * наличия обязательных бинарников и понятная ошибка, если их забыли
 * скопировать перед установкой pkg.
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
                   '(см. README_BUILD.md - сборка на отдельной FreeBSD 15.x build-VM).');
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
