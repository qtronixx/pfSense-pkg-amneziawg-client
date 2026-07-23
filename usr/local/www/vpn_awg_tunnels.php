<?php
/*
 * vpn_awg_tunnels.php
 * -----------------------------------------------------------------------
 * ИСПРАВЛЕНИЯ:
 *   - toggle/del/apply переведены с GET на POST (CSRF-защита через
 *     встроенный csrf-magic pfSense).
 *   - $id строго проверяется через ctype_digit() перед int.
 *   - Удаление туннеля обёрнуто в try/catch - битое имя интерфейса
 *     (InvalidArgumentException из awg_conf_path()) больше не мешает
 *     удалению записи из tunnels.json.
 *   - Добавлен переключатель отладочного логирования (флаг-файл,
 *     переживает install.sh update, в отличие от прежней константы).
 * -----------------------------------------------------------------------
 */

declare(strict_types=1);

require_once('guiconfig.inc');
require_once('/usr/local/pkg/awg.inc');

global $config;

$pgtitle = [gettext('VPN'), gettext('AmneziaWG'), gettext('Туннели')];

$tunnels = awg_get_tunnels();
awg_debug('vpn_awg_tunnels.php: получено туннелей: ' . count($tunnels));

/* ---------------------------------------------------------------------
 * Обработка действий: только через POST, с валидацией id
 * --------------------------------------------------------------------- */
$act = '';
$id  = null;

if ($_SERVER['REQUEST_METHOD'] === 'POST') {
    $act = (string)($_POST['act'] ?? '');
    $raw_id = (string)($_POST['id'] ?? '');
    if (ctype_digit($raw_id)) {
        $id = (int)$raw_id;
    }
}

if ($act === 'toggle' && $id !== null && isset($tunnels[$id])) {
    $tunnels[$id]['enabled'] = empty($tunnels[$id]['enabled']) ? '1' : '';
    awg_save_tunnels($tunnels);
    header('Location: /vpn_awg_tunnels.php');
    exit;
}

if ($act === 'del' && $id !== null && isset($tunnels[$id])) {
    try {
        awg_down($tunnels[$id], true);
    } catch (Throwable $e) {
        log_error('AmneziaWG: ошибка при остановке туннеля перед удалением (' .
                   awg_log_safe($tunnels[$id]['name'] ?? '?') . '): ' . awg_log_safe($e->getMessage()));
    }

    try {
        @unlink(awg_conf_path($tunnels[$id]['name']));
    } catch (Throwable $e) {
        log_error('AmneziaWG: не удалось удалить .conf-файл при удалении туннеля: ' . awg_log_safe($e->getMessage()));
    }

    // Запись из tunnels.json удаляется ВСЕГДА, даже если имя было
    // некорректным - иначе битая запись навсегда "залипает" в списке.
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

if ($act === 'debug_on') {
    awg_set_debug(true);
    header('Location: /vpn_awg_tunnels.php');
    exit;
}

if ($act === 'debug_off') {
    awg_set_debug(false);
    header('Location: /vpn_awg_tunnels.php');
    exit;
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
                    $conn_state = awg_connection_state($t['name']);
                ?>
                <tr>
                    <td>
                        <form method="post" style="display:inline">
                            <input type="hidden" name="act" value="toggle">
                            <input type="hidden" name="id" value="<?= (int)$i ?>">
                            <button type="submit" class="btn btn-link" style="padding:0;border:0;"
                                    onclick="return confirm('<?= gettext('Изменить статус включения этого туннеля?') ?>');">
                                <?php if (!empty($t['enabled'])): ?>
                                    <i class="fa-solid fa-square-check text-success" title="<?= gettext('Включен') ?>"></i>
                                <?php else: ?>
                                    <i class="fa-regular fa-square text-muted" title="<?= gettext('Выключен') ?>"></i>
                                <?php endif; ?>
                            </button>
                        </form>
                    </td>
                    <td>
                        <?= htmlspecialchars($t['name']) ?>
                        <?php if ($conn_state === 'connected'): ?>
                            <span class="label label-success"><?= gettext('подключен') ?></span>
                        <?php elseif ($conn_state === 'connecting'): ?>
                            <span class="label label-warning"><?= gettext('устанавливается соединение...') ?></span>
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
                           href="vpn_awg_edit.php?id=<?= (int)$i ?>"></a>
                        &nbsp;
                        <form method="post" style="display:inline">
                            <input type="hidden" name="act" value="del">
                            <input type="hidden" name="id" value="<?= (int)$i ?>">
                            <button type="submit" class="btn btn-link text-danger" style="padding:0;border:0;"
                                    onclick="return confirm('<?= gettext('Удалить туннель безвозвратно?') ?>');">
                                <i class="fa-solid fa-trash-can"></i>
                            </button>
                        </form>
                    </td>
                </tr>
                <?php endforeach; ?>
            </tbody>
        </table>

        <nav class="action-buttons">
            <a href="vpn_awg_edit.php" class="btn btn-success">
                <i class="fa-solid fa-plus icon-embed-btn"></i><?= gettext('Добавить туннель') ?>
            </a>
            <form method="post" style="display:inline">
                <input type="hidden" name="act" value="apply">
                <button type="submit" class="btn btn-primary">
                    <i class="fa-solid fa-check icon-embed-btn"></i><?= gettext('Применить изменения') ?>
                </button>
            </form>
            <a href="vpn_awg_status.php" class="btn btn-info">
                <i class="fa-solid fa-signal icon-embed-btn"></i><?= gettext('Статус подключений') ?>
            </a>
        </nav>

    </div>
</div>

<div class="panel panel-default">
    <div class="panel-heading">
        <h2 class="panel-title"><?= gettext('Отладка') ?></h2>
    </div>
    <div class="panel-body">
        <form method="post" style="display:inline">
            <input type="hidden" name="act" value="<?= awg_debug_enabled() ? 'debug_off' : 'debug_on' ?>">
            <button type="submit" class="btn btn-sm <?= awg_debug_enabled() ? 'btn-warning' : 'btn-default' ?>">
                <?php if (awg_debug_enabled()): ?>
                    <i class="fa-solid fa-toggle-on icon-embed-btn"></i><?= gettext('Отладочное логирование ВКЛЮЧЕНО - нажмите, чтобы выключить') ?>
                <?php else: ?>
                    <i class="fa-solid fa-toggle-off icon-embed-btn"></i><?= gettext('Отладочное логирование выключено - нажмите, чтобы включить') ?>
                <?php endif; ?>
            </button>
        </form>
        <p class="text-muted small">
            <?= gettext('При включении подробные сообщения пакета пишутся в системный лог ' .
                        '(Status -> System Logs) с префиксом "AmneziaWG DEBUG". Полезно для ' .
                        'диагностики проблем, но не рекомендуется держать включённым постоянно ' .
                        'на боевой системе - засоряет лог.') ?>
        </p>
    </div>
</div>

<?php include('foot.inc'); ?>