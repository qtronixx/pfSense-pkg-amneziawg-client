<?php
/*
 * /etc/inc/priv/awg.priv.inc
 * -----------------------------------------------------------------------
 * Описание привилегий доступа к страницам пакета для системы прав
 * pfSense (System -> User Manager -> Privileges). Без этого файла
 * страницы пакета не будут доступны для назначения обычным
 * (не admin) пользователям/группам.
 * -----------------------------------------------------------------------
 */

declare(strict_types=1);

$priv_list['page-vpn-awg-tunnels'] = [
    'name'   => 'WebCfg - VPN: AmneziaWG: Туннели',
    'descr'  => 'Просмотр и управление списком клиентских туннелей AmneziaWG.',
    'match'  => ['vpn_awg_tunnels.php*'],
];

$priv_list['page-vpn-awg-edit'] = [
    'name'   => 'WebCfg - VPN: AmneziaWG: Редактирование',
    'descr'  => 'Создание и редактирование клиентских туннелей AmneziaWG.',
    'match'  => ['vpn_awg_edit.php*'],
];

$priv_list['page-vpn-awg-status'] = [
    'name'   => 'WebCfg - VPN: AmneziaWG: Статус',
    'descr'  => 'Просмотр статуса подключения (awg show).',
    'match'  => ['vpn_awg_status.php*'],
];
