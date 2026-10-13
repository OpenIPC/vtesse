![OpenIPC logo][logo]

# VTesse

[English](README.md) | **Русский** | [简体中文](README.zh-CN.md)

[![Build](https://github.com/compot-dev/vtesse/actions/workflows/build.yml/badge.svg)](https://github.com/compot-dev/vtesse/actions/workflows/build.yml)
[![Release](https://img.shields.io/badge/download-latest-blue)](https://github.com/compot-dev/vtesse/releases/tag/latest)
[![License](https://img.shields.io/badge/license-Prosperity%203.0-blue)](LICENSE.md)
[![Platform](https://img.shields.io/badge/platform-Linux%20%7C%20Android-brightgreen)](docs/architecture.md)
[![Status](https://img.shields.io/badge/status-prototype-orange)](docs/security.md)

Микросистема доступа к ресурсам: клиент для существующих серверов **VTun** (`vtund -s`), который работает **без TUN/TAP-устройства и без системного VPN**. Туннель завершается внутри приложения собственным TCP/IP-стеком в userspace (lwIP); приложения обращаются к удалённым устройствам через локальный SOCKS5-прокси или проброс портов на `127.0.0.1`.

## Возможности

- **Без VPN:** не нужны root, `/dev/net/tun` и Android `VpnService` (нет значка VPN)
- **VTun:** серверы 3.0.x, транспорт TCP, сессии `type ether` и `type tun`, ответ на challenge методом XOR, keepalive, автоматическое переподключение с экспоненциальной задержкой
- **Сеть:** адрес и маршруты по DHCP через туннель (опция 121, RFC 3442), IPv4, только исходящие подключения
- **Локальный прокси:** SOCKS5 `CONNECT` с необязательной аутентификацией по логину и паролю, статический проброс TCP-портов (например, RTSP по TCP для видеоплееров)
- **Платформы:** встраиваемый Linux (IP-камеры, роутеры; ARM и MIPS, в том числе big-endian, uClibc/musl), Android, в будущем — встраиваемый модуль связи
- **Сначала библиотека:** C99 + POSIX, один поток, без собственного главного цикла — легко встраивается

## Скачивание

Статические бинарники (без зависимостей) — на [странице релизов](https://github.com/compot-dev/vtesse/releases): [`latest`](https://github.com/compot-dev/vtesse/releases/tag/latest) пересобирается при каждом push в `main` (постоянные имена файлов, используются ниже); у версионных релизов, например [`v1.00`](https://github.com/compot-dev/vtesse/releases/tag/v1.00), версия входит в имя файла (`vtesse-1.00-arm32`).

| Платформа | `ARCH` |
|---|---|
| Камеры OpenIPC: HiSilicon, Goke, SigmaStar, другие 32-битные ARM; Raspberry Pi с 32-битной ОС | `arm32` |
| Камеры OpenIPC: Ingenic (T31 и другие); роутеры MIPS little-endian (MediaTek MT7621, MT7628) | `mipsel` |
| Роутеры MIPS big-endian (Qualcomm Atheros ath79 и другие) | `mips` |
| 64-битный ARM: Raspberry Pi с 64-битной ОС, Rockchip, другие платы ARM64 | `arm64` |
| ПК и серверы | `x86_64` |

На каждую платформу два бинарника: `vtesse-ARCH` (режимы lwIP и TAP: SOCKS5 / проброс портов без root или интерфейс ядра с `--tap`) и `vtesse-tap-ARCH` (только TAP, примерно вдвое меньше). Режиму TAP нужны root, драйвер `tun` (`/dev/net/tun`) и хук-скрипты `vtesse-hook.sh` и `vtesse-udhcpc.script`.

### Конфигурация

Все варианты установки ниже читают `/etc/vtesse.conf` (имя сессии — ID устройства: 12 символов `0-9A-F`, он же MAC-адрес):

```sh
cat > /etc/vtesse.conf <<EOF
host=vtun.example.org
port=5555
session=02AB34CD56EF
password=secret
EOF
chmod 600 /etc/vtesse.conf
```

### Камера OpenIPC

```sh
# от root; ARCH = arm32 или mipsel, см. таблицу
ARCH=arm32
URL=https://github.com/compot-dev/vtesse/releases/download/latest
cd /tmp
curl -fsSLO $URL/vtesse-tap-$ARCH
curl -fsSLO $URL/vtesse-hook.sh
curl -fsSLO $URL/vtesse-udhcpc.script
curl -fsSL $URL/SHA256SUMS | grep -E " (vtesse-tap-$ARCH|vtesse-hook.sh|vtesse-udhcpc.script)$" | sha256sum -c -   # проверка загрузки
# установка
cp vtesse-tap-$ARCH /usr/bin/vtesse
mkdir -p /usr/share/vtesse
cp vtesse-hook.sh vtesse-udhcpc.script /usr/share/vtesse/
chmod 755 /usr/bin/vtesse /usr/share/vtesse/*
vtesse --version
# запуск в терминале (остановка — Ctrl+C)
vtesse --config /etc/vtesse.conf --tap --ifname vtesse0 --hook /usr/share/vtesse/vtesse-hook.sh
```

Автозапуск при загрузке:

```sh
cat > /etc/init.d/S99vtesse <<'EOF'
#!/bin/sh
case "$1" in
    start) start-stop-daemon -S -b -m -p /var/run/vtesse.pid -x /usr/bin/vtesse -- \
               --config /etc/vtesse.conf --tap --ifname vtesse0 --hook /usr/share/vtesse/vtesse-hook.sh ;;
    stop)  start-stop-daemon -K -p /var/run/vtesse.pid ;;
esac
EOF
chmod +x /etc/init.d/S99vtesse
/etc/init.d/S99vtesse start
```

### Роутер OpenWrt

```sh
opkg update && opkg install curl kmod-tun   # kmod-tun нужен только для --tap
# на OpenWrt 25+: apk add curl kmod-tun
# бинарник и скрипты устанавливаются как на камере выше, затем:
cat > /etc/init.d/vtesse <<'EOF'
#!/bin/sh /etc/rc.common
START=99
USE_PROCD=1
start_service() {
    procd_open_instance
    procd_set_param command /usr/bin/vtesse --config /etc/vtesse.conf \
        --tap --ifname vtesse0 --hook /usr/share/vtesse/vtesse-hook.sh
    procd_set_param respawn
    procd_close_instance
}
EOF
chmod +x /etc/init.d/vtesse
/etc/init.d/vtesse enable && /etc/init.d/vtesse start
```

### Linux на ПК, Raspberry Pi

```sh
ARCH=x86_64   # x86_64; arm64 / arm32 на Raspberry Pi с 64- / 32-битной ОС
curl -fsSLO https://github.com/compot-dev/vtesse/releases/download/latest/vtesse-$ARCH
chmod +x vtesse-$ARCH
# root не нужен: SOCKS5 на 127.0.0.1:1080 и проброс на RTSP камеры
./vtesse-$ARCH --config /etc/vtesse.conf --socks 1080 --forward 8554:10.78.0.10:554
```

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

| Документ | Содержание |
|---|---|
| [Usage](docs/usage.md) | сборка, опции `vtesse-cli`, файл конфигурации, ID устройства, DHCP и маршруты, локальный прокси, сообщения об ошибках, утилита-зонд |
| [VTun protocol](docs/protocol.md) | рукопожатие, ответ на challenge, флаги сессии, кадры, keepalive, особенности сервера, важные для клиента, UDP |
| [Architecture](docs/architecture.md) | слои, `tunnel_ops_t`, клиент VTun, стек lwIP, прокси и управление потоком, структура репозитория |
| [Embedding](docs/embedding.md) | использование библиотек в другом приложении, интеграция с циклом событий, пример |
| [Security](docs/security.md) | слабые места аутентификации VTun, открытый трафик, доступ к локальному прокси |
| [Testing](docs/testing.md) | юнит-тесты, санитайзеры, big-endian MIPS, тестовый стенд в Docker и приёмочные тесты |

## План работ

Этапы и задачи ведутся в [GitHub Issues](https://github.com/compot-dev/vtesse/issues):

| Этап | Статус | Задачи |
|---|---|---|
| 0. Тестовый стенд (VTun 3.0.2 в Docker, мост, DHCP с опцией 121) | ✅ Готово | [#1](https://github.com/compot-dev/vtesse/issues/1) ✅ |
| 1. Прототип CLI для Linux: TCP + `type ether` + DHCP поверх lwIP | ✅ Готово | [#2](https://github.com/compot-dev/vtesse/issues/2) ✅ [#3](https://github.com/compot-dev/vtesse/issues/3) ✅ [#4](https://github.com/compot-dev/vtesse/issues/4) ✅ [#5](https://github.com/compot-dev/vtesse/issues/5) [#6](https://github.com/compot-dev/vtesse/issues/6) [#7](https://github.com/compot-dev/vtesse/issues/7) ✅ [#8](https://github.com/compot-dev/vtesse/issues/8) |
| 2. Локальный прокси: SOCKS5 и проброс портов | ✅ Готово | [#10](https://github.com/compot-dev/vtesse/issues/10) ✅ [#11](https://github.com/compot-dev/vtesse/issues/11) ✅ |
| 3. Встраиваемый Linux: кросс-сборка ARM/MIPS, размер и память, CI | 🔄 В работе | [#12](https://github.com/compot-dev/vtesse/issues/12) [#20](https://github.com/compot-dev/vtesse/issues/20) ✅ |
| 3. Режим TAP/TUN (интерфейс ядра вместо lwIP, Linux) | 🔄 В работе | [#19](https://github.com/compot-dev/vtesse/issues/19) |
| 4. Приложение для Android (без VpnService) | ⏳ Запланировано | [#13](https://github.com/compot-dev/vtesse/issues/13) |
| 5. Опционально: транспорт UDP, обёртка TLS, аутентификация Blowfish | ⏳ Запланировано | [#14](https://github.com/compot-dev/vtesse/issues/14) [#15](https://github.com/compot-dev/vtesse/issues/15) [#16](https://github.com/compot-dev/vtesse/issues/16) |
| 6. Повторное использование верхних слоёв в camex | ⏳ Запланировано | [#17](https://github.com/compot-dev/vtesse/issues/17) |
| Общее: тесты, лицензия | 🔄 В работе | [#9](https://github.com/compot-dev/vtesse/issues/9) [#18](https://github.com/compot-dev/vtesse/issues/18) ✅ |

✅ — задача закрыта.

## Лицензия

[The Prosperity Public License 3.0.0](LICENSE.md): бесплатно для некоммерческого использования, для коммерческого — пробный период 30 дней. Полные условия — в [LICENSE.md](LICENSE.md). Сторонний код распространяется под своей лицензией (lwIP: BSD-3-Clause).

## Поддержка

- Вопросы и сообщения об ошибках: [GitHub Issues](https://github.com/compot-dev/vtesse/issues)
- Документация: [docs/](docs/)

[![Telegram](https://openipc.org/images/telegram_button.svg)](https://openipc.org/our-channels)

[logo]: https://raw.githubusercontent.com/OpenIPC/.github/refs/heads/main/assets/openipc-logo-black.svg
