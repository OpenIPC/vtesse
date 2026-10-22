![OpenIPC logo][logo]

# VTesse

[English](README.md) | **Русский** | [简体中文](README.zh-CN.md)

[![Build](https://github.com/OpenIPC/vtesse/actions/workflows/build.yml/badge.svg)](https://github.com/OpenIPC/vtesse/actions/workflows/build.yml)
[![Release](https://img.shields.io/badge/download-latest-blue)](https://github.com/OpenIPC/vtesse/releases/latest)
[![License](https://img.shields.io/badge/license-MIT-blue)](LICENSE.md)
[![Platform](https://img.shields.io/badge/platform-Linux%20%7C%20Android-brightgreen)](docs/architecture.md)
[![Status](https://img.shields.io/badge/status-prototype-orange)](docs/security.md)

Микросистема доступа к ресурсам: клиент для существующих серверов **VTun** (`vtund -s`), который работает **без TUN/TAP-устройства и без системного VPN**. Туннель завершается внутри приложения собственным TCP/IP-стеком в userspace (lwIP); приложения обращаются к удалённым устройствам через локальный SOCKS5-прокси или проброс портов на `127.0.0.1`.

> [!WARNING]
> **Инструмент для экспериментов, а не средство защиты.** В туннеле нет шифрования и никакой защиты приватности: весь трафик, включая видео, пароли и веб-интерфейсы устройств за туннелем, идёт открытым текстом, а из перехваченного рукопожатия VTun можно восстановить пароль сессии. Любой, кто находится на пути трафика (публичный Wi-Fi, мобильные сети и сети провайдера, интернет), может читать и подменять данные или перехватить сессию. Используйте только для экспериментов и только в доверенных сетях либо внутри собственного защищённого канала (TLS/stunnel, VPN оператора); не открывайте через него доступ к камерам и другим устройствам в открытых сетях. Подробнее: [docs/security.md](docs/security.md).

## Возможности

- **Без VPN:** не нужны root, `/dev/net/tun` и Android `VpnService` (нет значка VPN)
- **VTun:** серверы 3.0.x, транспорт TCP, сессии `type ether` и `type tun`, ответ на challenge методом XOR, keepalive, автоматическое переподключение с экспоненциальной задержкой
- **Сеть:** адрес и маршруты по DHCP через туннель (опция 121, RFC 3442), IPv4, только исходящие подключения
- **UDP-ретранслятор:** RTP, который камера шлёт на адрес в туннеле, попадает в локальный плеер (`--udp-relay`)
- **Локальный прокси:** SOCKS5 `CONNECT` с необязательной аутентификацией по логину и паролю, статический проброс TCP-портов (например, RTSP по TCP для видеоплееров)
- **Платформы:** встраиваемый Linux (IP-камеры, роутеры; ARM и MIPS, в том числе big-endian, uClibc/musl), Android, в будущем — встраиваемый модуль связи
- **Сначала библиотека:** C99 + POSIX, один поток, без собственного главного цикла — легко встраивается

## Скачивание

Статические бинарники (без зависимостей) — на [странице релизов](https://github.com/OpenIPC/vtesse/releases): [`latest`](https://github.com/OpenIPC/vtesse/releases/latest) пересобирается при каждом push в `main` (постоянные имена файлов, используются ниже). Версия VTesse указана в заголовке релиза; история изменений — в [CHANGELOG.md](CHANGELOG.md).

| Платформа                                                                                     | `ARCH`   |
|-----------------------------------------------------------------------------------------------|----------|
| Камеры OpenIPC: HiSilicon, Goke, SigmaStar, другие 32-битные ARM; Raspberry Pi с 32-битной ОС | `arm32`  |
| Камеры OpenIPC: Ingenic (T31 и другие); роутеры MIPS little-endian (MediaTek MT7621, MT7628)  | `mipsel` |
| Роутеры MIPS big-endian (Qualcomm Atheros ath79 и другие)                                     | `mips`   |
| 64-битный ARM: Raspberry Pi с 64-битной ОС, Rockchip, другие платы ARM64                      | `arm64`  |
| ПК и серверы                                                                                  | `x86_64` |

На каждую платформу один бинарник, `vtesse-ARCH`, с обоими режимами: lwIP (SOCKS5 / проброс портов без root) и TAP (интерфейс ядра с `--tap`). Режиму TAP нужны root и драйвер `tun` (`/dev/net/tun`); хук-скрипты `vtesse-hook.sh` и `vtesse-udhcpc.script` встроены в бинарник. До версии 1.12 выкладывался ещё `vtesse-tap-ARCH` (только TAP); его ссылки теперь отдают 404, вместо него — `vtesse-ARCH --tap`.

В релизах также есть сервер VTun для ПК и серверов, `vtund-3.0.2-x86_64`: оригинальный VTun 3.0.2 с нашими патчами (отказ во входе пишется в лог с ID устройства и причиной), см. [server/README.md](server/README.md). Для камер и роутеров vtund при необходимости собирается в среде OpenIPC.

### Постоянные ссылки

Релиз один — [`latest`](https://github.com/OpenIPC/vtesse/releases/latest), с заголовком `VTesse <версия>`; он пересобирается при каждом push в `main`, релизы по версиям не хранятся. В именах файлов нет версии (только у сервера VTun — версия VTun), поэтому эти ссылки не меняются:

| Файл                   | Что это                                                        | Постоянная ссылка                                                                 |
|------------------------|----------------------------------------------------------------|-----------------------------------------------------------------------------------|
| `vtesse-arm32`         | клиент, 32-битный ARM                                          | <https://github.com/OpenIPC/vtesse/releases/latest/download/vtesse-arm32>         |
| `vtesse-mipsel`        | клиент, MIPS little-endian                                     | <https://github.com/OpenIPC/vtesse/releases/latest/download/vtesse-mipsel>        |
| `vtesse-mips`          | клиент, MIPS big-endian                                        | <https://github.com/OpenIPC/vtesse/releases/latest/download/vtesse-mips>          |
| `vtesse-arm64`         | клиент, 64-битный ARM                                          | <https://github.com/OpenIPC/vtesse/releases/latest/download/vtesse-arm64>         |
| `vtesse-x86_64`        | клиент, ПК и серверы                                           | <https://github.com/OpenIPC/vtesse/releases/latest/download/vtesse-x86_64>        |
| `vtesse-android.apk`   | приложение для Android                                         | <https://github.com/OpenIPC/vtesse/releases/latest/download/vtesse-android.apk>   |
| `vtund-3.0.2-x86_64`   | сервер VTun с нашими патчами, ПК (имя меняется с версией VTun) | <https://github.com/OpenIPC/vtesse/releases/latest/download/vtund-3.0.2-x86_64>   |
| `vtesse-hook.sh`       | хук-скрипт режима TAP (встроен в бинарник; для `--hook FILE`)  | <https://github.com/OpenIPC/vtesse/releases/latest/download/vtesse-hook.sh>       |
| `vtesse-udhcpc.script` | скрипт udhcpc для интерфейса TAP (встроен)                     | <https://github.com/OpenIPC/vtesse/releases/latest/download/vtesse-udhcpc.script> |
| `S99vtesse`            | init-скрипт, OpenIPC (BusyBox; его пишет `--install`)          | <https://github.com/OpenIPC/vtesse/releases/latest/download/S99vtesse>            |
| `vtesse.init`          | init-скрипт, OpenWrt (procd; его пишет `--install`)            | <https://github.com/OpenIPC/vtesse/releases/latest/download/vtesse.init>          |

```sh
curl -fsSLO https://github.com/OpenIPC/vtesse/releases/latest/download/vtesse-arm32 &&
  chmod 755 vtesse-arm32
```

### Установка службой: камера OpenIPC, роутер OpenWrt, Linux на ПК

От root: скачайте бинарник для своего `ARCH` (см. таблицу) и дайте ему установить себя. Он определяет систему, копирует себя в `/usr/bin/vtesse` (на ПК — в `/usr/local/bin`), пишет `/etc/vtesse.conf` (права 600) из ключей и init-скрипт (OpenIPC — `S99vtesse`, OpenWrt — procd `vtesse.init`, systemd — `vtesse.service`) и запускает службу сразу и при каждой загрузке. Хук-скрипты встроены. На OpenWrt установка переживает `sysupgrade`.

```sh
ARCH=arm32   # arm32 или mipsel на камерах, mipsel / mips / arm64 на роутерах, x86_64 на ПК
curl -fsSL -o /tmp/vtesse https://github.com/OpenIPC/vtesse/releases/latest/download/vtesse-$ARCH &&
  chmod 755 /tmp/vtesse &&
  /tmp/vtesse --install --host vtun.example.org --port 5555 --password secret
```

- Имя сессии — ID устройства: 12 символов `0-9A-F`, он же MAC-адрес. Без `--session 02AB34CD56EF` он берётся из MAC интерфейса с маршрутом по умолчанию (`id_from=`, см. [Device ID](docs/usage.md#device-id)); `--id-from eth0` задаёт интерфейс.
- `--password -` читает пароль со стандартного ввода, а не из командной строки. Без `--port` порт — 5000.
- Существующий `/etc/vtesse.conf` сохраняется: `vtesse --install` без ключей только обновляет бинарник и init-скрипт, заданные снова ключи заменяют свои строки. Файл — обычные строки `key=value` ([configuration file](docs/usage.md#configuration-file)).
- `vtesse --uninstall` останавливает службу и удаляет всё, включая `/etc/vtesse.conf`.
- `--scripts` дополнительно пишет хук-скрипты в `/usr/libexec/vtesse` для правки; `--no-start` устанавливает без запуска. Все ключи: [Installation](docs/usage.md#installation).
- На OpenWrt режиму TAP нужен драйвер `tun`: `opkg update && opkg install kmod-tun` (на OpenWrt 25+: `apk add kmod-tun`).

**Доступ для техподдержки** на камере OpenIPC с установленным vtesse: достаточно `fw_setenv vtesse vtun.example.org` и перезагрузки, как с `fw_setenv vtun` для VTun. Если задан только сервер, порт — 5000, сессия — MAC интерфейса по умолчанию, а пароль получается из него так же, как у VTun-клиента OpenIPC; `vtesse_port`, `vtesse_password` и другие переменные это переопределяют ([U-Boot environment](docs/usage.md#u-boot-environment-helpdesk-access)). `fw_setenv vtesse` выключает доступ. Эти переменные, включая `vtesse_password`, может прочитать любой с root или с дампом флешки, а пароль из MAC не секрет (MAC виден в локальной сети): режим — для доступа техподдержки и защищён не сильнее, чем VTun в OpenIPC ([security](docs/security.md)).

Управление службой: `/etc/init.d/S99vtesse start|stop|restart|status` на OpenIPC (настройки переопределяются в `/etc/default/vtesse`), `/etc/init.d/vtesse start|stop` на OpenWrt, `systemctl status vtesse` на ПК.

### Роутер Keenetic (исследование, [#32](https://github.com/OpenIPC/vtesse/issues/32))

Пока не поддерживается. KeeneticOS (NDMS) — закрытая прошивка: по SSH доступна только командная строка NDMS, Linux-shell нет. Свои программы запускаются только через компонент **OPKG**, который ставит Entware в `/opt` на том хранения (`opkg disk <том> <URL установщика>`), поэтому роутеру нужна USB-флешка или внутренний раздел `storage:`.

Проверено на Netcraze Explorer NC-1621 (ребренд Keenetic, NDMS 5.2 beta, MIPS, 128 МБ RAM, `small_flash`): компонент `opkg` есть и ставится (`components install opkg`, `components commit`, прошивка пересобирается, роутер перезагружается), но у модели нет USB и раздела `storage:`, а Entware на RAM-диск `temp:` роутер ставить отказывается (`unsuitable disk type`). Дальше: Keenetic с USB-портом (MT7621 или arm64) и пакет `.ipk` с `vtesse-mipsel`/`vtesse-arm64`, init-скриптом Entware и хуками NDMS для TAP-бэкенда.

### Linux на ПК, Raspberry Pi

```sh
ARCH=x86_64   # x86_64; arm64 / arm32 на Raspberry Pi с 64- / 32-битной ОС
mkdir -p ~/.local/bin
curl -fsSL -o ~/.local/bin/vtesse https://github.com/OpenIPC/vtesse/releases/latest/download/vtesse-$ARCH && chmod 755 ~/.local/bin/vtesse
# root не нужен: SOCKS5 на 127.0.0.1:1080 и проброс на RTSP камеры
~/.local/bin/vtesse --config ~/.config/vtesse/vtesse.conf --socks 1080 --forward 8554:10.78.0.10:554
```

В файле те же строки `key=value`, что в `/etc/vtesse.conf` (см. [Сборка из исходников](#сборка-из-исходников)). Режим TAP службой systemd — установкой, как выше.

### Android

Android 5.0 и новее, включая ТВ-приставки: [`vtesse-android.apk`](https://github.com/OpenIPC/vtesse/releases/latest/download/vtesse-android.apk). Без разрешения VPN и без значка VPN — сервис с обычным уведомлением держит туннель и даёт SOCKS5 и проброс портов на `127.0.0.1`; плеер, например OpenIPC Decoder или VLC, открывает `rtsp://127.0.0.1:1551/...` (камера 1 сервера 1). До трёх серверов и до 8 камер на каждом, на фиксированных портах. ID устройства генерируется при первом запуске (долгое нажатие копирует его), его нужно добавить как сессию на сервере. До трёх профилей серверов, переключение одним нажатием. Подробнее — [android/README.md](android/README.md).

![VTesse для Android: профили серверов и камеры (пример данных)](docs/img/android-settings.png)

## Сборка из исходников

```sh
make
mkdir -p ~/.config/vtesse
cat > ~/.config/vtesse/vtesse.conf <<EOF
host=vtun.example.org
port=5555
session=02AB34CD56EF
password=secret
EOF
chmod 600 ~/.config/vtesse/vtesse.conf

build/vtesse-cli --config ~/.config/vtesse/vtesse.conf --socks 1080 --forward 8554:10.78.0.10:554
curl --socks5 127.0.0.1:1080 http://10.78.0.10/
ffplay -rtsp_transport tcp rtsp://127.0.0.1:8554/stream
```

Имя сессии — это ID устройства (12 символов `0-9A-F`, он же MAC-адрес); учётные данные читаются только из файла конфигурации.

## Документация

Документация на английском языке.

| Документ                             | Содержание                                                                                                                          |
|--------------------------------------|-------------------------------------------------------------------------------------------------------------------------------------|
| [Usage](docs/usage.md)               | сборка, опции `vtesse-cli`, файл конфигурации, ID устройства, DHCP и маршруты, локальный прокси, сообщения об ошибках, утилита-зонд |
| [VTun protocol](docs/protocol.md)    | рукопожатие, ответ на challenge, флаги сессии, кадры, keepalive, особенности сервера, важные для клиента, UDP                       |
| [Architecture](docs/architecture.md) | слои, `tunnel_ops_t`, клиент VTun, стек lwIP, прокси и управление потоком, структура репозитория                                    |
| [Embedding](docs/embedding.md)       | использование библиотек в другом приложении, интеграция с циклом событий, пример                                                    |
| [Security](docs/security.md)         | слабые места аутентификации VTun, открытый трафик, доступ к локальному прокси                                                       |
| [VTun server](server/README.md)      | сборки vtund с патчами, бинарники в релизах, список патчей                                                                          |
| [Testing](docs/testing.md)           | юнит-тесты, санитайзеры, big-endian MIPS, тестовый стенд в Docker и приёмочные тесты                                                |

## План работ

Этапы и задачи ведутся в [GitHub Issues](https://github.com/OpenIPC/vtesse/issues):

| Этап                                                                | Статус     | Задачи                                                                                                                                                                                                                                                                                                                                                                      |
|---------------------------------------------------------------------|------------|-----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| 0. Тестовый стенд (VTun 3.0.2 в Docker, мост, DHCP с опцией 121)    | ✅ Готово  | [#1](https://github.com/OpenIPC/vtesse/issues/1) ✅                                                                                                                                                                                                                                                                                                                         |
| 1. Прототип CLI для Linux: TCP + `type ether` + DHCP поверх lwIP    | ✅ Готово  | [#2](https://github.com/OpenIPC/vtesse/issues/2) ✅ [#3](https://github.com/OpenIPC/vtesse/issues/3) ✅ [#4](https://github.com/OpenIPC/vtesse/issues/4) ✅ [#5](https://github.com/OpenIPC/vtesse/issues/5) ✅ [#6](https://github.com/OpenIPC/vtesse/issues/6) ✅ [#7](https://github.com/OpenIPC/vtesse/issues/7) ✅ [#8](https://github.com/OpenIPC/vtesse/issues/8) ✅ |
| 2. Локальный прокси: SOCKS5 и проброс портов                        | ✅ Готово  | [#10](https://github.com/OpenIPC/vtesse/issues/10) ✅ [#11](https://github.com/OpenIPC/vtesse/issues/11) ✅                                                                                                                                                                                                                                                                 |
| 3. Встраиваемый Linux: кросс-сборка ARM/MIPS, размер и память, CI   | ✅ Готово  | [#12](https://github.com/OpenIPC/vtesse/issues/12) ✅ [#20](https://github.com/OpenIPC/vtesse/issues/20) ✅                                                                                                                                                                                                                                                                 |
| 3. Режим TAP/TUN (интерфейс ядра вместо lwIP, Linux)                | ✅ Готово  | [#19](https://github.com/OpenIPC/vtesse/issues/19) ✅                                                                                                                                                                                                                                                                                                                       |
| 4. Приложение для Android (без VpnService)                          | ✅ Готово  | [#13](https://github.com/OpenIPC/vtesse/issues/13) ✅                                                                                                                                                                                                                                                                                                                       |
| 5. Опционально: транспорт UDP, обёртка TLS, аутентификация Blowfish | ⏸ Отложено | [#14](https://github.com/OpenIPC/vtesse/issues/14) [#15](https://github.com/OpenIPC/vtesse/issues/15) [#16](https://github.com/OpenIPC/vtesse/issues/16)                                                                                                                                                                                                                    |
| 6. Повторное использование верхних слоёв в camex                    | ⏸ Отложено | [#17](https://github.com/OpenIPC/vtesse/issues/17)                                                                                                                                                                                                                                                                                                                          |
| Общее: тесты, лицензия                                              | ✅ Готово  | [#9](https://github.com/OpenIPC/vtesse/issues/9) ✅ [#18](https://github.com/OpenIPC/vtesse/issues/18) ✅                                                                                                                                                                                                                                                                   |

✅ — задача закрыта. ⏸ — отложено.

## Лицензия

[MIT License](LICENSE.md). Сторонний код распространяется под своей лицензией: lwIP — BSD-3-Clause; патчи сервера VTun в [`server/patches`](server/patches) и бинарник `vtund-*` — GPL, как сам VTun.

## Поддержка

- Вопросы и сообщения об ошибках: [GitHub Issues](https://github.com/OpenIPC/vtesse/issues)
- Документация: [docs/](docs/)

[![Telegram](https://openipc.org/images/telegram_button.svg)](https://openipc.org/our-channels)

[logo]: https://raw.githubusercontent.com/OpenIPC/.github/refs/heads/main/assets/openipc-logo-black.svg
