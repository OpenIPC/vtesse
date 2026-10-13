![OpenIPC logo][logo]

# VTesse

[English](README.md) | [Русский](README.ru.md) | **简体中文**

[![Build](https://github.com/compot-dev/vtesse/actions/workflows/build.yml/badge.svg)](https://github.com/compot-dev/vtesse/actions/workflows/build.yml)
[![Release](https://img.shields.io/badge/download-latest-blue)](https://github.com/compot-dev/vtesse/releases/tag/latest)
[![License](https://img.shields.io/badge/license-Prosperity%203.0-blue)](LICENSE.md)
[![Platform](https://img.shields.io/badge/platform-Linux%20%7C%20Android-brightgreen)](docs/architecture.md)
[![Status](https://img.shields.io/badge/status-prototype-orange)](docs/security.md)

一个资源访问微系统：面向现有 **VTun** 服务器（`vtund -s`）的客户端，**无需 TUN/TAP 设备，也无需系统 VPN**。隧道在应用内部由用户态 TCP/IP 协议栈（lwIP）终结；应用通过 `127.0.0.1` 上的本地 SOCKS5 代理或端口转发访问远端设备。

## 功能

- **无需 VPN：** 不需要 root、`/dev/net/tun` 或 Android `VpnService`（不显示 VPN 图标）
- **VTun：** 3.0.x 服务器，TCP 传输，`type ether` 与 `type tun` 会话，XOR 质询应答，keepalive，按指数退避自动重连
- **网络：** 通过隧道内的 DHCP 获取地址和路由（选项 121，RFC 3442），IPv4，仅出站连接
- **本地代理：** 支持可选用户名/密码认证的 SOCKS5 `CONNECT`，静态 TCP 端口转发（例如为视频播放器提供基于 TCP 的 RTSP）
- **目标平台：** 嵌入式 Linux（IP 摄像机、路由器；ARM 与 MIPS，含大端序，uClibc/musl）、Android，后续还将提供可嵌入的通信模块
- **库优先：** C99 + POSIX，单线程，不自带主循环——易于嵌入

## 快速开始

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

会话名即设备 ID（12 个 `0-9A-F` 字符，同时用作 MAC 地址）；凭据只从配置文件读取。

## 文档

文档为英文。

| 文档 | 内容 |
|---|---|
| [Usage](docs/usage.md) | 构建、`vtesse-cli` 选项、配置文件、设备 ID、DHCP 与路由、本地代理、错误信息、探测工具 |
| [VTun protocol](docs/protocol.md) | 握手、质询应答、会话标志、帧格式、keepalive、影响客户端的服务器行为、UDP |
| [Architecture](docs/architecture.md) | 分层、`tunnel_ops_t`、VTun 客户端、lwIP 协议栈、代理与流量控制、仓库结构 |
| [Embedding](docs/embedding.md) | 在其他应用中使用这些库、事件循环集成、示例 |
| [Security](docs/security.md) | VTun 认证的弱点、明文数据、本地代理的暴露面 |
| [Testing](docs/testing.md) | 单元测试、sanitizer、大端序 MIPS、Docker 测试台与验收测试 |

## 路线图

各阶段与任务记录在 [GitHub Issues](https://github.com/compot-dev/vtesse/issues) 中：

| 阶段 | Issues |
|---|---|
| 0. 测试台（Docker 中的 VTun 3.0.2、网桥、带选项 121 的 DHCP） | [#1](https://github.com/compot-dev/vtesse/issues/1) |
| 1. Linux 命令行原型：TCP + `type ether` + 基于 lwIP 的 DHCP | [#2](https://github.com/compot-dev/vtesse/issues/2) [#3](https://github.com/compot-dev/vtesse/issues/3) [#4](https://github.com/compot-dev/vtesse/issues/4) [#5](https://github.com/compot-dev/vtesse/issues/5) [#6](https://github.com/compot-dev/vtesse/issues/6) [#7](https://github.com/compot-dev/vtesse/issues/7) [#8](https://github.com/compot-dev/vtesse/issues/8) |
| 2. 本地代理：SOCKS5 与端口转发 | [#10](https://github.com/compot-dev/vtesse/issues/10) [#11](https://github.com/compot-dev/vtesse/issues/11) |
| 3. 嵌入式 Linux：ARM/MIPS 交叉编译、体积与内存 | [#12](https://github.com/compot-dev/vtesse/issues/12) |
| 3. 可选 TAP/TUN 后端（用内核接口代替 lwIP，Linux 平台） | [#19](https://github.com/compot-dev/vtesse/issues/19) |
| 4. Android 应用（不使用 VpnService） | [#13](https://github.com/compot-dev/vtesse/issues/13) |
| 5. 可选：UDP 传输、TLS 封装、Blowfish 认证 | [#14](https://github.com/compot-dev/vtesse/issues/14) [#15](https://github.com/compot-dev/vtesse/issues/15) [#16](https://github.com/compot-dev/vtesse/issues/16) |
| 6. 在 camex 中复用上层模块 | [#17](https://github.com/compot-dev/vtesse/issues/17) |
| 通用：测试与 CI、许可证 | [#9](https://github.com/compot-dev/vtesse/issues/9) [#18](https://github.com/compot-dev/vtesse/issues/18) |

## 许可证

[The Prosperity Public License 3.0.0](LICENSE.md)：非商业用途免费，商业用途可试用 30 天。完整条款见 [LICENSE.md](LICENSE.md)。第三方代码沿用其自身的许可证（lwIP：BSD-3-Clause）。

## 支持

- 问题与缺陷报告：[GitHub Issues](https://github.com/compot-dev/vtesse/issues)
- 文档：[docs/](docs/)

[![Telegram](https://openipc.org/images/telegram_button.svg)](https://openipc.org/our-channels)

[logo]: https://raw.githubusercontent.com/OpenIPC/.github/refs/heads/main/assets/openipc-logo-black.svg
