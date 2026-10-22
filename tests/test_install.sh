#!/bin/sh
# shellcheck disable=SC2034  # SCRIPTS, rc: used inside the eval strings of check
# tests/test_install.sh CLI — "vtesse --install" / "--uninstall" below --root
# (#50, #51, #52), the inbound defaults of the service (#67) and the clean-up of
# the TAP backend's files of 1.x (#72). Run by "make test".
set -u
CLI=$1
HERE=$(cd "$(dirname "$0")" && pwd)
SCRIPTS=$HERE/../scripts
fails=0
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT

check() { if eval "$2"; then echo "PASS  $1"; else echo "FAIL  $1"; fails=$((fails + 1)); fi; }

conf_has() { grep -qx "$2" "$1/etc/vtesse.conf"; }
# a build without the UDP relay (make UDP_RELAY=0) refuses udp_relay lines
if "$CLI" --version | grep -q udp-relay; then RELAY=udp_relay=1559; else RELAY="# no UDP relay in this build"; fi
mode() { stat -c %a "$1"; }

# ---- OpenIPC: detected from /etc/os-release
R=$T/openipc
mkdir -p "$R/etc"
printf 'NAME=Buildroot\nOPENIPC_VERSION=2.6.10.11\n' > "$R/etc/os-release"

"$CLI" --install --root "$R" >"$T/log" 2>&1; rc=$?
check "no config and no keys: refused" '[ $rc -ne 0 ] && grep -q "needs at least the server" "$T/log"'
check "nothing written when refused" '[ ! -e "$R/usr/bin/vtesse" ] && [ ! -e "$R/etc/vtesse.conf" ]'
"$CLI" --install --root "$R" --host vtun.example.org --password "$(printf 'p\033q')" --session 02AB34CD56EF \
    >"$T/log" 2>&1; rc=$?
check "a value with a control character: refused" '[ $rc -ne 0 ] && grep -q "control characters" "$T/log" && [ ! -e "$R/etc/vtesse.conf" ]'

"$CLI" --install --root "$R" --host vtun.example.org --password secret --session 02AB34CD56EF >"$T/log" 2>&1
rc=$?
check "OpenIPC: install succeeds" '[ $rc -eq 0 ]'
check "OpenIPC: system detected" 'grep -q "system: OpenIPC" "$T/log"'
check "OpenIPC: binary copied, executable" 'cmp -s "$CLI" "$R/usr/bin/vtesse" && [ -x "$R/usr/bin/vtesse" ]'
check "OpenIPC: S99vtesse from the built-in copy" 'cmp -s "$SCRIPTS/S99vtesse" "$R/etc/init.d/S99vtesse" && [ -x "$R/etc/init.d/S99vtesse" ]'
check "OpenIPC: config with the keys, port 5000 by default, mode 600" \
    'conf_has "$R" host=vtun.example.org && conf_has "$R" password=secret && conf_has "$R" session=02AB34CD56EF &&
     conf_has "$R" port=5000 && [ "$(mode "$R/etc/vtesse.conf")" = 600 ]'
check "OpenIPC: no hook files" '[ ! -e "$R/usr/libexec/vtesse" ]'
check "OpenIPC: a new config opens the camera's web, ssh and RTSP to the server (#67)" \
    'conf_has "$R" accept=80 && conf_has "$R" accept=22 && conf_has "$R" accept=554'

printf '# my note\nudp_relay=1559\n' >> "$R/etc/vtesse.conf"
"$CLI" --install --root "$R" --port 6000 --id-from eth1 >"$T/log" 2>&1
check "update: the given keys replace theirs, other lines stay" \
    'conf_has "$R" port=6000 && ! conf_has "$R" port=5000 && conf_has "$R" host=vtun.example.org &&
     conf_has "$R" "# my note" && conf_has "$R" udp_relay=1559'
check "update: id_from replaces session" 'conf_has "$R" id_from=eth1 && ! grep -q "^session=" "$R/etc/vtesse.conf"'
check "update: reported as updated" 'grep -q "updated .*/etc/vtesse.conf" "$T/log"'

chmod 644 "$R/etc/vtesse.conf"
"$CLI" --install --root "$R" >"$T/log" 2>&1
check "again without keys: config kept" 'grep -q "kept .*/etc/vtesse.conf" "$T/log"'
check "a kept config is made private (644 -> 600)" '[ "$(mode "$R/etc/vtesse.conf")" = 600 ]'

