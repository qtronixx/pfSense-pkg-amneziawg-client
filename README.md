# pfSense-pkg-amneziawg-client

pfSense-pkg-amneziawg-client/
├── Makefile                                  # сборка FreeBSD pkg
├── pkg-plist                                 # список файлов пакета
├── pkg-descr                                 # описание для pkg manager
├── files/
│   ├── usr/local/pkg/
│   │   ├── awg.inc                           # основная backend-библиотека (PHP)
│   │   ├── awg.xml                           # XML-описание пакета для Package Manager
│   │   └── awg_validate.inc                  # валидация форм
│   ├── usr/local/www/
│   │   ├── vpn_awg_tunnels.php               # список туннелей (клиентов)
│   │   ├── vpn_awg_edit.php                  # форма редактирования клиента
│   │   ├── vpn_awg_status.php                # статус (awg show)
│   │   └── widgets/widgets/awg_status.widget.php
│   ├── usr/local/etc/rc.d/
│   │   └── awg.sh                            # rc.d скрипт управления службой
│   ├── etc/inc/priv/
│   │   └── awg.priv.inc                      # привилегии GUI
│   └── usr/local/share/pfSense-pkg-amneziawg-client/
│       └── info.xml
└── scripts/
    └── build_kmod.sh                         # инструкция/скрипт сборки if_awg.ko из исходников


Пояснение по совместимости с 2.8.1

Все обращения к конфигу идут через обёртки awg_config_get_path() / awg_config_set_path() — pfSense 2.8.x постепенно переводит внутренний API на config_get_path()/config_set_path() вместо прямых обращений к $config[...]; обёртка даёт совместимость в обе стороны.
declare(strict_types=1) и явные приведения типов ((int), (string)) — под PHP 8.2, где неявные приведения null→string в некоторых контекстах генерируют Deprecated-предупреждения.
Служба зарегистрирована как нормальный rc.d-сервис с REQUIRE: NETWORKING, а не через костыль Shellcmd/earlyshellcmd, как в оригинальной инструкции — это убирает лишнюю пакетную зависимость и решает ту же проблему порядка загрузки штатно.
