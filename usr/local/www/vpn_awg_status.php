<?php
/*
 * vpn_awg_status.php
 * -----------------------------------------------------------------------
 * Страница статуса: парсит вывод `awg show all dump` (машиночитаемый
 * формат, аналогичный `wg show all dump`) и рисует аккуратную таблицу
 * по каждому туннелю/пиру - handshake, трафик, endpoint.
 *
 * Формат dump-строки для интерфейса:
 *   ifname  privkey  pubkey  listenport  fwmark
 * Формат dump-строки для peer:
 *   ifname  pubkey  psk  endpoint  allowedips  latest-handshake  rx  tx  keepalive
 * -----------------------------------------------------------------------
 */

declare(strict_types=1);

require_once('guiconfig.inc');
require_once('/usr/local/pkg/awg.inc');

$pgtitle = [gettext('VPN'), gettext('AmneziaWG'), gettext('Статус')];

/*
 * Выполняет `awg show all dump` и разбирает вывод.
 *
 * ИСПРАВЛЕНО (подтверждено реальным выводом 15.07.2026): формат dump
 * для AmneziaWG отличается от ванильного WireGuard - у строки
 * интерфейса 21 поле (privkey, pubkey, listenport, jc, jmin, jmax,
 * s1-s4, h1-h4, i1-i5, advanced-security-flag), поля fwmark НЕТ
 * ВООБЩЕ (в отличие от wg, где оно есть). Строка peer - ровно 9 полей.
 * Различаем по точному количеству колонок, а не диапазону "больше/меньше".
 */
function awg_get_status(): array
{
    $out = [];
    exec(escapeshellcmd(AWG_BIN) . ' show all dump 2>/dev/null', $out);

    $result = [];
    foreach ($out as $line) {
        $cols = explode("\t", $line);
        $ifname = $cols[0] ?? '';
        if ($ifname === '') {
            continue;
        }
        if (!isset($result[$ifname])) {
            $result[$ifname] = ['interface' => [], 'peers' => []];
        }

        // Заменяем строковый "(null)" (буквально выводится awg для
        // незаполненных I2-I5) на пустую строку - иначе на экране
        // будет мусорный текст "(null)" вместо прочерка.
        $nn = fn(string $v): string => ($v === '(null)') ? '' : $v;

        if (count($cols) === 9) {
            // --- Peer ---
            $result[$ifname]['peers'][] = [
                'pubkey'      => $cols[1],
                'psk'         => $cols[2],
                'endpoint'    => $cols[3],
                'allowedips'  => $cols[4],
                'handshake'   => (int)$cols[5],
                'rx'          => (int)$cols[6],
                'tx'          => (int)$cols[7],
                'keepalive'   => $cols[8] ?? '',
            ];
        } elseif (count($cols) >= 20) {
            // --- Интерфейс (AmneziaWG-формат, полный набор обфускации) ---
            $result[$ifname]['interface'] = [
                'privkey'     => $cols[1]  ?? '',
                'pubkey'      => $cols[2]  ?? '',
                'listenport'  => $cols[3]  ?? '',
                'jc'          => $cols[4]  ?? '',
                'jmin'        => $cols[5]  ?? '',
                'jmax'        => $cols[6]  ?? '',
                's1'          => $cols[7]  ?? '',
                's2'          => $cols[8]  ?? '',
                's3'          => $cols[9]  ?? '',
                's4'          => $cols[10] ?? '',
                'h1'          => $cols[11] ?? '',
                'h2'          => $cols[12] ?? '',
                'h3'          => $cols[13] ?? '',
                'h4'          => $cols[14] ?? '',
                'i1'          => $nn($cols[15] ?? ''),
                'i2'          => $nn($cols[16] ?? ''),
                'i3'          => $nn($cols[17] ?? ''),
                'i4'          => $nn($cols[18] ?? ''),
                'i5'          => $nn($cols[19] ?? ''),
            ];
        } else {
            // Неизвестный формат строки - логируем, но не падаем,
            // чтобы будущие изменения формата dump не роняли страницу статуса.
            log_error("AmneziaWG: неожиданный формат строки dump ({$ifname}, " . count($cols) . " полей) - пропущена.");
        }
    }
    return $result;
}
/*
 * Человекочитаемое "N секунд/минут/часов назад" для времени handshake.
 */
function awg_format_handshake(int $ts): string
{
    if ($ts === 0) {
        return gettext('никогда');
    }
    $diff = time() - $ts;
    if ($diff < 60) {
        return sprintf(gettext('%d сек. назад'), $diff);
    }
    if ($diff < 3600) {
        return sprintf(gettext('%d мин. назад'), (int)($diff / 60));
    }
    return sprintf(gettext('%d ч. назад'), (int)($diff / 3600));
}