long=$(printf '#%02000d' 0)
echo "$long" >> "$R/etc/vtesse.conf"
"$CLI" --install --root "$R" --port 6001 >/dev/null 2>&1
check "a 2000-character line survives an update" 'conf_has "$R" "$long" && conf_has "$R" port=6001'

"$CLI" --install --root "$R" --scripts >"$T/log" 2>&1; rc=$?
check "--scripts: refused, the hook scripts went with the TAP backend" \
    '[ $rc -eq 2 ] && grep -q "TAP backend" "$T/log" && [ ! -e "$R/usr/libexec/vtesse" ]'
check "accept lines are written once" '[ "$(grep -c "^accept=" "$R/etc/vtesse.conf")" = 3 ]'
sed -i '/^accept=/d' "$R/etc/vtesse.conf"
echo accept=8080 >> "$R/etc/vtesse.conf"
"$CLI" --install --root "$R" >/dev/null 2>&1
check "a config with its own accept line keeps it, no defaults added" \
    'conf_has "$R" accept=8080 && [ "$(grep -c "^accept=" "$R/etc/vtesse.conf")" = 1 ]'
sed -i 's/^accept=8080$/accept=none/' "$R/etc/vtesse.conf"
"$CLI" --install --root "$R" >/dev/null 2>&1
check "accept=none: the opt-out is kept, no defaults added" \
    'conf_has "$R" accept=none && [ "$(grep -c "^accept=" "$R/etc/vtesse.conf")" = 1 ]'

"$CLI" --uninstall --root "$R" >"$T/log" 2>&1
check "uninstall: everything removed, the config included" \
    '[ ! -e "$R/usr/bin/vtesse" ] && [ ! -e "$R/etc/init.d/S99vtesse" ] && [ ! -e "$R/etc/vtesse.conf" ] &&
     [ ! -e "$R/usr/libexec/vtesse" ]'
"$CLI" --uninstall --root "$R" >"$T/log" 2>&1; rc=$?
check "uninstall on a clean system is not an error" '[ $rc -eq 0 ]'

# ---- OpenWrt: rc.d link and the sysupgrade keep list; hook scripts of 1.x removed
R=$T/openwrt
mkdir -p "$R/etc" "$R/usr/share/vtesse"
echo "DISTRIB_ID='OpenWrt'" > "$R/etc/openwrt_release"
echo "# edited by hand" > "$R/usr/share/vtesse/vtesse-hook.sh"
"$CLI" --install --root "$R" --host h --password p >"$T/log" 2>&1; rc=$?
check "--root without --session or --id-from: refused (the host's interfaces say nothing)" \
    '[ $rc -ne 0 ] && grep -q "with --root, give --session" "$T/log"'
"$CLI" --install --root "$R" --host h --password p --id-from eth0 >"$T/log" 2>&1
check "OpenWrt: system detected" 'grep -q "system: OpenWrt" "$T/log"'
check "OpenWrt: procd init script and its rc.d link" \
    'cmp -s "$SCRIPTS/vtesse.init" "$R/etc/init.d/vtesse" && [ "$(readlink "$R/etc/rc.d/S99vtesse")" = ../init.d/vtesse ]'
check "OpenWrt: keep list for sysupgrade" \
    'grep -qx /usr/bin/vtesse "$R/lib/upgrade/keep.d/vtesse" && grep -qx /etc/vtesse.conf "$R/lib/upgrade/keep.d/vtesse"'
check "OpenWrt: id_from as given" 'grep -qx "id_from=eth0" "$R/etc/vtesse.conf"'
check "hook scripts of 1.13 (/usr/share/vtesse) removed with the TAP backend" '[ ! -e "$R/usr/share/vtesse" ]'
check "OpenWrt: the keep list has no libexec any more" '! grep -q libexec "$R/lib/upgrade/keep.d/vtesse"'
check "OpenWrt: the keep list carries the rc.d link" 'grep -qx /etc/rc.d/S99vtesse "$R/lib/upgrade/keep.d/vtesse"'
"$CLI" --uninstall --root "$R" >/dev/null 2>&1

