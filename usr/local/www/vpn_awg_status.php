<?php
/*
 * vpn_awg_status.php
 * -----------------------------------------------------------------------
 * ИСПРАВЛЕНИЯ:
 *   - Фильтрация только "своих" туннелей - awg show all опрашивает
 *     ВСЕ WireGuard-совместимые интерфейсы в системе, включая штатный
 *     пакет WireGuard pfSense, если он установлен.
 *   - Визуальный разделитель между блоками туннелей.
 *   - Отображение "дружественного" имени интерфейса pfSense (Interfaces
 *     -> Description) рядом с системным именем tun9NNN.
 *   - Автообновление страницы с переключаемым чекбоксом.
 *   - Отображение MTU интерфейса.
 * -----------------------------------------------------------------------
 */

declare(strict_types=1);

require_once('guiconfig.inc');
require_once('/usr/local/pkg/awg.inc');

$pgtitle = [gettext('VPN'), gettext('AmneziaWG'), gettext('Статус')];


/*
 * Один batched-вызов ifconfig вместо N отдельных exec() в цикле по
 * туннелям - раньше на каждый активный туннель spawn-ился отдельный
 * /bin/sh -c при каждой загрузке страницы статуса.
 */
function awg_get_all_mtu(): array
{
    $all_mtu = [];
    exec('/sbin/ifconfig -a', $ifconfig_all);
    $cur_if = null;
    foreach ($ifconfig_all as $l) {
        if (preg_match('/^(\S+):/', $l, $m)) {
            $cur_if = $m[1];
        }
        if ($cur_if !== null && preg_match('/mtu\s+(\d+)/', $l, $m)) {
            $all_mtu[$cur_if] = $m[1];
        }
    }
    return $all_mtu;
}

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


$status = awg_get_status();
$tunnels_cfg = array_column(awg_get_tunnels(), null, 'name');

// Показываем только интерфейсы, реально управляемые этим пакетом -
// awg show all видит ВСЕ WireGuard-совместимые интерфейсы в системе,
// включая штатный пакет WireGuard pfSense, если он установлен.
$status = array_intersect_key($status, $tunnels_cfg);

$auto_refresh = isset($_GET['autorefresh']) && $_GET['autorefresh'] === '1';

include('head.inc');
?>
<body>
<?php include('fbegin.inc'); ?>

<div class="panel panel-default">
    <div class="panel-heading">
        <h2 class="panel-title"><?= gettext('Статус подключений AmneziaWG') ?></h2>
    </div>
    <div class="panel-body">

        <div class="form-group">
            <label class="checkbox-inline">
                <input type="checkbox" id="autorefresh-toggle" <?= $auto_refresh ? 'checked' : '' ?>>
                <?= gettext('Автообновление каждые 15 секунд') ?>
            </label>
            <a href="vpn_awg_status.php<?= $auto_refresh ? '?autorefresh=1' : '' ?>" class="btn btn-default btn-sm pull-right">
                <i class="fa-solid fa-rotate icon-embed-btn"></i><?= gettext('Обновить сейчас') ?>
            </a>
        </div>

        <?php if (empty($status)): ?>
            <div class="alert alert-warning">
                <?= gettext('Ни один туннель AmneziaWG в данный момент не активен.') ?>
            </div>
        <?php endif; ?>

        <?php
        $all_mtu = awg_get_all_mtu();
        foreach ($status as $ifname => $data):
            $descr = $tunnels_cfg[$ifname]['descr'] ?? '';
            $friendly_name = awg_display_name($ifname);
            $mtu = $all_mtu[$ifname] ?? '';
        ?>
        <div class="panel panel-default" style="border-left: 4px solid #337ab7; margin-top: 15px;">
        <div class="panel-body">
        <h3 style="margin-top:0;">
            <?= htmlspecialchars($ifname) ?>
            <?php if ($friendly_name !== ''): ?>
                <small class="text-muted">(<?= htmlspecialchars($friendly_name) ?>)</small>
            <?php endif; ?>
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
                <th><?= gettext('MTU') ?></th>
                <td><?= htmlspecialchars($mtu) ?></td>
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
        </div>
        </div>
        <?php endforeach; ?>

    </div>
</div>

<script>
(function () {
    var checkbox = document.getElementById('autorefresh-toggle');
    var params = new URLSearchParams(window.location.search);

    checkbox.addEventListener('change', function () {
        if (checkbox.checked) {
            window.location.href = 'vpn_awg_status.php?autorefresh=1';
        } else {
            window.location.href = 'vpn_awg_status.php';
        }
    });

    if (params.get('autorefresh') === '1') {
        setTimeout(function () {
            window.location.href = 'vpn_awg_status.php?autorefresh=1';
        }, 15000);
    }
})();
</script>

<?php include('foot.inc'); ?>