function awg_format_bytes(int $bytes): string
{
    $units = ['Б', 'КБ', 'МБ', 'ГБ', 'ТБ'];
    $i = 0;
    $val = (float)$bytes;
    while ($val >= 1024 && $i < count($units) - 1) {
        $val /= 1024;
        $i++;
    }
    return sprintf('%.2f %s', $val, $units[$i]);
}

$status = awg_get_status();
$tunnels_cfg = array_column(awg_get_tunnels(), null, 'name');

include('head.inc');
?>
<body>
<?php include('fbegin.inc'); ?>

<div class="panel panel-default">
    <div class="panel-heading">
        <h2 class="panel-title"><?= gettext('Статус подключений AmneziaWG') ?></h2>
    </div>
    <div class="panel-body">

        <a href="vpn_awg_status.php" class="btn btn-default btn-sm pull-right">
            <i class="fa-solid fa-rotate icon-embed-btn"></i><?= gettext('Обновить') ?>
        </a>

        <?php if (empty($status)): ?>
            <div class="alert alert-warning">
                <?= gettext('Ни один туннель AmneziaWG в данный момент не активен.') ?>
            </div>
        <?php endif; ?>

        <?php foreach ($status as $ifname => $data):
            $descr = $tunnels_cfg[$ifname]['descr'] ?? '';
        ?>
        <h3>
            <?= htmlspecialchars($ifname) ?>
            <?php if ($descr): ?><small><?= htmlspecialchars($descr) ?></small><?php endif; ?>
            <span class="label label-success"><?= gettext('активен') ?></span>
        </h3>
        <table class="table table-condensed table-striped">
            <tr>
                <th style="width:220px;"><?= gettext('Публичный ключ') ?></th>
                <td><code><?= htmlspecialchars($data['interface']['pubkey'] ?? '') ?></code></td>
            </tr>
            <tr>
                <th><?= gettext('Порт прослушивания') ?></th>
                <td><?= htmlspecialchars($data['interface']['listenport'] ?? '') ?></td>
            </tr>
            <tr>
                <th><?= gettext('Jc / Jmin / Jmax') ?></th>
                <td>
                    <?= htmlspecialchars($data['interface']['jc'] ?? '') ?> /
                    <?= htmlspecialchars($data['interface']['jmin'] ?? '') ?> /
                    <?= htmlspecialchars($data['interface']['jmax'] ?? '') ?>
                </td>
            </tr>
            <tr>
                <th><?= gettext('S1 / S2 / S3 / S4') ?></th>
                <td>
                    <?= htmlspecialchars($data['interface']['s1'] ?? '') ?> /
                    <?= htmlspecialchars($data['interface']['s2'] ?? '') ?> /
                    <?= htmlspecialchars($data['interface']['s3'] ?? '') ?> /
                    <?= htmlspecialchars($data['interface']['s4'] ?? '') ?>
                </td>
            </tr>
            <tr>
                <th><?= gettext('H1-H4') ?></th>
                <td>
                    <?= htmlspecialchars($data['interface']['h1'] ?? '') ?>,
                    <?= htmlspecialchars($data['interface']['h2'] ?? '') ?>,
                    <?= htmlspecialchars($data['interface']['h3'] ?? '') ?>,
                    <?= htmlspecialchars($data['interface']['h4'] ?? '') ?>
                </td>
            </tr>
        </table>

        <table class="table table-striped table-hover">
            <thead>
                <tr>
                    <th><?= gettext('Peer (сервер)') ?></th>
                    <th><?= gettext('Endpoint') ?></th>
                    <th><?= gettext('AllowedIPs') ?></th>
                    <th><?= gettext('Последний handshake') ?></th>
                    <th><?= gettext('Принято') ?></th>
                    <th><?= gettext('Отправлено') ?></th>
                </tr>
            </thead>
            <tbody>
                <?php foreach ($data['peers'] as $p): ?>
                <tr class="<?= (time() - $p['handshake'] > 180) ? 'text-muted' : '' ?>">
                    <td><code><?= htmlspecialchars(substr($p['pubkey'], 0, 20)) ?>&hellip;</code></td>
                    <td><?= htmlspecialchars($p['endpoint']) ?></td>
                    <td><?= htmlspecialchars($p['allowedips']) ?></td>
                    <td><?= awg_format_handshake($p['handshake']) ?></td>
                    <td><?= awg_format_bytes($p['rx']) ?></td>
                    <td><?= awg_format_bytes($p['tx']) ?></td>
                </tr>
                <?php endforeach; ?>
            </tbody>
        </table>
        <?php endforeach; ?>

    </div>
</div>

<?php include('foot.inc'); ?>
