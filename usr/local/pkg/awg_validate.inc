<?php
/*
 * awg_validate.inc
 * -----------------------------------------------------------------------
 * Функции валидации входных данных формы редактирования туннеля.
 * Вынесены отдельно от awg.inc, чтобы GUI-страницы могли подключать
 * только валидацию, не таща за собой весь backend (снижает нагрузку
 * при простом рендере формы). Требование PHP 8.2+ strictness (п.2 ТЗ)
 * учтено: везде явные типы и без "мягких" сравнений (==) там, где это
 * может дать неожиданный результат (0 == "abc" и т.п. в PHP8 уже не
 * актуально, но для читаемости используем строгое === везде).
 * -----------------------------------------------------------------------
 */

declare(strict_types=1);

require_once('util.inc');

function awg_validate_tunnel_form(array $pconfig): array
{
    $input_errors = [];

    if (empty($pconfig['name']) || !preg_match('/^awg[0-9]{1,3}$/', (string)$pconfig['name'])) {
        $input_errors[] = 'Имя интерфейса должно быть в формате awgN (например, awg0).';
    }

    if (empty($pconfig['privkey'])) {
        $input_errors[] = 'Приватный ключ обязателен (сгенерируйте или вставьте свой).';
    } elseif (!awg_is_valid_key((string)$pconfig['privkey'])) {
        $input_errors[] = 'Приватный ключ имеет неверный формат (ожидается base64, 44 символа).';
    }

    if (!empty($pconfig['address']) && !is_subnet((string)$pconfig['address'])) {
        $input_errors[] = 'Адрес туннеля (IPv4) указан в неверном формате (ожидается CIDR, например 10.0.14.88/24).';
    }

    if (!empty($pconfig['address6']) && !is_subnetv6((string)$pconfig['address6'])) {
        $input_errors[] = 'Адрес туннеля (IPv6) указан в неверном формате.';
    }

    if (!empty($pconfig['listenport']) && !is_port((string)$pconfig['listenport'])) {
        $input_errors[] = 'ListenPort должен быть корректным номером порта (1-65535).';
    }
    
    if (!empty($pconfig['dns'])) {
        foreach (explode(',', (string)$pconfig['dns']) as $srv) {
            $srv = trim($srv);
            if ($srv !== '' && !is_ipaddr($srv)) {
                $input_errors[] = "DNS-сервер '{$srv}' указан в неверном формате.";
            }
        }
    }

    // Пиры - обязателен минимум 1 внешний сервер (клиентское ограничение).
    $peers = $pconfig['peer'] ?? [];
    if (isset($peers['pubkey'])) {
        $peers = [$peers];
    }
    if (empty($peers)) {
        $input_errors[] = 'Необходимо указать минимум одного peer (внешний AmneziaWG-сервер).';
    }

    foreach ($peers as $i => $peer) {
        $n = $i + 1;
        if (empty($peer['pubkey']) || !awg_is_valid_key((string)$peer['pubkey'])) {
            $input_errors[] = "Peer #{$n}: некорректный публичный ключ сервера.";
        }
        if (empty($peer['endpoint'])) {
            $input_errors[] = "Peer #{$n}: обязательно укажите Endpoint (адрес внешнего AmneziaWG-сервера). ".
                               "Пиры без Endpoint не поддерживаются - пакет работает только в клиентском режиме.";
        } elseif (!is_ipaddr((string)$peer['endpoint']) && !is_hostname((string)$peer['endpoint'])) {
            $input_errors[] = "Peer #{$n}: Endpoint должен быть корректным IP-адресом или доменным именем.";
        }
        if (empty($peer['endpointport']) || !is_port((string)$peer['endpointport'])) {
            $input_errors[] = "Peer #{$n}: некорректный порт Endpoint.";
        }
        if (!empty($peer['allowedips'])) {
            foreach (explode(',', (string)$peer['allowedips']) as $cidr) {
                $cidr = trim($cidr);
                if ($cidr === '') {
                    continue;
                }
                if (!is_subnet($cidr) && !is_subnetv6($cidr)) {
                    $input_errors[] = "Peer #{$n}: AllowedIPs содержит некорректную подсеть '{$cidr}'.";
                }
            }
        }
        if (!empty($peer['keepalive']) && (!ctype_digit((string)$peer['keepalive']) || (int)$peer['keepalive'] > 65535)) {
            $input_errors[] = "Peer #{$n}: PersistentKeepalive должен быть числом секунд (0-65535).";
        }
    }

    // Обфускационные параметры AmneziaWG - переиспользуем awg_validate_obfuscation() из awg.inc
    if (function_exists('awg_validate_obfuscation')) {
        $input_errors = array_merge($input_errors, awg_validate_obfuscation($pconfig));
    }

    return $input_errors;
}

/*
 * Проверка формата base64 ключа WireGuard/AmneziaWG (32 байта -> 44 символа base64 с '=' на конце).
 */
function awg_is_valid_key(string $key): bool
{
    return (bool) preg_match('/^[A-Za-z0-9+\/]{43}=$/', trim($key));
}
