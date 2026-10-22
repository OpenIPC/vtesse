# Security

> **Status: prototype.** Traffic is not encrypted.

## VTun authentication

- **XOR mode** (servers built without OpenSSL): the challenge and the response are
  sent in clear and `response = challenge XOR password` — one captured handshake
  reveals the first 16 password characters. The 3.0.2 challenge comes from
  `srand(time(NULL))` + `rand()` and is predictable.
- **Blowfish mode** (servers with OpenSSL): unsalted `MD5(password)` key — offline
  brute force is possible from a captured handshake.

## Data

- Data is sent in clear. VTesse refuses sessions with VTun encryption or compression;
  VTun's own encryption has no integrity protection anyway.
- Use only over trusted networks or inside an external secure channel (TLS /
  stunnel, an operator VPN). A TLS wrapper for the TCP transport is planned
  ([#15](https://github.com/OpenIPC/vtesse/issues/15)).

## Sessions

- With `multi killold` anyone who knows a session's credentials can take it over;
  the legitimate client sees "closed by server" and reconnects, and both keep kicking
  each other off.
- Never use production session names for tests.

## Local side

- The local proxy listens on `127.0.0.1` by default. Any process on the device can
  use an unauthenticated proxy — configure `socks_user` / `socks_password` where
  other applications run. Listening on other addresses (`--listen`) exposes the
  tunnel to the network.
- Credentials are read from a configuration file only (never from the command line,
  where other users could see them in the process list); keep it with mode 600. The
  CLI wipes the password from memory after use.
- Without `accept=` only outbound connections are possible: nothing behind the tunnel
  can connect to services of the device running VTesse (the tunnel address answers ARP
  and ping only). Each `accept=` line ([inbound forwards](usage.md#inbound-forwards))
  opens one port of the device to everyone on the VTun server side, in clear text. The
  service started by `vtesse --install` or the U-Boot environment opens 80, 22 and 554
  by default, to the device's LAN address: its own logins (web, ssh, RTSP) still
  apply, so keep them set; remove the `accept` lines that are not needed.

## U-Boot environment

- Settings in the [U-Boot environment](usage.md#u-boot-environment-helpdesk-access)
  (`vtesse`, `vtesse_*`) are readable by anyone with root (`fw_printenv`) or a dump of
  the flash, including `vtesse_password`, and they survive a firmware upgrade and
  `vtesse --uninstall`.
- With only `vtesse` set, the password is derived from the device's MAC the way
  OpenIPC's VTun client does it: anyone who knows the MAC (it is on the LAN and often on
  a label) knows the password. This mode is for quick helpdesk access, as VTun on
  OpenIPC, not for a tunnel that needs a secret.
