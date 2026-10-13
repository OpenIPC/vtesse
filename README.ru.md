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

## Быстрый старт

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

| Этап | Задачи |
|---|---|
| 0. Тестовый стенд (VTun 3.0.2 в Docker, мост, DHCP с опцией 121) | [#1](https://github.com/compot-dev/vtesse/issues/1) |
| 1. Прототип CLI для Linux: TCP + `type ether` + DHCP поверх lwIP | [#2](https://github.com/compot-dev/vtesse/issues/2) [#3](https://github.com/compot-dev/vtesse/issues/3) [#4](https://github.com/compot-dev/vtesse/issues/4) [#5](https://github.com/compot-dev/vtesse/issues/5) [#6](https://github.com/compot-dev/vtesse/issues/6) [#7](https://github.com/compot-dev/vtesse/issues/7) [#8](https://github.com/compot-dev/vtesse/issues/8) |
| 2. Локальный прокси: SOCKS5 и проброс портов | [#10](https://github.com/compot-dev/vtesse/issues/10) [#11](https://github.com/compot-dev/vtesse/issues/11) |
| 3. Встраиваемый Linux: кросс-сборка ARM/MIPS, размер и память | [#12](https://github.com/compot-dev/vtesse/issues/12) |
| 3. Дополнительный режим TAP/TUN (интерфейс ядра вместо lwIP, Linux) | [#19](https://github.com/compot-dev/vtesse/issues/19) |
| 4. Приложение для Android (без VpnService) | [#13](https://github.com/compot-dev/vtesse/issues/13) |
| 5. Опционально: транспорт UDP, обёртка TLS, аутентификация Blowfish | [#14](https://github.com/compot-dev/vtesse/issues/14) [#15](https://github.com/compot-dev/vtesse/issues/15) [#16](https://github.com/compot-dev/vtesse/issues/16) |
| 6. Повторное использование верхних слоёв в camex | [#17](https://github.com/compot-dev/vtesse/issues/17) |
| Общее: тесты и CI, лицензия | [#9](https://github.com/compot-dev/vtesse/issues/9) [#18](https://github.com/compot-dev/vtesse/issues/18) |

## Лицензия

[The Prosperity Public License 3.0.0](LICENSE.md): бесплатно для некоммерческого использования, для коммерческого — пробный период 30 дней. Полные условия — в [LICENSE.md](LICENSE.md). Сторонний код распространяется под своей лицензией (lwIP: BSD-3-Clause).

## Поддержка

- Вопросы и сообщения об ошибках: [GitHub Issues](https://github.com/compot-dev/vtesse/issues)
- Документация: [docs/](docs/)

[![Telegram](https://openipc.org/images/telegram_button.svg)](https://openipc.org/our-channels)

[logo]: https://raw.githubusercontent.com/OpenIPC/.github/refs/heads/main/assets/openipc-logo-black.svg
