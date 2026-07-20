<?php
/*
 * vpn_awg_tunnels.php
 * -----------------------------------------------------------------------
 * Главная страница пакета: список настроенных клиентских туннелей
 * AmneziaWG, с возможностью включить/выключить, удалить, применить
 * изменения и перейти к статусу.
 *
 * Совместимость с pfSense 2.8.1 (п.2 ТЗ):
 *   - подключение через $pgtitle/require_once('guiconfig.inc') в
 *     актуальном для 2.8.x стиле (Bootstrap 5 layout, без legacy
 *     табличных вёрсток вручную - используется штатный класс
 *     "table table-striped table-hover table-responsive").
 *   - strict_types и явные приведения типов под PHP 8.2.
 * -----------------------------------------------------------------------
 */

declare(strict_types=1);

require_once('guiconfig.inc');
require_once('/usr/local/pkg/awg.inc');

global $config;

$pgtitle = [gettext('VPN'), gettext('AmneziaWG'), gettext('Туннели')];

$tunnels = awg_get_tunnels();

// включение отладочного вывода в логах.
awg_debug('vpn_awg_tunnels.php: получено туннелей: ' . count($tunnels));
if (!empty($tunnels)) {
    awg_debug('Первый туннель: ' . print_r($tunnels[0], true));
}

/* ---------------------------------------------------------------------
 * Обработка действий: включение/выключение, удаление, применение
 * --------------------------------------------------------------------- */
$act = $_REQUEST['act'] ?? '';
$id  = isset($_REQUEST['id']) ? (int)$_REQUEST['id'] : null;

if ($act === 'toggle' && $id !== null && isset($tunnels[$id])) {
    $tunnels[$id]['enabled'] = empty($tunnels[$id]['enabled']) ? '1' : '';
    awg_save_tunnels($tunnels);
    header('Location: /vpn_awg_tunnels.php');
    exit;
}

if ($act === 'del' && $id !== null && isset($tunnels[$id])) {
    // Перед удалением записи из конфига - аккуратно опускаем интерфейс,
    // чтобы не оставить "висящий" awgN в системе.
    awg_down($tunnels[$id], true);
    @unlink(awg_conf_path($tunnels[$id]['name']));
    unset($tunnels[$id]);
    $tunnels = array_values($tunnels);
    awg_save_tunnels($tunnels);
    header('Location: /vpn_awg_tunnels.php');
    exit;
}

if ($act === 'apply') {
    awg_sync_all();
    $savemsg = gettext('Конфигурация всех туннелей AmneziaWG применена.');
}

include('head.inc');
?>

<body>
<?php include('fbegin.inc'); ?>

<div class="panel panel-default">
    <div class="panel-heading">
        <h2 class="panel-title"><?= gettext('Клиентские туннели AmneziaWG') ?></h2>
    </div>
    <div class="panel-body">

        <?php if (!empty($savemsg)): ?>
            <div class="alert alert-success"><?= htmlspecialchars($savemsg) ?></div>
        <?php endif; ?>

        <div class="alert alert-info">
            <?= gettext('Данный пакет работает только в клиентском режиме: каждый туннель ' .
                        'подключается к внешнему AmneziaWG-серверу. Функции поднятия ' .
                        'собственного сервера AmneziaWG в этом пакете отсутствуют.') ?>
        </div>

        <table class="table table-striped table-hover table-responsive">
            <thead>
                <tr>
                    <th><?= gettext('Включен') ?></th>
                    <th><?= gettext('Имя') ?></th>
                    <th><?= gettext('Peer (сервер)') ?></th>
                    <th><?= gettext('Endpoint') ?></th>
                    <th><?= gettext('Адрес туннеля') ?></th>
                    <th><?= gettext('Jc / Jmin / Jmax') ?></th>
                    <th><?= gettext('Действия') ?></th>
                </tr>
            </thead>
            <tbody>
                <?php if (empty($tunnels)): ?>
                    <tr>
                        <td colspan="7" class="text-center text-muted">
                            <?= gettext('Туннели не настроены.') ?>
                        </td>
                    </tr>
                <?php endif; ?>

                <?php foreach ($tunnels as $i => $t):
                    $peers = $t['peer'] ?? [];
                    if (isset($peers['pubkey'])) {
                        $peers = [$peers];
                    }
                    $first_peer = $peers[0] ?? [];
                    $running = does_interface_exist($t['name']);
                ?>
                <tr>
                    <td>
                        <a href="?act=toggle&amp;id=<?= $i ?>"
                           onclick="return confirm('<?= gettext('Изменить статус включения этого туннеля?') ?>');">
                            <?php if (!empty($t['enabled'])): ?>
                                <i class="fa-solid fa-square-check text-success" title="<?= gettext('Включен') ?>"></i>
                            <?php else: ?>
                                <i class="fa-regular fa-square text-muted" title="<?= gettext('Выключен') ?>"></i>
                            <?php endif; ?>
                        </a>
                    </td>
                    <td>
                        <?= htmlspecialchars($t['name']) ?>
                        <?php if ($running): ?>
                            <span class="label label-success"><?= gettext('запущен') ?></span>
                        <?php else: ?>
                            <span class="label label-default"><?= gettext('остановлен') ?></span>
                        <?php endif; ?>
                    </td>
                    <td><?= htmlspecialchars(substr((string)($first_peer['pubkey'] ?? ''), 0, 16)) ?>&hellip;</td>
                    <td><?= htmlspecialchars(($first_peer['endpoint'] ?? '') . ':' . ($first_peer['endpointport'] ?? '')) ?></td>
                    <td><?= htmlspecialchars($t['address'] ?? '') ?></td>
                    <td>
                        <?= (int)($t['jc'] ?? 0) ?> /
                        <?= (int)($t['jmin'] ?? 0) ?> /
                        <?= (int)($t['jmax'] ?? 0) ?>
                    </td>
                    <td>
                        <a class="fa-solid fa-pencil"
                           title="<?= gettext('Редактировать') ?>"
                           href="vpn_awg_edit.php?id=<?= $i ?>"></a>
                        &nbsp;
                        <a class="fa-solid fa-trash-can text-danger"
                           title="<?= gettext('Удалить') ?>"
                           href="?act=del&amp;id=<?= $i ?>"
                           onclick="return confirm('<?= gettext('Удалить туннель безвозвратно?') ?>');"></a>
                    </td>
                </tr>
                <?php endforeach; ?>
            </tbody>
        </table>

        <nav class="action-buttons">
            <a href="vpn_awg_edit.php" class="btn btn-success">
                <i class="fa-solid fa-plus icon-embed-btn"></i><?= gettext('Добавить туннель') ?>
            </a>
            <a href="?act=apply" class="btn btn-primary">
                <i class="fa-solid fa-check icon-embed-btn"></i><?= gettext('Применить изменения') ?>
            </a>
            <a href="vpn_awg_status.php" class="btn btn-info">
                <i class="fa-solid fa-signal icon-embed-btn"></i><?= gettext('Статус подключений') ?>
            </a>
        </nav>

    </div>
</div>

<?php include('foot.inc'); ?>
