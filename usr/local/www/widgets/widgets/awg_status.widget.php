<?php
/*
 * awg_status.widget.php
 * -----------------------------------------------------------------------
 * Мини-виджет для главного дашборда pfSense (Status -> Dashboard),
 * по образцу штатного виджета WireGuard: имя туннеля, дружественное
 * описание интерфейса, состояние (стрелка вверх/вниз), порт
 * прослушивания, суммарный RX/TX по всем peer'ам.
 * -----------------------------------------------------------------------
 */

declare(strict_types=1);

require_once('/usr/local/pkg/awg.inc');

$awg_widget_tunnels = awg_get_tunnels();
$awg_widget_status  = awg_get_status();
?>
<div class="table-responsive">
    <table class="table table-striped table-hover table-condensed">
        <thead>
            <tr>
                <th><?= gettext('Туннель') ?></th>
                <th><?= gettext('Описание') ?></th>
                <th><?= gettext('Порт') ?></th>
                <th><?= gettext('RX') ?></th>
                <th><?= gettext('TX') ?></th>
            </tr>
        </thead>
        <tbody>
            <?php if (empty($awg_widget_tunnels)): ?>
                <tr><td colspan="5" class="text-muted"><?= gettext('Туннели не настроены') ?></td></tr>
            <?php endif; ?>
            <?php foreach ($awg_widget_tunnels as $t):
                $ifname = $t['name'];
                $up = does_interface_exist($ifname);
                $data = $awg_widget_status[$ifname] ?? null;
                $friendly = awg_display_name($ifname);

                $rx = 0;
                $tx = 0;
                if ($data !== null) {
                    foreach ($data['peers'] as $p) {
                        $rx += $p['rx'];
                        $tx += $p['tx'];
                    }
                }
            ?>
            <tr>
                <td>
                    <?php if ($up): ?>
                        <i class="fa-solid fa-arrow-up text-success"></i>
                    <?php else: ?>
                        <i class="fa-solid fa-arrow-down text-danger"></i>
                    <?php endif; ?>
                    <?= htmlspecialchars($ifname) ?>
                </td>
                <td><?= htmlspecialchars($friendly !== '' ? $friendly : ($t['descr'] ?? '')) ?></td>
                <td><?= htmlspecialchars($data['interface']['listenport'] ?? '') ?></td>
                <td><?= $up ? awg_format_bytes($rx) : '-' ?></td>
                <td><?= $up ? awg_format_bytes($tx) : '-' ?></td>
            </tr>
            <?php endforeach; ?>
        </tbody>
    </table>
    <a href="/vpn_awg_tunnels.php" class="btn btn-xs btn-default">
        <?= gettext('Управление туннелями') ?>
    </a>
</div>