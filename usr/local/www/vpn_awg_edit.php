<?php
/*
 * vpn_awg_edit.php
 * -----------------------------------------------------------------------
 * ИСПРАВЛЕНИЯ:
 *   - form-horizontal для корректной вёрстки Bootstrap.
 *   - id формы "awgform" вместо "iform".
 *   - Логирование через awg_debug() на каждом шаге обработки POST.
 *   - Импорт готового .conf (текст/файл) через awg_parse_conf().
 *   - УБРАН дублирующийся блок обработки POST, который оставался в
 *     файле со времён предыдущей правки - вызывал повторную обработку
 *     $_POST и затирал результат разбора конфига пустыми значениями.
 *   - I1-I5 хранятся в $pconfig как ЧИТАЕМЫЙ текст везде (загрузка
 *     существующего туннеля, разбор .conf, рендер формы); кодируются
 *     в base64 только один раз - непосредственно при формировании
 *     $entry перед awg_save_tunnels().
 * -----------------------------------------------------------------------
 */

declare(strict_types=1);

require_once('guiconfig.inc');
require_once('/usr/local/pkg/awg.inc');
require_once('/usr/local/pkg/awg_validate.inc');

global $config;

$pgtitle = [gettext('VPN'), gettext('AmneziaWG'), gettext('Редактировать')];

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

$tunnels = awg_get_tunnels();
$id = isset($_REQUEST['id']) ? (int)$_REQUEST['id'] : null;

$pconfig = [
    'name'        => awg_next_free_name(),
    'enabled'     => '1',
    'descr'       => '',
    'privkey'     => '',
    'pubkey'      => '',
    'address'     => '',
    'address6'    => '',
    'listenport'  => '',
    'mtu'         => '1420',
    'jc'          => '4',
    'jmin'        => '10',
    'jmax'        => '50',
    's1'          => '0',
    's2'          => '0',
    's3'          => '0',
    's4'          => '0',
    'h1'          => '1',
    'h2'          => '2',
    'h3'          => '3',
    'h4'          => '4',
    'i1'          => '',
    'i2'          => '',
    'i3'          => '',
    'i4'          => '',
    'i5'          => '',
    'dns'         => '',
    'peer'        => [
        ['pubkey' => '', 'presharedkey' => '', 'endpoint' => '', 'endpointport' => '51820',
         'allowedips' => '0.0.0.0/0, ::/0', 'keepalive' => '25'],
    ],
];

if ($id !== null && isset($tunnels[$id])) {
    $pconfig = array_merge($pconfig, $tunnels[$id]);
    foreach (['i1', 'i2', 'i3', 'i4', 'i5'] as $k) {
        $pconfig[$k] = awg_decode_cps((string)($pconfig[$k] ?? ''));
    }
    if (isset($pconfig['peer']['pubkey'])) {
        $pconfig['peer'] = [$pconfig['peer']];
    }
}

$input_errors  = [];
$parse_message = '';

awg_debug('vpn_awg_edit.php: REQUEST_METHOD=' . ($_SERVER['REQUEST_METHOD'] ?? '?'));