# an update of a 1.x installation: its libexec hook scripts go, TAP options in
# /etc/default/vtesse are reported (the client would refuse them), the file stays
mkdir -p "$R/usr/libexec/vtesse" "$R/etc/default"
echo "# hook" > "$R/usr/libexec/vtesse/vtesse-hook.sh"
echo "# udhcpc" > "$R/usr/libexec/vtesse/vtesse-udhcpc.script"
echo 'ARGS="--tap --ifname vtesse0"' > "$R/etc/default/vtesse"
"$CLI" --install --root "$R" --host h --password p --id-from eth0 >"$T/log" 2>&1
check "update from 1.x: libexec hook scripts removed" '[ ! -e "$R/usr/libexec/vtesse" ]'
check "update from 1.x: --tap in /etc/default/vtesse reported, the file kept" \
    'grep -q "TAP backend was removed" "$T/log" && grep -q -- --tap "$R/etc/default/vtesse"'
"$CLI" --uninstall --root "$R" >/dev/null 2>&1
check "OpenWrt: uninstall removes the link, the keep list and the scripts" \
    '[ ! -e "$R/etc/rc.d/S99vtesse" ] && [ ! -e "$R/lib/upgrade/keep.d/vtesse" ] && [ ! -e "$R/usr/libexec/vtesse" ]'

# ---- systemd (a PC): /usr/local
R=$T/pc
mkdir -p "$R/etc"
echo h | "$CLI" --install --root "$R" --system systemd --host h --password - --id-from eth0 >"$T/log" 2>&1
check "systemd: binary in /usr/local/bin, unit in /etc/systemd/system" \
    '[ -x "$R/usr/local/bin/vtesse" ] && cmp -s "$SCRIPTS/vtesse.service" "$R/etc/systemd/system/vtesse.service"'
check "--password - reads standard input" 'conf_has "$R" password=h'

# ---- a commented OPENIPC_VERSION is not OpenIPC; symlinks are not followed
R=$T/other
mkdir -p "$R/etc/init.d" "$T/outside"
echo 'for i in /etc/init.d/S??* ; do "$i" start; done' > "$R/etc/init.d/rcS"
printf 'NAME=Debian\n#OPENIPC_VERSION=1\n' > "$R/etc/os-release"
"$CLI" --install --root "$R" --host h --password p --id-from eth0 >"$T/log" 2>&1
check "commented OPENIPC_VERSION: BusyBox init, not OpenIPC" 'grep -q "system: BusyBox" "$T/log"'
ln -s "$T/outside/conf" "$R/etc/vtesse.conf.new"
"$CLI" --install --root "$R" --port 6000 >/dev/null 2>&1
check "a symlink at PATH.new is not followed" '[ ! -e "$T/outside/conf" ] && conf_has "$R" port=6000'
"$CLI" --uninstall --root "$R" >/dev/null 2>&1
echo keep > "$T/outside/vtesse-hook.sh"
mkdir -p "$R/usr/share"
ln -s "$T/outside" "$R/usr/libexec"
ln -s "$T/outside" "$R/usr/share/vtesse"
"$CLI" --uninstall --root "$R" >/dev/null 2>&1
check "uninstall does not delete through a symlinked directory" '[ -f "$T/outside/vtesse-hook.sh" ]'

# ---- /etc/init.d without BusyBox init (OpenRC, sysvinit): not guessed
R=$T/openrc
mkdir -p "$R/etc/init.d" "$R/run/openrc"
"$CLI" --install --root "$R" --host h --password p --id-from eth0 >"$T/log" 2>&1; rc=$?
check "OpenRC: unknown system, asks for --system" '[ $rc -ne 0 ] && grep -q "name one with --system" "$T/log"'
"$CLI" --install --root "" --host h >/dev/null 2>&1; rc=$?
check "--root \"\": refused" '[ $rc -eq 2 ]'

