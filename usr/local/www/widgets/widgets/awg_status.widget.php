<?php
/*
 * awg_status.widget.php
 * -----------------------------------------------------------------------
 * Мини-виджет для главного дашборда pfSense (Status -> Dashboard),
 * показывает краткую сводку по всем клиентским туннелям AmneziaWG:
 * имя, состояние линка, время последнего handshake.
 * -----------------------------------------------------------------------
 */

declare(strict_types=1);

require_once('/usr/local/pkg/awg.inc');

$awg_widget_tunnels = awg_get_tunnels();
?>
<div class="table-responsive">
    <table class="table table-striped table-hover table-condensed">
        <thead>
            <tr>
                <th><?= gettext('Туннель') ?></th>
                <th><?= gettext('Состояние') ?></th>
            </tr>
        </thead>
        <tbody>
            <?php if (empty($awg_widget_tunnels)): ?>
                <tr><td colspan="2" class="text-muted"><?= gettext('Туннели не настроены') ?></td></tr>
            <?php endif; ?>
            <?php foreach ($awg_widget_tunnels as $t):
                $up = does_interface_exist($t['name']);
            ?>
            <tr>
                <td><?= htmlspecialchars($t['name']) ?> <small class="text-muted"><?= htmlspecialchars($t['descr'] ?? '') ?></small></td>
                <td>
                    <?php if (!empty($t['enabled']) && $up): ?>
                        <span class="label label-success"><?= gettext('подключен') ?></span>
                    <?php elseif (!empty($t['enabled']) && !$up): ?>
                        <span class="label label-danger"><?= gettext('ошибка') ?></span>
                    <?php else: ?>
                        <span class="label label-default"><?= gettext('выключен') ?></span>
                    <?php endif; ?>
                </td>
            </tr>
            <?php endforeach; ?>
        </tbody>
    </table>
    <a href="/vpn_awg_tunnels.php" class="btn btn-xs btn-default">
        <?= gettext('Управление туннелями') ?>
    </a>
</div>
