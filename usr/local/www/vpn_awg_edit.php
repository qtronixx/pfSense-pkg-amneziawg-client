<?php
/*
 * vpn_awg_edit.php
 * -----------------------------------------------------------------------
 * Форма добавления/редактирования одного клиентского туннеля AmneziaWG.
 *
 * Состоит из двух логических частей:
 *   1) [Interface] - локальные параметры + ПОЛНЫЙ набор обфускации
 *      AmneziaWG (Jc/Jmin/Jmax/S1/S2/H1-H4) - требование п.3 ТЗ.
 *   2) [Peer] x N - динамический список ВНЕШНИХ серверов. Поле
 *      Endpoint обязательно для каждого peer - это и есть техническая
 *      реализация клиентского ограничения (п.1 ТЗ): без Endpoint
 *      добавить peer через эту форму невозможно, а значит невозможно
 *      создать "серверную" конфигурацию, ожидающую входящих клиентов.
 * -----------------------------------------------------------------------
 */

declare(strict_types=1);

require_once('guiconfig.inc');
require_once('/usr/local/pkg/awg.inc');
require_once('/usr/local/pkg/awg_validate.inc');

global $config;

$pgtitle = [gettext('VPN'), gettext('AmneziaWG'), gettext('Редактировать')];

$tunnels = awg_get_tunnels();
$id = isset($_REQUEST['id']) ? (int)$_REQUEST['id'] : null;

/* -----------------------------------------------------------------
 * Значения по умолчанию для новой записи (в т.ч. дефолтные значения
 * обфускации, рекомендуемые upstream-проектом AmneziaWG).
 * ----------------------------------------------------------------- */
$pconfig = [
    'name'        => awg_next_free_name(),
    'enabled'     => '1',
    'privkey'     => '',
    'pubkey'      => '',
    'address'     => '',
    'address6'    => '',
    'listenport'  => '',
    'mtu'         => '1420',
    'jc'          => '4',
    'jmin'        => '40',
    'jmax'        => '70',
    's1'          => '0',
    's2'          => '0',
    'h1'          => (string) random_int(5, 2000000000),
    'h2'          => (string) random_int(5, 2000000000),
    'h3'          => (string) random_int(5, 2000000000),
    'h4'          => (string) random_int(5, 2000000000),
    'peer'        => [
        ['pubkey' => '', 'presharedkey' => '', 'endpoint' => '', 'endpointport' => '51820',
         'allowedips' => '0.0.0.0/0, ::/0', 'keepalive' => '25'],
    ],
];

if ($id !== null && isset($tunnels[$id])) {
    $pconfig = array_merge($pconfig, $tunnels[$id]);
    // Нормализация одиночного peer в список (см. awg.inc)
    if (isset($pconfig['peer']['pubkey'])) {
        $pconfig['peer'] = [$pconfig['peer']];
    }
}

/*
 * Подбирает следующее свободное имя интерфейса awgN.
 */
function awg_next_free_name(): string
{
    $used = array_column(awg_get_tunnels(), 'name');
    for ($i = 0; $i < 100; $i++) {
        if (!in_array(AWG_IF_PREFIX . $i, $used, true)) {
            return AWG_IF_PREFIX . $i;
        }
    }
    return AWG_IF_PREFIX . '0';
}

$input_errors = [];

/* -----------------------------------------------------------------
 * Обработка POST
 * ----------------------------------------------------------------- */