# ---- bad arguments
R=$T/pc
"$CLI" --install --root "$R" --session A --id-from eth0 >/dev/null 2>&1; rc=$?
check "--session and --id-from together: refused" '[ $rc -eq 2 ]'
"$CLI" --install --root "$R" --port 70000 >/dev/null 2>&1; rc=$?
check "bad --port: refused" '[ $rc -eq 2 ]'
"$CLI" --install --root "$R" --system windows >/dev/null 2>&1; rc=$?
check "unknown --system: refused" '[ $rc -eq 2 ]'
"$CLI" --install --root "$R" --session "AB CD" >/dev/null 2>&1; rc=$?
check "session with a space: refused" '[ $rc -eq 2 ]'
"$CLI" --install --root "$R" --password "$(printf '%0128d' 0)" >/dev/null 2>&1; rc=$?
check "password of 128 characters: refused (127 is the limit)" '[ $rc -eq 2 ]'
"$CLI" --install --root "$R" --system systemd --password "$(printf '%0127d' 0)" >/dev/null 2>&1; rc=$?
check "password of 127 characters: accepted" '[ $rc -eq 0 ]'
"$CLI" --install --root "$R" --password " secret" >/dev/null 2>&1; rc=$?
check "password with a leading space: refused" '[ $rc -eq 2 ]'
"$CLI" --install --root "$R" --id-from ../eth0 >/dev/null 2>&1; rc=$?
check "--id-from with a slash: refused" '[ $rc -eq 2 ]'
"$CLI" --install --root "$R" --ip nope >/dev/null 2>&1; rc=$?
check "--ip without a prefix: refused" '[ $rc -eq 2 ]'
"$CLI" --install --root "$R" --system systemd --gw 10.0.0.1 >/dev/null 2>&1; rc=$?
check "--gw without ip: refused" '[ $rc -ne 0 ] && ! grep -q "^gw=" "$R/etc/vtesse.conf"'
R=$T/dup
mkdir -p "$R/etc"
printf 'host=server\nhost=\npassword=p\nsession=02AB34CD56EF\n' > "$R/etc/vtesse.conf"
"$CLI" --install --root "$R" --system systemd >/dev/null 2>&1; rc=$?
check "a later empty host= wins, as in the client: refused" '[ $rc -ne 0 ]'
printf 'host=server\npassword=p\nsession=02AB34CD56EF\nid_from=eth0\n' > "$R/etc/vtesse.conf"
"$CLI" --install --root "$R" --system systemd >/dev/null 2>&1; rc=$?
check "both session and id_from in the file: refused" '[ $rc -ne 0 ]'
printf 'host=server\npassword=p\nsession=02AB34CD56EF\nport=abc\n' > "$R/etc/vtesse.conf"
"$CLI" --install --root "$R" --system systemd >/dev/null 2>&1; rc=$?
check "port=abc in the file: refused" '[ $rc -ne 0 ]'

# ---- TAP options of 1.x: refused with the reason (#72)
"$CLI" --tap --config "$T/none.conf" >"$T/log" 2>&1; rc=$?
check "--tap: refused, the TAP backend was removed" '[ $rc -eq 2 ] && grep -q "TAP backend was removed" "$T/log"'
printf 'host=h\nport=5000\nsession=02AB34CD56EF\npassword=p\ntap=1\n' > "$T/tap.conf"
"$CLI" --no-env --config "$T/tap.conf" --show-config >"$T/log" 2>&1; rc=$?
check "tap= in the configuration: refused, not ignored" '[ $rc -ne 0 ] && grep -q "tap: the TAP backend was removed" "$T/log"'

# ---- U-Boot environment (#64): fake fw_printenv / fw_setenv in PATH
mkdir -p "$T/env"
cat > "$T/env/fw_printenv" <<EOS
#!/bin/sh
cat "$T/env/vars" 2>/dev/null
exit "\$(cat "$T/env/rc" 2>/dev/null || echo 0)"
EOS
printf '#!/bin/sh\necho "\$*" >> "%s/env/setenv.log"\n' "$T" > "$T/env/fw_setenv"
chmod 755 "$T/env/fw_printenv" "$T/env/fw_setenv"
EP=$T/env:$PATH
iface=$(awk '$2 == "00000000" { print $1; exit }' /proc/net/route 2>/dev/null)
if [ -n "$iface" ] && [ -r "/sys/class/net/$iface/address" ]; then
    mac=$(tr a-f A-F < "/sys/class/net/$iface/address")
    id=$(echo "$mac" | tr -d :)
    pw=$(echo "$mac" | sha1sum | cut -d" " -f1)
    echo "vtesse=vtun.helpdesk" > "$T/env/vars"
    PATH=$EP "$CLI" --config "$T/none.conf" --show-config >"$T/log" 2>&1
    check "env: only the server -> port 5000, session = MAC of $iface, password as VTun" \
        'grep -qx "host vtun.helpdesk" "$T/log" && grep -qx "port 5000" "$T/log" &&
         grep -qx "session $id" "$T/log" && grep -qx "password $pw" "$T/log"'
    check "env: the service opens 80, 22 and 554 to the default-route address (#67)" \
        'grep -qx "accept 80 -> (default-route address):80" "$T/log" &&
         grep -qx "accept 22 -> (default-route address):22" "$T/log" &&
         grep -qx "accept 554 -> (default-route address):554" "$T/log"'
    printf 'accept=none\n' > "$T/none-accept.conf"
    echo "vtesse=vtun.helpdesk" > "$T/env/vars"
    PATH=$EP "$CLI" --config "$T/none-accept.conf" --show-config >"$T/log" 2>&1
    check "env: accept=none turns the default ports off" '! grep -q "^accept " "$T/log" && grep -qx "host vtun.helpdesk" "$T/log"'
    printf 'accept=none\naccept=80\n' > "$T/none-accept.conf"
    PATH=$EP "$CLI" --config "$T/none-accept.conf" --show-config >"$T/log" 2>&1; rc=$?
    check "accept=none with another accept line: refused" '[ $rc -eq 2 ] && grep -q "accept=none cannot be combined" "$T/log"'
    printf 'vtesse=vtun.helpdesk\nvtesse_session=02AB34CD56EF\n' > "$T/env/vars"
    PATH=$EP "$CLI" --config "$T/none.conf" --show-config >"$T/log" 2>&1
    check "env: an explicit session still gets the MAC-derived password" \
        'grep -qx "session 02AB34CD56EF" "$T/log" && grep -qx "password $pw" "$T/log"'
