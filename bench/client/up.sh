#!/bin/sh
# `up` hook of the stock vtund client: obtain address and routes via DHCP
# (Debian dhclient requests option 121 and applies it per RFC 3442).
dev=$1
dhclient -v -1 -pf "/run/dhclient-$dev.pid" -lf "/var/lib/dhcp/dhclient-$dev.leases" "$dev" \
    >"/var/log/dhclient-$dev.log" 2>&1 &