if ($_SERVER['REQUEST_METHOD'] === 'POST') {
    awg_debug('POST получен, ключи: ' . implode(', ', array_keys($_POST)));

    $pconfig = $_POST;

    if ($_POST['parse_conf'] ?? false) {
        /*
         * Импорт .conf изолирован в свою ветку - не должен доходить до
         * кода ниже, который строит $pconfig['peer'] из плоских полей
         * peer_pubkey[]/peer_endpoint[] и т.д. Иначе пустые значения из
         * ещё не заполненной формы перезаписали бы то, что мы только
         * что разобрали из текста конфига.
         */
        awg_debug('Нажата кнопка "Разобрать конфиг"');
        $conf_text = (string)($pconfig['conf_text'] ?? '');
        if (trim($conf_text) === '') {
            $parse_message = 'Вставьте текст конфигурации или выберите файл перед разбором.';
        } else {
            $parsed = awg_parse_conf($conf_text);
            $pconfig = array_merge($pconfig, $parsed);
            awg_debug('Конфиг разобран, найдено peer: ' . count($parsed['peer'] ?? []));
            $parse_message = 'Конфигурация разобрана и подставлена в поля формы ниже. Проверьте значения и нажмите «Сохранить».';
        }
    } else {
        $peer_count = count($_POST['peer_pubkey'] ?? []);
        awg_debug('peer_count из POST: ' . $peer_count);

        $peers = [];
        for ($i = 0; $i < $peer_count; $i++) {
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
        awg_debug('Собрано peers: ' . count($peers));

        if ($_POST['genkey'] ?? false) {
            awg_debug('Нажата кнопка genkey');
            $kp = awg_genkey();
            $pconfig['privkey'] = $kp['privkey'] ?? '';
            $pconfig['pubkey']  = $kp['pubkey'] ?? '';
            if (!empty($kp['error'])) {
                awg_debug('awg_genkey() ошибка: ' . $kp['error']);
            }
        }

        if ($_POST['save'] ?? false) {
            awg_debug('Нажата кнопка save, запускаю валидацию');
            $input_errors = awg_validate_tunnel_form($pconfig);
            awg_debug('Результат валидации, ошибок: ' . count($input_errors) . ' | ' . implode(' || ', $input_errors));

            if (empty($input_errors)) {
                // Защита от обхода readonly на имени интерфейса при редактировании -
                // берём имя из уже сохранённого туннеля, а не из POST.
                $final_name = ($id !== null && isset($tunnels[$id]))
                    ? $tunnels[$id]['name']
                    : $pconfig['name'];

                $entry = [
                    'name'       => $final_name,
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
                    's3'         => $pconfig['s3'] ?? '0',
                    's4'         => $pconfig['s4'] ?? '0',
                    'h1'         => $pconfig['h1'],
                    'h2'         => $pconfig['h2'],
                    'h3'         => $pconfig['h3'],
                    'h4'         => $pconfig['h4'],
                    // ЕДИНСТВЕННОЕ место кодирования I1-I5 в base64 -
                    // $pconfig['i1'..'i5'] здесь ВСЕГДА читаемый текст,
                    // независимо от того, откуда он взялся (существующий
                    // туннель после decode, разбор .conf, или ручной ввод).
                    'i1' => awg_encode_cps(trim((string)($pconfig['i1'] ?? ''))),
                    'i2' => awg_encode_cps(trim((string)($pconfig['i2'] ?? ''))),
                    'i3' => awg_encode_cps(trim((string)($pconfig['i3'] ?? ''))),
                    'i4' => awg_encode_cps(trim((string)($pconfig['i4'] ?? ''))),
                    'i5' => awg_encode_cps(trim((string)($pconfig['i5'] ?? ''))),
                    'dns'        => trim((string)($pconfig['dns'] ?? '')),
                    'peer'       => $peers,
                ];

                if ($id !== null && isset($tunnels[$id])) {
                    $tunnels[$id] = $entry;
                    awg_debug('Обновляю существующий туннель id=' . $id);
                } else {
                    $tunnels[] = $entry;
                    awg_debug('Добавляю новый туннель: ' . $entry['name']);
                }

                $save_ok = awg_save_tunnels($tunnels);
                awg_debug('awg_save_tunnels() результат: ' . ($save_ok ? 'true' : 'FALSE'));

                $conf_ok = awg_write_conf($entry);
                awg_debug('awg_write_conf() результат: ' . ($conf_ok ? 'true' : 'FALSE'));

                awg_debug('Редирект на vpn_awg_tunnels.php');
                header('Location: /vpn_awg_tunnels.php');
                exit;
            } else {
                awg_debug('Валидация не прошла, форма НЕ сохранена, показываю ошибки пользователю');
            }
        }
    }
}

include('head.inc');
?>
<body>
<?php include('fbegin.inc'); ?>

<?php if (!empty($input_errors)): ?>
    <?php print_input_errors($input_errors); ?>
<?php endif; ?>

<form method="post" name="awgform" id="awgform" class="form-horizontal">
<input type="hidden" name="id" value="<?= htmlspecialchars((string)($id ?? '')) ?>">

<div class="panel panel-default">
    <div class="panel-heading"><h2 class="panel-title"><?= gettext('Импорт из готового .conf файла') ?></h2></div>
    <div class="panel-body">
        <?php if (!empty($parse_message)): ?>
            <div class="alert alert-info"><?= htmlspecialchars($parse_message) ?></div>
        <?php endif; ?>

        <div class="form-group">
            <label class="col-sm-2 control-label"><?= gettext('Загрузить файл') ?></label>
            <div class="col-sm-6">
                <input type="file" id="conf_file_input" accept=".conf,.txt">
                <span class="help-block small"><?= gettext('Или вставьте содержимое конфига прямо в поле ниже.') ?></span>
            </div>
        </div>

        <div class="form-group">
            <label class="col-sm-2 control-label"><?= gettext('Текст конфигурации') ?></label>
            <div class="col-sm-10">
                <textarea class="form-control" id="conf_text" name="conf_text" rows="8"
                          placeholder="[Interface]&#10;PrivateKey = ...&#10;...&#10;[Peer]&#10;PublicKey = ...&#10;Endpoint = ..."></textarea>
            </div>
        </div>

        <div class="form-group">
            <div class="col-sm-offset-2 col-sm-10">
                <button type="submit" name="parse_conf" value="1" class="btn btn-default" formnovalidate>
                    <i class="fa-solid fa-file-import icon-embed-btn"></i><?= gettext('Разобрать и заполнить форму') ?>
                </button>
            </div>
        </div>
    </div>
</div>

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
                <button type="submit" name="genkey" value="1" class="btn btn-default" formnovalidate>
                    <?= gettext('Сгенерировать пару ключей') ?>
                </button>
            </div>
        </div>

        <div class="form-group">
            <label class="col-sm-2 control-label"><?= gettext('Публичный ключ') ?></label>
            <div class="col-sm-6">
                <input type="text" class="form-control" name="pubkey" readonly
                       value="<?= htmlspecialchars($pconfig['pubkey']) ?>">
            </div>
        </div>

        <div class="form-group">
            <label class="col-sm-2 control-label"><?= gettext('Адрес туннеля (IPv4/CIDR)') ?></label>
            <div class="col-sm-4">
                <input type="text" class="form-control" name="address" placeholder="10.8.1.31/32"
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

        <div class="form-group">
            <label class="col-sm-2 control-label"><?= gettext('DNS-серверы (через запятую)') ?></label>
            <div class="col-sm-6">
                <input type="text" class="form-control" name="dns" placeholder="172.29.172.254, 1.0.0.1"
                       value="<?= htmlspecialchars((string)($pconfig['dns'] ?? '')) ?>">
                <span class="help-block small"><?= gettext('Автоматически не применяется - настройте DNS Resolver в System -> General Setup вручную.') ?></span>
            </div>
        </div>

    </div>
</div>

<div class="panel panel-default">
    <div class="panel-heading"><h2 class="panel-title"><?= gettext('Параметры обфускации AmneziaWG 2.0') ?></h2></div>
    <div class="panel-body">
        <div class="alert alert-info">
            <?= gettext('Все значения должны ТОЧНО совпадать со значениями на сервере. H1-H4 указываются в формате диапазона "min-max" либо одиночным числом.') ?>
        </div>

        <div class="form-group">
            <label class="col-sm-2 control-label">Jc / Jmin / Jmax</label>
            <div class="col-sm-2">
                <input type="number" class="form-control" name="jc" placeholder="Jc" value="<?= htmlspecialchars((string)($pconfig['jc'] ?? '')) ?>">
            </div>
            <div class="col-sm-2">
                <input type="number" class="form-control" name="jmin" placeholder="Jmin" value="<?= htmlspecialchars((string)($pconfig['jmin'] ?? '')) ?>">
            </div>
            <div class="col-sm-2">
                <input type="number" class="form-control" name="jmax" placeholder="Jmax" value="<?= htmlspecialchars((string)($pconfig['jmax'] ?? '')) ?>">
            </div>
        </div>

        <div class="form-group">
            <label class="col-sm-2 control-label">S1 / S2 / S3 / S4</label>
            <div class="col-sm-2">
                <input type="number" class="form-control" name="s1" value="<?= htmlspecialchars((string)($pconfig['s1'] ?? '0')) ?>">
            </div>
            <div class="col-sm-2">
                <input type="number" class="form-control" name="s2" value="<?= htmlspecialchars((string)($pconfig['s2'] ?? '0')) ?>">
            </div>
            <div class="col-sm-2">
                <input type="number" class="form-control" name="s3" value="<?= htmlspecialchars((string)($pconfig['s3'] ?? '0')) ?>">
            </div>
            <div class="col-sm-2">
                <input type="number" class="form-control" name="s4" value="<?= htmlspecialchars((string)($pconfig['s4'] ?? '0')) ?>">
            </div>
        </div>

        <div class="form-group">
            <label class="col-sm-2 control-label">H1-H4</label>
            <?php foreach (['h1','h2','h3','h4'] as $k): ?>
            <div class="col-sm-2">
                <input type="text" class="form-control" name="<?= $k ?>" placeholder="min-max"
                       value="<?= htmlspecialchars((string)($pconfig[$k] ?? '')) ?>">
            </div>
            <?php endforeach; ?>
        </div>

        <div class="alert alert-info">
            <?= gettext('I1-I5 - опциональные CPS-строки. Пустые поля НЕ записываются в конфиг.') ?>
        </div>
        <?php foreach (['i1','i2','i3','i4','i5'] as $k): ?>
        <div class="form-group">
            <label class="col-sm-2 control-label"><?= strtoupper($k) ?></label>
            <div class="col-sm-10">
                <input type="text" class="form-control" name="<?= $k ?>" value="<?= htmlspecialchars((string)($pconfig[$k] ?? '')) ?>">
            </div>
        </div>
        <?php endforeach; ?>
    </div>
</div>

<div class="panel panel-default">
    <div class="panel-heading">
        <h2 class="panel-title"><?= gettext('Пиры (внешние серверы AmneziaWG)') ?></h2>
    </div>
    <div class="panel-body">
        <p class="text-muted">
            <?= gettext('Каждая запись обязана указывать Endpoint - адрес внешнего сервера.') ?>
        </p>

        <div class="table-responsive">
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
        </div>
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

document.getElementById('conf_file_input').addEventListener('change', function (e) {
    var file = e.target.files[0];
    if (!file) return;
    var reader = new FileReader();
    reader.onload = function (evt) {
        document.getElementById('conf_text').value = evt.target.result;
    };
    reader.readAsText(file);
});
</script>

<?php include('foot.inc'); ?>