if ($_POST['save'] ?? false) {
    $pconfig = $_POST;

    // Сборка массива peer'ов из плоских POST-полей peer_pubkey[], peer_endpoint[] и т.д.
    $peer_count = count($_POST['peer_pubkey'] ?? []);
    $peers = [];
    for ($i = 0; $i < $peer_count; $i++) {
        // Пропускаем полностью пустые "дополнительные" строки формы
        if (trim((string)($_POST['peer_pubkey'][$i] ?? '')) === '') {
            continue;
        }
        $peers[] = [
            'pubkey'        => trim((string)$_POST['peer_pubkey'][$i]),
            'presharedkey'  => trim((string)($_POST['peer_psk'][$i] ?? '')),
            'endpoint'      => trim((string)($_POST['peer_endpoint'][$i] ?? '')),
            'endpointport'  => trim((string)($_POST['peer_endpointport'][$i] ?? '')),
            'allowedips'    => trim((string)($_POST['peer_allowedips'][$i] ?? '')),
            'keepalive'     => trim((string)($_POST['peer_keepalive'][$i] ?? '')),
        ];
    }
    $pconfig['peer'] = $peers;

    $input_errors = awg_validate_tunnel_form($pconfig);

    if (empty($input_errors)) {
        $entry = [
            'name'       => $pconfig['name'],
            'enabled'    => !empty($pconfig['enabled']) ? '1' : '',
            'descr'      => $pconfig['descr'] ?? '',
            'privkey'    => $pconfig['privkey'],
            'pubkey'     => $pconfig['pubkey'] ?? '',
            'address'    => $pconfig['address'] ?? '',
            'address6'   => $pconfig['address6'] ?? '',
            'listenport' => $pconfig['listenport'] ?? '',
            'mtu'        => $pconfig['mtu'] ?? '1420',
            'jc'         => $pconfig['jc'],
            'jmin'       => $pconfig['jmin'],
            'jmax'       => $pconfig['jmax'],
            's1'         => $pconfig['s1'],
            's2'         => $pconfig['s2'],
            'h1'         => $pconfig['h1'],
            'h2'         => $pconfig['h2'],
            'h3'         => $pconfig['h3'],
            'h4'         => $pconfig['h4'],
            'peer'       => $peers,
        ];

        if ($id !== null && isset($tunnels[$id])) {
            $tunnels[$id] = $entry;
        } else {
            $tunnels[] = $entry;
        }

        awg_save_tunnels($tunnels);
        awg_write_conf($entry);

        header('Location: /vpn_awg_tunnels.php');
        exit;
    }
}

/* Обработка кнопки "Сгенерировать ключи" через AJAX-подобный POST */
if (($_POST['genkey'] ?? false)) {
    $kp = awg_genkey();
    $pconfig['privkey'] = $kp['privkey'] ?? '';
    $pconfig['pubkey']  = $kp['pubkey'] ?? '';
}

include('head.inc');
?>
<body>
<?php include('fbegin.inc'); ?>

<?php if (!empty($input_errors)): ?>
    <?php print_input_errors($input_errors); ?>
<?php endif; ?>

<form method="post" name="iform" id="iform">
<input type="hidden" name="id" value="<?= htmlspecialchars((string)($id ?? '')) ?>">

