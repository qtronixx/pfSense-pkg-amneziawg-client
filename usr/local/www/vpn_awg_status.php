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
 * Выполняет `awg show all dump` и разбирает вывод в структуру
 * ['awg0' => ['interface' => [...], 'peers' => [[...], ...]], ...]
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
        // Строка интерфейса содержит 5 колонок, строка peer - 9
        if (count($cols) === 5) {
            $result[$ifname]['interface'] = [
                'privkey'     => $cols[1],
                'pubkey'      => $cols[2],
                'listenport'  => $cols[3],
                'fwmark'      => $cols[4],
            ];
        } elseif (count($cols) >= 8) {
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
