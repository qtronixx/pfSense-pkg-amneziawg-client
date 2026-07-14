<?php
/*
 * awg.inc
 * -----------------------------------------------------------------------
 * Backend-библиотека пакета AmneziaWG Client для pfSense 2.8.1.
 *
 * ОГРАНИЧЕНИЕ (по ТЗ): пакет работает ИСКЛЮЧИТЕЛЬНО в режиме клиента.
 * В отличие от полноценных портов WireGuard/AmneziaWG для pfSense,
 * здесь сознательно отсутствуют:
 *   - генерация серверных ключей "по умолчанию" с ожиданием входящих
 *     подключений от множества пиров;
 *   - функции биндинга на несколько локальных peer-ов как "точка входа";
 *   - DNS/DHCP-хуки для раздачи адресов подключающимся клиентам.
 * Каждый "туннель" в этом пакете = один локальный интерфейс awgN
 * с ОДНИМ или несколькими peer-ами, где peer - это ВНЕШНИЙ сервер,
 * а не клиент, стучащийся к нам.
 *
 * Совместимость с pfSense 2.8.1 / PHP 8.2+:
 *   - используются типизированные параметры функций и strict_types;
 *   - используется современный config_get_path()/config_set_path()
 *     API (пришёл на смену прямому обращению к $config['...'] в
 *     последних ветках pfSense) с fallback на глобальный $config,
 *     если функции недоступны (на случай более старой подветки 2.8.x);
 *   - убраны все конструкции, генерирующие E_DEPRECATED в PHP 8.2
 *     (динамические свойства, передача null в non-nullable параметры
 *     строковых функций и т.д.).
 * -----------------------------------------------------------------------
 */

declare(strict_types=1);

require_once('globals.inc');
require_once('util.inc');
require_once('interfaces.inc');
require_once('services.inc');

/* Префикс, под которым хранится конфигурация в config.xml */
const AWG_XML_ROOT      = 'installedpackages';
const AWG_XML_SECTION   = 'awgclient';   // <installedpackages><awgclient><config>...
const AWG_IF_PREFIX     = 'awg';         // имена интерфейсов: awg0, awg1, ...
const AWG_CONF_DIR      = '/usr/local/etc/amnezia/amneziawg/';
const AWG_BIN           = '/usr/local/bin/awg';
const AWG_RCD_SCRIPT    = '/usr/local/etc/rc.d/awg.sh';

/*
 * Обёртка чтения конфигурации с поддержкой нового API config_get_path()
 * (pfSense 2.8.x) и старого прямого обращения к глобальному $config
 * (на случай отката/иной сборки). Совместимость - требование ТЗ п.2.
 */
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

/*
 * Возвращает массив всех настроенных клиентских туннелей AmneziaWG.
 * Каждый элемент - это одна запись <tunnel> из config.xml.
 */
function awg_get_tunnels(): array
{
    $tunnels = awg_config_get_path(AWG_XML_ROOT . '/' . AWG_XML_SECTION . '/tunnels/tunnel', []);
    // pfSense при одном элементе XML может вернуть ассоц. массив вместо
    // массива массивов - нормализуем к списку.
    if (isset($tunnels['name'])) {
        $tunnels = [$tunnels];
    }
    return is_array($tunnels) ? $tunnels : [];
}

/*
 * Сохраняет список туннелей обратно в config.xml.
 */
function awg_save_tunnels(array $tunnels): void
{
    awg_config_set_path(AWG_XML_ROOT . '/' . AWG_XML_SECTION . '/tunnels/tunnel', $tunnels);
    write_config('AmneziaWG Client: обновление конфигурации туннелей');
}

/*
 * Генерация пары приватный/публичный ключ через штатную утилиту awg.
 * (Аналог "wg genkey | wg pubkey", но бинарём amneziawg-tools).
 */
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

/*
 * Валидация обфускационных параметров AmneziaWG.
 * Возвращает массив ошибок (пустой - если всё ок).
 *
 * Требование ТЗ п.3: полная поддержка Jc, Jmin, Jmax, S1, S2, H1-H4.
 */
