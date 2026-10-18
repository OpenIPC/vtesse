#!/bin/sh
# shellcheck disable=SC2034  # SCRIPTS, rc: used inside the eval strings of check
# tests/test_install.sh CLI — "vtesse --install" / "--uninstall" below --root
# (#50, #51, #52), and the built-in udhcpc script. Run by "make test".
set -u
CLI=$1
HERE=$(cd "$(dirname "$0")" && pwd)
SCRIPTS=$HERE/../scripts
fails=0
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT

check() { if eval "$2"; then echo "PASS  $1"; else echo "FAIL  $1"; fails=$((fails + 1)); fi; }

if ! "$CLI" --version | grep -q tap; then
    echo "SKIP  built without the TAP backend"
    exit 0
fi

conf_has() { grep -qx "$2" "$1/etc/vtesse.conf"; }
mode() { stat -c %a "$1"; }

# ---- OpenIPC: detected from /etc/os-release
R=$T/openipc
mkdir -p "$R/etc"
printf 'NAME=Buildroot\nOPENIPC_VERSION=2.6.10.11\n' > "$R/etc/os-release"

"$CLI" --install --root "$R" >"$T/log" 2>&1; rc=$?
check "no config and no keys: refused" '[ $rc -ne 0 ] && grep -q "needs at least the server" "$T/log"'
check "nothing written when refused" '[ ! -e "$R/usr/bin/vtesse" ] && [ ! -e "$R/etc/vtesse.conf" ]'

"$CLI" --install --root "$R" --host vtun.example.org --password secret --session 02AB34CD56EF >"$T/log" 2>&1
rc=$?
check "OpenIPC: install succeeds" '[ $rc -eq 0 ]'
check "OpenIPC: system detected" 'grep -q "system: OpenIPC" "$T/log"'
check "OpenIPC: binary copied, executable" 'cmp -s "$CLI" "$R/usr/bin/vtesse" && [ -x "$R/usr/bin/vtesse" ]'
check "OpenIPC: S99vtesse from the built-in copy" 'cmp -s "$SCRIPTS/S99vtesse" "$R/etc/init.d/S99vtesse" && [ -x "$R/etc/init.d/S99vtesse" ]'
check "OpenIPC: config with the keys, port 5000 by default, mode 600" \
    'conf_has "$R" host=vtun.example.org && conf_has "$R" password=secret && conf_has "$R" session=02AB34CD56EF &&
     conf_has "$R" port=5000 && [ "$(mode "$R/etc/vtesse.conf")" = 600 ]'
check "OpenIPC: no hook files by default" '[ ! -e "$R/usr/libexec/vtesse" ]'

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

"$CLI" --install --root "$R" --scripts >/dev/null 2>&1
check "--scripts: hook scripts in /usr/libexec/vtesse" \
    'cmp -s "$SCRIPTS/vtesse-hook.sh" "$R/usr/libexec/vtesse/vtesse-hook.sh" &&
     cmp -s "$SCRIPTS/vtesse-udhcpc.script" "$R/usr/libexec/vtesse/vtesse-udhcpc.script"'

"$CLI" --uninstall --root "$R" >"$T/log" 2>&1
check "uninstall: everything removed, the config included" \
    '[ ! -e "$R/usr/bin/vtesse" ] && [ ! -e "$R/etc/init.d/S99vtesse" ] && [ ! -e "$R/etc/vtesse.conf" ] &&
     [ ! -e "$R/usr/libexec/vtesse" ]'
"$CLI" --uninstall --root "$R" >"$T/log" 2>&1; rc=$?
check "uninstall on a clean system is not an error" '[ $rc -eq 0 ]'

# ---- OpenWrt: rc.d link and the sysupgrade keep list; old hook scripts moved
R=$T/openwrt
mkdir -p "$R/etc" "$R/usr/share/vtesse"
echo "DISTRIB_ID='OpenWrt'" > "$R/etc/openwrt_release"
echo "# edited by hand" > "$R/usr/share/vtesse/vtesse-hook.sh"
"$CLI" --install --root "$R" --host h --password p >"$T/log" 2>&1
check "OpenWrt: system detected" 'grep -q "system: OpenWrt" "$T/log"'
check "OpenWrt: procd init script and its rc.d link" \
    'cmp -s "$SCRIPTS/vtesse.init" "$R/etc/init.d/vtesse" && [ "$(readlink "$R/etc/rc.d/S99vtesse")" = ../init.d/vtesse ]'