else
    echo "SKIP  env defaults: no default route"
fi
printf 'vtesse=vtun.helpdesk\nvtesse_port=60x0\nvtesse_password=p\nvtesse_session=02AB34CD56EF\n' > "$T/env/vars"
PATH=$EP "$CLI" --config "$T/none.conf" --show-config >"$T/log" 2>&1; rc=$?
check "a mistyped vtesse_port is refused, not turned into 5000" '[ $rc -ne 0 ] && grep -q "vtesse_port: expected" "$T/log"'
printf 'host=file.example\nport=6000\nsession=02AB34CD56EF\npassword=filepw\n' > "$T/f.conf"
printf 'vtesse=env.example\nvtesse_port=7000\nbootargs=x\n' > "$T/env/vars"
PATH=$EP "$CLI" --config "$T/f.conf" --show-config >"$T/log" 2>&1
check "env overrides the file key by key" \
    'grep -qx "host env.example" "$T/log" && grep -qx "port 7000" "$T/log" &&
     grep -qx "session 02AB34CD56EF" "$T/log" && grep -qx "password (set)" "$T/log"'
printf 'route=10.78.0.0/24\nip=10.76.0.2/32\n%s\naccept=8080:127.0.0.1:80\n' "$RELAY" >> "$T/f.conf"
PATH=$EP "$CLI" --config "$T/f.conf" --show-config >"$T/log" 2>&1
check "--show-config lists routes, UDP relays and accepts" \
    'grep -qx "route 10.78.0.0/24" "$T/log" && { [ "$RELAY" != udp_relay=1559 ] || grep -qx "udp_relay 1559" "$T/log"; } &&
     grep -qx "accept 8080 -> 127.0.0.1:80" "$T/log" && ! grep -q "accept 80 " "$T/log"'
sed -i '/^accept=/d' "$T/f.conf"
printf 'port=abc\n' >> "$T/f.conf"
PATH=$EP "$CLI" --config "$T/f.conf" --show-config >"$T/log" 2>&1; rc=$?
printf 'vtesse=env.example\nvtesse_port=7000\n' > "$T/env/vars"
check "a port=abc in the file is not taken for 5000 (the variable wins here)" '[ $rc -eq 0 ] && grep -qx "port 7000" "$T/log"'
echo vtesse=env.example > "$T/env/vars"
PATH=$EP "$CLI" --config "$T/f.conf" --show-config >"$T/log" 2>&1; rc=$?
check "a port=abc in the file with only vtesse set: refused" '[ $rc -ne 0 ]'
sed -i '/^port=abc$/d' "$T/f.conf"
printf 'vtesse=env.example\nvtesse_port=7000\nbootargs=x\n' > "$T/env/vars"
PATH=$EP "$CLI" --config "$T/f.conf" --show-config --no-env >"$T/log" 2>&1
check "--no-env: the file only" 'grep -qx "host file.example" "$T/log" && grep -qx "port 6000" "$T/log"'
printf 'vtesse_port=7100\n' > "$T/env/vars"
PATH=$EP "$CLI" --config "$T/f.conf" --show-config >"$T/log" 2>&1
check "vtesse_port alone (no vtesse) still overrides the file's port" 'grep -qx "host file.example" "$T/log" && grep -qx "port 7100" "$T/log"'
printf 'vtesse=h\nvtesse_session=AB CD\nvtesse_password=p\n' > "$T/env/vars"
PATH=$EP "$CLI" --config "$T/none.conf" --show-config >"$T/log" 2>&1; rc=$?
check "--show-config refuses a session the client would refuse" '[ $rc -eq 2 ]'
printf 'vtesse=h\nvtesse_session=02AB34CD56EF\nvtesse_id_from=eth9\n' > "$T/env/vars"
PATH=$EP "$CLI" --config "$T/none.conf" --show-config >"$T/log" 2>&1; rc=$?
check "vtesse_session and vtesse_id_from both set: refused, not picked by order" \
    '[ $rc -eq 2 ] && grep -q "both set" "$T/log"'