function awg_validate_obfuscation(array $t): array
{
    $errors = [];

    $jc   = (int)($t['jc']   ?? 0);
    $jmin = (int)($t['jmin'] ?? 0);
    $jmax = (int)($t['jmax'] ?? 0);
    $s1   = (int)($t['s1']   ?? 0);
    $s2   = (int)($t['s2']   ?? 0);

    if ($jc < 1 || $jc > 128) {
        $errors[] = 'Jc должен быть в диапазоне 1-128.';
    }
    if ($jmin < 0 || $jmin >= $jmax) {
        $errors[] = 'Jmin должен быть меньше Jmax и не отрицательным.';
    }
    if ($jmax > 1280) {
        $errors[] = 'Jmax не должен превышать 1280 (рекомендация upstream AmneziaWG).';
    }
    if ($s1 < 0 || $s1 > 1280) {
        $errors[] = 'S1 вне допустимого диапазона (0-1280).';
    }
    if ($s2 < 0 || $s2 > 1280) {
        $errors[] = 'S2 вне допустимого диапазона (0-1280).';
    }
    if (($s1 + 56) === ($s2 + 56) && $s1 === $s2 && $s1 !== 0) {
        // S1 и S2 не должны совпадать (иначе легче фингерпринтить пакет)
        $errors[] = 'S1 и S2 не должны быть равны друг другу.';
    }

    // H1-H4 - произвольные 32-битные "magic header" значения, но обязаны
    // быть уникальны между собой (это требование самого протокола AWG).
    $h = [
        'h1' => (int)($t['h1'] ?? 0),
        'h2' => (int)($t['h2'] ?? 0),
        'h3' => (int)($t['h3'] ?? 0),
        'h4' => (int)($t['h4'] ?? 0),
    ];
    foreach ($h as $key => $val) {
        if ($val < 1 || $val > 2147483647) {
            $errors[] = strtoupper($key) . ' должен быть в диапазоне 1-2147483647.';
        }
    }
    if (count(array_unique($h)) !== count($h)) {
        $errors[] = 'Значения H1, H2, H3, H4 должны быть уникальны между собой.';
    }

    return $errors;
}
/*
 * Формирует путь к конфиг-файлу интерфейса awgN в формате,
 * который понимает бинарь `awg` (синтаксис в духе wg-quick,
 * но с секциями AmneziaWG-параметров в [Interface]).
 */
function awg_conf_path(string $ifname): string
{
    return AWG_CONF_DIR . $ifname . '.conf';
}

/*
 * Генерирует текстовое содержимое конфиг-файла awgN.conf
 * из массива данных туннеля ($t) - как он хранится в config.xml.
 *
 * ВАЖНО (клиентское ограничение, п.1 ТЗ):
 *   - Address и DNS сознательно НЕ пишутся в файл конфигурации,
 *     а применяются отдельно через ifconfig/resolvconf-хуки pfSense,
 *     как это сделано и в оригинальном ручном гайде track2 (см. Step 7
 *     первоисточника) - на роутере автоматический wg-quick-style
 *     апдейт адреса/маршрутов может увести администратора в оффлайн,
 *     поэтому адресация управляется штатным механизмом pfSense
 *     "Interfaces -> Assignments", а не самим awg-quick.
 *   - Секция [Peer] в этом пакете допускает несколько записей
 *     (мульти-хоп/бэкап-сервер), но каждая обязана иметь Endpoint -
 *     то есть являться ВНЕШНИМ сервером. Полей "AllowedIPs" без
 *     Endpoint (характерных для серверного режима с множеством
 *     подключающихся клиентов) GUI не создаёт в принципе -
 *     соответствующего поля в форме редактирования нет (см. Блок 5).
 */
function awg_build_conf(array $t): string
{
    $lines = [];
    $lines[] = '# Автоматически сгенерировано пакетом AmneziaWG Client для pfSense';
    $lines[] = '# Ручное редактирование этого файла будет перезаписано при следующем сохранении в GUI.';
    $lines[] = '';
    $lines[] = '[Interface]';
    $lines[] = 'PrivateKey = ' . trim((string)($t['privkey'] ?? ''));

    if (!empty($t['listenport'])) {
        $lines[] = 'ListenPort = ' . (int)$t['listenport'];
    }

    // Обфускационные параметры AmneziaWG - обязательный набор по п.3 ТЗ
    $lines[] = 'Jc = '   . (int)($t['jc']   ?? 4);
    $lines[] = 'Jmin = ' . (int)($t['jmin'] ?? 40);
    $lines[] = 'Jmax = ' . (int)($t['jmax'] ?? 70);
    $lines[] = 'S1 = '   . (int)($t['s1']   ?? 0);
    $lines[] = 'S2 = '   . (int)($t['s2']   ?? 0);
    $lines[] = 'H1 = '   . (int)($t['h1']   ?? 1);
    $lines[] = 'H2 = '   . (int)($t['h2']   ?? 2);
    $lines[] = 'H3 = '   . (int)($t['h3']   ?? 3);
    $lines[] = 'H4 = '   . (int)($t['h4']   ?? 4);

    // MTU - опционально, влияет на стабильность при обфускации
    if (!empty($t['mtu'])) {
        $lines[] = 'MTU = ' . (int)$t['mtu'];
    }

    $lines[] = '';

    $peers = $t['peer'] ?? [];
    if (isset($peers['pubkey'])) {
        $peers = [$peers]; // нормализация одиночного peer
    }

    foreach ($peers as $peer) {
        if (empty($peer['endpoint'])) {
            // Жёсткая защита от превращения записи в "серверный" peer:
            // без Endpoint запись пропускается и логируется как ошибка.
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
 * Записывает конфиг на диск с правами 0600 (приватный ключ внутри!).
 */
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
 * Поднимает интерфейс awgN:
 *   1) создаёт netdev через `ifconfig awgN create` (используя if_awg.ko);
 *   2) загружает в него конфиг обфускации/ключей через `awg setconf`;
 *   3) назначает IP-адрес (Address) - ИМЕННО здесь, а не в конфиг-файле,
 *      согласно клиентскому ограничению, описанному в awg_build_conf();
 *   4) поднимает линк.
 *
 * Это прямой аналог ручных шагов 6-9 из оригинального README track2,
 * но выполняемый программно из GUI/service-хука вместо ручного SSH.
 */