check "OpenWrt: keep list for sysupgrade" \
    'grep -qx /usr/bin/vtesse "$R/lib/upgrade/keep.d/vtesse" && grep -qx /etc/vtesse.conf "$R/lib/upgrade/keep.d/vtesse"'
check "OpenWrt: device ID from an interface when no session is given" 'grep -q "^id_from=" "$R/etc/vtesse.conf"'
check "1.13 hook scripts moved from /usr/share/vtesse to /usr/libexec/vtesse" \
    'grep -q "edited by hand" "$R/usr/libexec/vtesse/vtesse-hook.sh" && [ ! -e "$R/usr/share/vtesse" ]'
check "OpenWrt: the keep list carries the rc.d link" 'grep -qx /etc/rc.d/S99vtesse "$R/lib/upgrade/keep.d/vtesse"'
"$CLI" --uninstall --root "$R" >/dev/null 2>&1

# a hand-edited 1.13 hook survives --install --scripts; /etc/default/vtesse follows the move
mkdir -p "$R/usr/share/vtesse" "$R/etc/default"
echo "# edited by hand" > "$R/usr/share/vtesse/vtesse-hook.sh"
echo 'ARGS="--tap --ifname vtesse0 --hook /usr/share/vtesse/vtesse-hook.sh"' > "$R/etc/default/vtesse"
"$CLI" --install --root "$R" --host h --password p --scripts >"$T/log" 2>&1
check "--scripts keeps a moved hand-edited hook" \
    'grep -q "edited by hand" "$R/usr/libexec/vtesse/vtesse-hook.sh" &&
     cmp -s "$SCRIPTS/vtesse-udhcpc.script" "$R/usr/libexec/vtesse/vtesse-udhcpc.script"'
check "/etc/default/vtesse: --hook path moved to /usr/libexec/vtesse" \
    'grep -q -- "--hook /usr/libexec/vtesse/vtesse-hook.sh" "$R/etc/default/vtesse"'
"$CLI" --uninstall --root "$R" >/dev/null 2>&1
check "OpenWrt: uninstall removes the link, the keep list and the scripts" \
    '[ ! -e "$R/etc/rc.d/S99vtesse" ] && [ ! -e "$R/lib/upgrade/keep.d/vtesse" ] && [ ! -e "$R/usr/libexec/vtesse" ]'

# ---- systemd (a PC): /usr/local
R=$T/pc
mkdir -p "$R/etc"
echo h | "$CLI" --install --root "$R" --system systemd --host h --password - >"$T/log" 2>&1
check "systemd: binary in /usr/local/bin, unit in /etc/systemd/system" \
    '[ -x "$R/usr/local/bin/vtesse" ] && cmp -s "$SCRIPTS/vtesse.service" "$R/etc/systemd/system/vtesse.service"'
check "--password - reads standard input" 'conf_has "$R" password=h'

# ---- a commented OPENIPC_VERSION is not OpenIPC; symlinks are not followed
R=$T/other
mkdir -p "$R/etc/init.d" "$T/outside"
printf 'NAME=Debian\n#OPENIPC_VERSION=1\n' > "$R/etc/os-release"
"$CLI" --install --root "$R" --host h --password p >"$T/log" 2>&1
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

# ---- the built-in udhcpc script: the binary runs it with VTESSE_RUN=udhcpc-script
mkdir -p "$T/bin"
printf '#!/bin/sh\necho "ip $*" >> "%s/ip.log"\n' "$T" > "$T/bin/ip"
chmod 755 "$T/bin/ip"
PATH=$T/bin:$PATH VTESSE_RUN=udhcpc-script interface=vt0 "$CLI" deconfig
check "built-in udhcpc script runs (deconfig flushes the address)" 'grep -q "ip -4 addr flush dev vt0" "$T/ip.log"'

echo "$fails failure(s)"
[ "$fails" -eq 0 ]