printf 'vtesse=h\nvtesse_session=02AB34CD56EF\nvtesse_password=p\nvtesse_ip=10.0.0.2/garbage\n' > "$T/env/vars"
PATH=$EP "$CLI" --config "$T/none.conf" --show-config >"$T/log" 2>&1; rc=$?
check "vtesse_ip with a malformed prefix: refused, not taken for /0" '[ $rc -eq 2 ]'
printf 'host=file.example\nport=6000\nsession=02AB34CD56EF\npassword=filepw\nudp_relay=not-a-port\n' > "$T/r.conf"
echo vtesse=env.example > "$T/env/vars"
PATH=$EP "$CLI" --config "$T/r.conf" --show-config >"$T/log" 2>&1; rc=$?
if [ "$RELAY" = udp_relay=1559 ]; then
    check "--show-config refuses a udp_relay the client would refuse" '[ $rc -eq 2 ] && grep -q "udp_relay: expected" "$T/log"'
else
    check "--show-config refuses udp_relay in a build without it" '[ $rc -eq 2 ] && grep -q "built without the UDP relay" "$T/log"'
fi
printf 'vtesse=env.example\nvtesse_port=7000\n' > "$T/env/vars"
echo 1 > "$T/env/rc"
PATH=$EP "$CLI" --config "$T/f.conf" --show-config >"$T/log" 2>&1
check "a failed fw_printenv: nothing it printed is used, and it is said" \
    'grep -q "fw_printenv failed" "$T/log" && grep -qx "host file.example" "$T/log" && grep -qx "port 6000" "$T/log"'
: > "$T/env/vars"
PATH=$EP "$CLI" --config "$T/f.conf" --show-config >"$T/log" 2>&1
check "a failed fw_printenv without vtesse variables: silent" '! grep -q "fw_printenv failed" "$T/log"'
rm -f "$T/env/rc"
rm -f "$T/env/vars"
PATH=$EP "$CLI" --config "$T/none.conf" --show-config >"$T/log" 2>&1; rc=$?
check "no file and no vtesse variable: refused" '[ $rc -ne 0 ] && grep -q "cannot open" "$T/log"'

R=$T/envinst
mkdir -p "$R/etc"
printf 'NAME=Buildroot\nOPENIPC_VERSION=2.6\n' > "$R/etc/os-release"
echo "vtesse=vtun.helpdesk" > "$T/env/vars"
PATH=$EP "$CLI" --install --root "$R" >"$T/log" 2>&1; rc=$?
check "--root: this machine's environment does not stand in for the image's config" \
    '[ $rc -ne 0 ] && [ ! -e "$R/usr/bin/vtesse" ] && grep -q "needs at least the server" "$T/log"'
PATH=$EP "$CLI" --install --root "$R" --host h --password p >/dev/null 2>&1
check "--root: fw_setenv is not called" '[ ! -e "$T/env/setenv.log" ]'
PATH=$EP "$CLI" --uninstall --root "$R" >"$T/log" 2>&1
check "uninstall --root: no fw_setenv, and this machine's variables are not reported" \
    '! grep -q "keeps vtesse" "$T/log" && [ ! -e "$T/env/setenv.log" ]'

echo "$fails failure(s)"
[ "$fails" -eq 0 ]