<div class="panel panel-default">
    <div class="panel-heading"><h2 class="panel-title"><?= gettext('Интерфейс') ?></h2></div>
    <div class="panel-body">

        <div class="form-group">
            <label class="col-sm-2 control-label"><?= gettext('Включен') ?></label>
            <div class="col-sm-10">
                <input type="checkbox" name="enabled" value="1" <?= !empty($pconfig['enabled']) ? 'checked' : '' ?>>
            </div>
        </div>

        <div class="form-group">
            <label class="col-sm-2 control-label"><?= gettext('Имя интерфейса') ?></label>
            <div class="col-sm-4">
                <input type="text" class="form-control" name="name" required pattern="awg[0-9]{1,3}"
                       value="<?= htmlspecialchars($pconfig['name']) ?>" <?= $id !== null ? 'readonly' : '' ?>>
                <span class="help-block"><?= gettext('Формат: awg0, awg1, ...') ?></span>
            </div>
        </div>

        <div class="form-group">
            <label class="col-sm-2 control-label"><?= gettext('Описание') ?></label>
            <div class="col-sm-6">
                <input type="text" class="form-control" name="descr"
                       value="<?= htmlspecialchars($pconfig['descr'] ?? '') ?>">
            </div>
        </div>

        <div class="form-group">
            <label class="col-sm-2 control-label"><?= gettext('Приватный ключ') ?></label>
            <div class="col-sm-6">
                <input type="text" class="form-control" name="privkey"
                       value="<?= htmlspecialchars($pconfig['privkey']) ?>">
            </div>
            <div class="col-sm-2">
                <button type="submit" name="genkey" value="1" class="btn btn-default">
                    <?= gettext('Сгенерировать пару ключей') ?>
                </button>
            </div>
        </div>

        <div class="form-group">
            <label class="col-sm-2 control-label"><?= gettext('Публичный ключ') ?></label>
            <div class="col-sm-6">
                <input type="text" class="form-control" name="pubkey" readonly
                       value="<?= htmlspecialchars($pconfig['pubkey']) ?>">
                <span class="help-block"><?= gettext('Передайте этот ключ администратору AmneziaWG-сервера, если требуется allow-list на его стороне.') ?></span>
            </div>
        </div>

        <div class="form-group">
            <label class="col-sm-2 control-label"><?= gettext('Адрес туннеля (IPv4/CIDR)') ?></label>
            <div class="col-sm-4">
                <input type="text" class="form-control" name="address" placeholder="10.0.14.88/24"
                       value="<?= htmlspecialchars($pconfig['address'] ?? '') ?>">
            </div>
        </div>

        <div class="form-group">
            <label class="col-sm-2 control-label"><?= gettext('Адрес туннеля (IPv6/CIDR)') ?></label>
            <div class="col-sm-4">
                <input type="text" class="form-control" name="address6"
                       value="<?= htmlspecialchars($pconfig['address6'] ?? '') ?>">
            </div>
        </div>

        <div class="form-group">
            <label class="col-sm-2 control-label"><?= gettext('ListenPort (опционально)') ?></label>
            <div class="col-sm-3">
                <input type="number" class="form-control" name="listenport" min="1" max="65535"
                       value="<?= htmlspecialchars($pconfig['listenport'] ?? '') ?>">
            </div>
        </div>

        <div class="form-group">
            <label class="col-sm-2 control-label"><?= gettext('MTU') ?></label>
            <div class="col-sm-3">
                <input type="number" class="form-control" name="mtu" min="576" max="9000"
                       value="<?= htmlspecialchars($pconfig['mtu']) ?>">
            </div>
        </div>

    </div>
</div>

<div class="panel panel-default">
    <div class="panel-heading"><h2 class="panel-title"><?= gettext('Параметры обфускации AmneziaWG') ?></h2></div>
    <div class="panel-body">
        <p class="text-muted">
            <?= gettext('Эти параметры должны ТОЧНО совпадать со значениями на стороне сервера AmneziaWG. Их можно получить из файла экспорта клиента приложения Amnezia либо запросить у администратора сервера.') ?>
        </p>

        <div class="row">
            <?php
            $fields = [
                'jc'   => ['Jc',   gettext('Количество junk-пакетов перед handshake (1-128)')],
                'jmin' => ['Jmin', gettext('Мин. размер junk-пакета, байт')],
                'jmax' => ['Jmax', gettext('Макс. размер junk-пакета, байт')],
                's1'   => ['S1',   gettext('Доп. байты перед init-пакетом')],
                's2'   => ['S2',   gettext('Доп. байты перед response-пакетом')],
                'h1'   => ['H1',   gettext('Magic header 1 (уникальное число)')],
                'h2'   => ['H2',   gettext('Magic header 2 (уникальное число)')],
                'h3'   => ['H3',   gettext('Magic header 3 (уникальное число)')],
                'h4'   => ['H4',   gettext('Magic header 4 (уникальное число)')],
            ];
            foreach ($fields as $key => [$label, $hint]): ?>
                <div class="col-sm-4 form-group">
                    <label><?= $label ?></label>
                    <input type="number" class="form-control" name="<?= $key ?>"
                           value="<?= htmlspecialchars((string)$pconfig[$key]) ?>">
                    <span class="help-block small"><?= $hint ?></span>
                </div>
            <?php endforeach; ?>
        </div>
    </div>