function awg_up(array $t): array
{
    $ifname = $t['name'];
    $conf   = awg_conf_path($ifname);

    if (!file_exists($conf)) {
        awg_write_conf($t);
    }

    // Если интерфейс уже существует - сначала аккуратно опускаем
    awg_down($t, /* silent */ true);

    $out = [];
    $rc  = 0;

    mwexec('/sbin/ifconfig ' . escapeshellarg($ifname) . ' create', true);

    exec(
        escapeshellcmd(AWG_BIN) . ' setconf ' . escapeshellarg($ifname) . ' ' . escapeshellarg($conf) . ' 2>&1',
        $out,
        $rc
    );
    if ($rc !== 0) {
        return ['ok' => false, 'error' => 'awg setconf завершился с ошибкой: ' . implode("\n", $out)];
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
 * Опускает и удаляет интерфейс awgN.
 */
function awg_down(array $t, bool $silent = false): void
{
    $ifname = $t['name'];
    if (does_interface_exist($ifname)) {
        mwexec('/sbin/ifconfig ' . escapeshellarg($ifname) . ' destroy', $silent);
    }
}

/*
 * Синхронизирует ВСЕ настроенные туннели: используется службой при
 * старте (rc.d), а также кнопкой "Apply changes" в GUI.
 */
function awg_sync_all(): void
{
    foreach (awg_get_tunnels() as $t) {
        if (empty($t['enabled'])) {
            awg_down($t, true);
            continue;
        }
        awg_write_conf($t);
        awg_up($t);
    }

    // Регистрируем интерфейсы в системе назначения интерфейсов pfSense,
    // чтобы awgN появлялся в Interfaces -> Assignments как обычный порт.
    awg_register_assignable_interfaces();
}

/*
 * Требование п.4 ТЗ ("Web GUI... контроль службы"): регистрация
 * awgN-интерфейсов в pfSense как assignable, чтобы можно было
 * применить firewall-правила, gateway, DNS resolver и т.д. штатными
 * средствами pfSense, не изобретая отдельный слой NAT/Firewall.
 */
function awg_register_assignable_interfaces(): void
{
    $ifdescrs = awg_config_get_path('interfaces', []);
    $tunnels  = awg_get_tunnels();

    foreach ($tunnels as $t) {
        $ifname = $t['name'];
        $already = false;
        foreach ($ifdescrs as $key => $entry) {
            if (($entry['if'] ?? '') === $ifname) {
                $already = true;
                break;
            }
        }
        if (!$already && does_interface_exist($ifname)) {
            // Не назначаем автоматически физически - лишь делаем
            // интерфейс видимым в выпадающем списке "available interfaces".
            // Реальное присвоение имени opt-интерфейса пользователь
            // выполняет вручную на странице Interfaces -> Assignments,
            // как и в оригинальной инструкции (Step 12).
            interfaces_interface_add_number_to_list_if_missing($ifname);
        }
    }
}

/*
 * Хук, вызываемый из pkg-plist при установке пакета (@postexec).
 */
function awg_install_hook(string $stage = 'post'): void
{
    if (!is_dir(AWG_CONF_DIR)) {
        mkdir(AWG_CONF_DIR, 0700, true);
    }
    // Проверка наличия кернел-модуля и автозагрузки при старте системы
    $loader_conf = '/boot/loader.conf';
    $content = file_exists($loader_conf) ? file_get_contents($loader_conf) : '';
    if (strpos($content, 'if_awg_load') === false) {
        file_put_contents($loader_conf, "\nif_awg_load=\"YES\"\n", FILE_APPEND);
    }
    if (!kldstat_module_loaded('if_awg')) {
        mwexec('/sbin/kldload if_awg', true);
    }
    log_error('AmneziaWG Client: пакет установлен, каталог конфигурации создан.');
}

function awg_deinstall_hook(string $stage = 'post'): void
{
    foreach (awg_get_tunnels() as $t) {
        awg_down($t, true);
    }
    log_error('AmneziaWG Client: пакет удалён, все туннели awgN опущены.');
}

/*
 * Небольшой helper - проверка, загружен ли модуль ядра.
 */
function kldstat_module_loaded(string $name): bool
{
    $out = [];
    exec('/sbin/kldstat -q -m ' . escapeshellarg($name), $out, $rc);
    return $rc === 0;
}
