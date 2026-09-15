#!/bin/sh
# bennugd-launcherd: starts the BennuGD frontend when Main_MiSTer loads the
# BennuGD core (Main writes the core name to /tmp/CORENAME) and stops it
# when another core is loaded. Started at boot from
# /media/fat/linux/user-startup.sh by dist/install.sh.
BIN=/media/fat/bennugd/bennugd
last=""
pid=""
while :; do
    name=$(cat /tmp/CORENAME 2>/dev/null)
    if [ "$name" != "$last" ]; then
        if [ -n "$pid" ]; then
            kill -TERM "$pid" 2>/dev/null
            sleep 1
            kill -KILL "$pid" 2>/dev/null
            wait "$pid" 2>/dev/null
            pid=""
        fi
        if [ "$name" = "BennuGD" ]; then
            sleep 1      # let Main finish the video mode setup
            "$BIN" --core &
            pid=$!
        fi
        last=$name
    fi
    sleep 0.5
done