</div>

<div class="panel panel-default">
    <div class="panel-heading">
        <h2 class="panel-title"><?= gettext('Пиры (внешние серверы AmneziaWG)') ?></h2>
    </div>
    <div class="panel-body">
        <p class="text-muted">
            <?= gettext('Каждая запись обязана указывать Endpoint - адрес внешнего сервера. Пакет работает только в клиентском режиме, поэтому добавить peer без Endpoint (ожидание входящих подключений) через этот интерфейс нельзя.') ?>
        </p>

        <table class="table" id="peer-table">
            <thead>
                <tr>
                    <th><?= gettext('Публичный ключ сервера') ?></th>
                    <th><?= gettext('Preshared key (опц.)') ?></th>
                    <th><?= gettext('Endpoint (хост/IP)') ?></th>
                    <th><?= gettext('Порт') ?></th>
                    <th><?= gettext('AllowedIPs') ?></th>
                    <th><?= gettext('Keepalive, сек') ?></th>
                    <th></th>
                </tr>
            </thead>
            <tbody>
                <?php foreach ($pconfig['peer'] as $p): ?>
                <tr>
                    <td><input type="text" class="form-control" name="peer_pubkey[]" value="<?= htmlspecialchars($p['pubkey'] ?? '') ?>"></td>
                    <td><input type="text" class="form-control" name="peer_psk[]" value="<?= htmlspecialchars($p['presharedkey'] ?? '') ?>"></td>
                    <td><input type="text" class="form-control" name="peer_endpoint[]" required value="<?= htmlspecialchars($p['endpoint'] ?? '') ?>"></td>
                    <td><input type="number" class="form-control" name="peer_endpointport[]" value="<?= htmlspecialchars($p['endpointport'] ?? '51820') ?>"></td>
                    <td><input type="text" class="form-control" name="peer_allowedips[]" value="<?= htmlspecialchars($p['allowedips'] ?? '0.0.0.0/0, ::/0') ?>"></td>
                    <td><input type="number" class="form-control" name="peer_keepalive[]" value="<?= htmlspecialchars($p['keepalive'] ?? '25') ?>"></td>
                    <td><button type="button" class="btn btn-danger btn-xs remove-peer"><i class="fa-solid fa-trash-can"></i></button></td>
                </tr>
                <?php endforeach; ?>
            </tbody>
        </table>
        <button type="button" id="add-peer" class="btn btn-default btn-sm">
            <i class="fa-solid fa-plus icon-embed-btn"></i><?= gettext('Добавить peer (резервный сервер)') ?>
        </button>
    </div>
</div>

<nav class="action-buttons">
    <button type="submit" name="save" value="1" class="btn btn-primary">
        <i class="fa-solid fa-save icon-embed-btn"></i><?= gettext('Сохранить') ?>
    </button>
    <a href="vpn_awg_tunnels.php" class="btn btn-default"><?= gettext('Отмена') ?></a>
</nav>

</form>

<script>
/* Простое клиентское добавление/удаление строк peer-таблицы без React/Vue -
   в стиле остального pfSense GUI (чистый jQuery, уже подключён в head.inc). */
document.getElementById('add-peer').addEventListener('click', function () {
    var tbody = document.querySelector('#peer-table tbody');
    var row = tbody.rows[0].cloneNode(true);
    Array.from(row.querySelectorAll('input')).forEach(function (el) { el.value = ''; });
    row.querySelector('input[name="peer_endpointport[]"]').value = '51820';
    row.querySelector('input[name="peer_allowedips[]"]').value = '0.0.0.0/0, ::/0';
    row.querySelector('input[name="peer_keepalive[]"]').value = '25';
    tbody.appendChild(row);
});
document.querySelector('#peer-table').addEventListener('click', function (e) {
    if (e.target.closest('.remove-peer')) {
        var tbody = document.querySelector('#peer-table tbody');
        if (tbody.rows.length > 1) {
            e.target.closest('tr').remove();
        }
    }
});
</script>

<?php include('foot.inc'); ?>
