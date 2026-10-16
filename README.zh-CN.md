![OpenIPC logo][logo]

# VTesse

[English](README.md) | [Русский](README.ru.md) | **简体中文**

[![Build](https://github.com/compot-dev/vtesse/actions/workflows/build.yml/badge.svg)](https://github.com/compot-dev/vtesse/actions/workflows/build.yml)
[![Release](https://img.shields.io/badge/download-latest-blue)](https://github.com/compot-dev/vtesse/releases/tag/latest)
[![License](https://img.shields.io/badge/license-Prosperity%203.0-blue)](LICENSE.md)
[![Platform](https://img.shields.io/badge/platform-Linux%20%7C%20Android-brightgreen)](docs/architecture.md)
[![Status](https://img.shields.io/badge/status-prototype-orange)](docs/security.md)

一个资源访问微系统：面向现有 **VTun** 服务器（`vtund -s`）的客户端，**无需 TUN/TAP 设备，也无需系统 VPN**。隧道在应用内部由用户态 TCP/IP 协议栈（lwIP）终结；应用通过 `127.0.0.1` 上的本地 SOCKS5 代理或端口转发访问远端设备。

> [!WARNING]
> **这是一个实验性工具，而不是安全产品。** 隧道没有任何加密和隐私保护：所有流量（包括视频、密码以及隧道后设备的 Web 界面）均以明文传输，截获的 VTun 握手还可能泄露会话密码。传输路径上的任何人（公共 Wi-Fi、移动网络和运营商网络、互联网）都可以读取、篡改数据或接管会话。请仅用于实验，并且只在可信网络中使用，或置于您自己的安全通道（TLS/stunnel、运营商 VPN）之内；不要在开放网络中通过它暴露摄像头或其他设备。详情见 [docs/security.md](docs/security.md)。

## 功能

- **无需 VPN：** 不需要 root、`/dev/net/tun` 或 Android `VpnService`（不显示 VPN 图标）
- **VTun：** 3.0.x 服务器，TCP 传输，`type ether` 与 `type tun` 会话，XOR 质询应答，keepalive，按指数退避自动重连
- **网络：** 通过隧道内的 DHCP 获取地址和路由（选项 121，RFC 3442），IPv4，仅出站连接
- **UDP 中继：** 摄像机推送到隧道地址的 RTP 可送达本地播放器（`--udp-relay`）
- **本地代理：** 支持可选用户名/密码认证的 SOCKS5 `CONNECT`，静态 TCP 端口转发（例如为视频播放器提供基于 TCP 的 RTSP）
- **目标平台：** 嵌入式 Linux（IP 摄像机、路由器；ARM 与 MIPS，含大端序，uClibc/musl）、Android，后续还将提供可嵌入的通信模块
- **库优先：** C99 + POSIX，单线程，不自带主循环——易于嵌入

## 下载

静态二进制文件（无依赖）发布在 [Releases 页面](https://github.com/compot-dev/vtesse/releases)：[`latest`](https://github.com/compot-dev/vtesse/releases/tag/latest) 随 `main` 的每次推送重新构建（文件名固定，下文使用）。发布标题中注明 VTesse 版本；变更记录见 [CHANGELOG.md](CHANGELOG.md)。

| 平台                                                                           | `ARCH`   |
|--------------------------------------------------------------------------------|----------|
| OpenIPC 摄像机：海思、国科、SigmaStar 及其他 32 位 ARM；运行 32 位系统的树莓派 | `arm32`  |
| OpenIPC 摄像机：君正（T31 等）；小端 MIPS 路由器（联发科 MT7621、MT7628）      | `mipsel` |
| 大端 MIPS 路由器（高通 Atheros ath79 等）                                      | `mips`   |
| 64 位 ARM：运行 64 位系统的树莓派、瑞芯微及其他 ARM64 开发板                   | `arm64`  |
| PC 与服务器                                                                    | `x86_64` |

每个平台提供两个二进制文件：`vtesse-ARCH`（lwIP 与 TAP 两种后端：无需 root 的 SOCKS5 / 端口转发，或使用 `--tap` 的内核接口）和 `vtesse-tap-ARCH`（仅 TAP，体积约为一半）。TAP 后端需要 root、`tun` 驱动（`/dev/net/tun`）以及钩子脚本 `vtesse-hook.sh` 和 `vtesse-udhcpc.script`。

发布页还提供面向 PC 和服务器的 VTun 服务器 `vtund-3.0.2-x86_64`：原版 VTun 3.0.2 加上我们的补丁（登录被拒时日志会记录设备 ID 和原因），详见 [server/README.md](server/README.md)。摄像头和路由器如需 vtund，请在 OpenIPC 环境中构建。

### 配置

下面所有安装方式都读取 `/etc/vtesse.conf`（会话名即设备 ID：12 个 `0-9A-F` 字符，同时用作 MAC 地址）：

```sh
cat > /etc/vtesse.conf <<EOF
host=vtun.example.org
port=5555
session=02AB34CD56EF
password=secret
EOF
chmod 600 /etc/vtesse.conf
```

在摄像头或路由器上，也可以直接使用设备自身的 MAC 作为 ID：用 `id_from=eth0` 代替 `session=…`（见 [Device ID](docs/usage.md#device-id)）。

### OpenIPC 摄像机

```sh
# 以 root 运行；ARCH = arm32 或 mipsel，见上表
ARCH=arm32
URL=https://github.com/compot-dev/vtesse/releases/download/latest
# 先建目录，再把每个文件直接下载到目标位置，下载成功后设置权限
mkdir -p /usr/bin /usr/share/vtesse /etc/init.d
curl -fsSL -o /usr/bin/vtesse $URL/vtesse-tap-$ARCH && chmod 755 /usr/bin/vtesse
curl -fsSL -o /usr/share/vtesse/vtesse-hook.sh $URL/vtesse-hook.sh && chmod 755 /usr/share/vtesse/vtesse-hook.sh
curl -fsSL -o /usr/share/vtesse/vtesse-udhcpc.script $URL/vtesse-udhcpc.script && chmod 755 /usr/share/vtesse/vtesse-udhcpc.script
curl -fsSL -o /etc/init.d/S99vtesse $URL/S99vtesse && chmod 755 /etc/init.d/S99vtesse
vtesse --version
# 前台运行（Ctrl+C 停止）
vtesse --config /etc/vtesse.conf --tap --ifname vtesse0 --hook /usr/share/vtesse/vtesse-hook.sh
```

开机自启（`S99vtesse` 已安装；可在 `/etc/default/vtesse` 中覆盖设置）：

```sh
/etc/init.d/S99vtesse start
/etc/init.d/S99vtesse status
```

### OpenWrt 路由器

```sh
opkg update && opkg install curl kmod-tun   # kmod-tun 仅在使用 --tap 时需要；OpenWrt 25+ 使用：apk add curl kmod-tun
# ARCH = mipsel（MT7621、MT7628）、mips（ath79）、arm64 或 arm32，见上表
ARCH=mipsel
URL=https://github.com/compot-dev/vtesse/releases/download/latest
mkdir -p /usr/bin /usr/share/vtesse /etc/init.d
curl -fsSL -o /usr/bin/vtesse $URL/vtesse-tap-$ARCH && chmod 755 /usr/bin/vtesse
curl -fsSL -o /usr/share/vtesse/vtesse-hook.sh $URL/vtesse-hook.sh && chmod 755 /usr/share/vtesse/vtesse-hook.sh
curl -fsSL -o /usr/share/vtesse/vtesse-udhcpc.script $URL/vtesse-udhcpc.script && chmod 755 /usr/share/vtesse/vtesse-udhcpc.script
curl -fsSL -o /etc/init.d/vtesse $URL/vtesse.init && chmod 755 /etc/init.d/vtesse
/etc/init.d/vtesse enable && /etc/init.d/vtesse start
```

### Keenetic 路由器（调研，[#32](https://github.com/OpenIPC/vtesse/issues/32)）

暂不支持。KeeneticOS（NDMS）是闭源固件：SSH 只提供 NDMS 命令行，没有 Linux shell。自有软件只能通过 **OPKG** 组件运行，它把 Entware 安装到存储卷上的 `/opt`（`opkg disk <卷> <安装包 URL>`），因此路由器需要 USB 存储或内部 `storage:` 分区。

已在 Netcraze Explorer NC-1621（Keenetic 换牌型号，NDMS 5.2 beta，MIPS，128 MB 内存，`small_flash`）上测试：`opkg` 组件可用并可安装（`components install opkg`、`components commit`，固件重新生成后路由器重启），但该型号没有 USB，也没有 `storage:` 卷，而将 Entware 安装到内存盘 `temp:` 会被拒绝（`unsuitable disk type`）。下一步：使用带 USB 接口的 Keenetic（MT7621 或 arm64），并制作 `.ipk` 软件包，包含 `vtesse-mipsel`/`vtesse-arm64`、Entware 启动脚本以及 TAP 后端所需的 NDMS 钩子。

### Linux PC、树莓派

```sh
ARCH=x86_64   # x86_64；64 位 / 32 位系统的树莓派用 arm64 / arm32
mkdir -p ~/.local/bin
curl -fsSL -o ~/.local/bin/vtesse https://github.com/compot-dev/vtesse/releases/download/latest/vtesse-$ARCH && chmod 755 ~/.local/bin/vtesse
# 无需 root：127.0.0.1:1080 上的 SOCKS5，以及到摄像机 RTSP 的转发
~/.local/bin/vtesse --config /etc/vtesse.conf --socks 1080 --forward 8554:10.78.0.10:554
```

### Android

Android 5.0 及以上（包括电视盒子）：[`vtesse-android.apk`](https://github.com/compot-dev/vtesse/releases/download/latest/vtesse-android.apk)。无需 VPN 权限、没有 VPN 图标——带普通通知的服务维持隧道，并在 `127.0.0.1` 上提供 SOCKS5 和端口转发；播放器（如 OpenIPC Decoder 或 VLC）即可打开 `rtsp://127.0.0.1:1551/...`（服务器 1 的摄像机 1）。最多三台服务器，每台最多 8 台摄像机，端口固定。设备 ID 在首次启动时生成（长按可复制），需要在服务器上添加为会话。最多三个服务器配置，一键切换。详见 [android/README.md](android/README.md)。

![VTesse Android 版：服务器配置与摄像头（示例数据）](docs/img/android-settings.png)

## 从源码构建

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

| 文档                                 | 内容                                                                                  |
|--------------------------------------|---------------------------------------------------------------------------------------|
| [Usage](docs/usage.md)               | 构建、`vtesse-cli` 选项、配置文件、设备 ID、DHCP 与路由、本地代理、错误信息、探测工具 |
| [VTun protocol](docs/protocol.md)    | 握手、质询应答、会话标志、帧格式、keepalive、影响客户端的服务器行为、UDP              |
| [Architecture](docs/architecture.md) | 分层、`tunnel_ops_t`、VTun 客户端、lwIP 协议栈、代理与流量控制、仓库结构              |
| [Embedding](docs/embedding.md)       | 在其他应用中使用这些库、事件循环集成、示例                                            |
| [Security](docs/security.md)         | VTun 认证的弱点、明文数据、本地代理的暴露面                                           |
| [VTun server](server/README.md)      | 打补丁的 vtund 构建、发布的二进制文件、补丁列表                                       |
| [Testing](docs/testing.md)           | 单元测试、sanitizer、大端序 MIPS、Docker 测试台与验收测试                             |

## 路线图

各阶段与任务记录在 [GitHub Issues](https://github.com/compot-dev/vtesse/issues) 中：

| 阶段                                                          | 状态      | Issues                                                                                                                                                                                                                                                                                                                                                                                           |
|---------------------------------------------------------------|-----------|--------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| 0. 测试台（Docker 中的 VTun 3.0.2、网桥、带选项 121 的 DHCP） | ✅ 已完成 | [#1](https://github.com/compot-dev/vtesse/issues/1) ✅                                                                                                                                                                                                                                                                                                                                           |
| 1. Linux 命令行原型：TCP + `type ether` + 基于 lwIP 的 DHCP   | ✅ 已完成 | [#2](https://github.com/compot-dev/vtesse/issues/2) ✅ [#3](https://github.com/compot-dev/vtesse/issues/3) ✅ [#4](https://github.com/compot-dev/vtesse/issues/4) ✅ [#5](https://github.com/compot-dev/vtesse/issues/5) ✅ [#6](https://github.com/compot-dev/vtesse/issues/6) ✅ [#7](https://github.com/compot-dev/vtesse/issues/7) ✅ [#8](https://github.com/compot-dev/vtesse/issues/8) ✅ |
| 2. 本地代理：SOCKS5 与端口转发                                | ✅ 已完成 | [#10](https://github.com/compot-dev/vtesse/issues/10) ✅ [#11](https://github.com/compot-dev/vtesse/issues/11) ✅                                                                                                                                                                                                                                                                                |
| 3. 嵌入式 Linux：ARM/MIPS 交叉编译、体积与内存、CI            | ✅ 已完成 | [#12](https://github.com/compot-dev/vtesse/issues/12) ✅ [#20](https://github.com/compot-dev/vtesse/issues/20) ✅                                                                                                                                                                                                                                                                                |
| 3. TAP/TUN 后端（用内核接口代替 lwIP，Linux 平台）            | ✅ 已完成 | [#19](https://github.com/compot-dev/vtesse/issues/19) ✅                                                                                                                                                                                                                                                                                                                                         |
| 4. Android 应用（不使用 VpnService）                          | ✅ 已完成 | [#13](https://github.com/compot-dev/vtesse/issues/13) ✅                                                                                                                                                                                                                                                                                                                                         |
| 5. 可选：UDP 传输、TLS 封装、Blowfish 认证                    | ⏸ 已推迟  | [#14](https://github.com/compot-dev/vtesse/issues/14) [#15](https://github.com/compot-dev/vtesse/issues/15) [#16](https://github.com/compot-dev/vtesse/issues/16)                                                                                                                                                                                                                                |
| 6. 在 camex 中复用上层模块                                    | ⏸ 已推迟  | [#17](https://github.com/compot-dev/vtesse/issues/17)                                                                                                                                                                                                                                                                                                                                            |
| 通用：测试、许可证                                            | ✅ 已完成 | [#9](https://github.com/compot-dev/vtesse/issues/9) ✅ [#18](https://github.com/compot-dev/vtesse/issues/18) ✅                                                                                                                                                                                                                                                                                  |

✅ — 已关闭的 issue。 ⏸ — 已推迟。

## 许可证

[The Prosperity Public License 3.0.0](LICENSE.md)：非商业用途免费，商业用途可试用 30 天。完整条款见 [LICENSE.md](LICENSE.md)。第三方代码沿用其自身的许可证（lwIP：BSD-3-Clause）。

## 支持

- 问题与缺陷报告：[GitHub Issues](https://github.com/compot-dev/vtesse/issues)
- 文档：[docs/](docs/)

[![Telegram](https://openipc.org/images/telegram_button.svg)](https://openipc.org/our-channels)

[logo]: https://raw.githubusercontent.com/OpenIPC/.github/refs/heads/main/assets/openipc-logo-black.svg
