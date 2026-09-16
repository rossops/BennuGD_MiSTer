#!/bin/sh
# bennugd-launcherd: runs the BennuGD frontend while Main_MiSTer has the
# BennuGD core loaded. Started at boot from /media/fat/linux/user-startup.sh
# by dist/install.sh.
#
# Main writes the core name to /tmp/CORENAME. With log_file_entry=1 it also
# writes the path of a file chosen in the OSD, or by an .mgl, to
# /tmp/FULLPATH, as it sees it: absolute, or relative to the core's games
# folder, or to the card root. A .dat/.dcb under our games folder selects
# the game; when that path changes while the core is loaded the frontend
# is restarted on the new game. Without one, game= in bennugd.cfg is used.
BIN=/media/fat/bennugd/bennugd
GAMES=/media/fat/games/BennuGD
LOG=/media/fat/bennugd/launcherd.log
last_core=""
last_game=""
pid=""

log() { echo "$(date '+%H:%M:%S') $*" >> "$LOG"; }

resolve_game() {
    [ -f /tmp/FULLPATH ] || return 1
    p=$(cat /tmp/FULLPATH)
    case "$p" in *.dat|*.DAT|*.dcb|*.DCB) ;; *) return 1 ;; esac
    for c in "$p" "$GAMES/$p" "/media/fat/games/$p" "/media/fat/$p"; do
        case "$c" in "$GAMES"/*) [ -f "$c" ] && { echo "$c"; return 0; } ;; esac
    done
    return 1
}

stop_game() {
    # ours, and any frontend left behind by an earlier daemon
    for p in $pid $(pidof bennugd 2>/dev/null); do kill -TERM "$p" 2>/dev/null; done
    sleep 1
    for p in $(pidof bennugd 2>/dev/null); do kill -KILL "$p" 2>/dev/null; done
    [ -n "$pid" ] && wait "$pid" 2>/dev/null
    pid=""
}

# only one of us: an older instance would keep launching its own frontend
for p in $(ps | grep "[b]ennugd-launcherd" | awk '{print $1}'); do [ "$p" != "$$" ] && kill "$p" 2>/dev/null; done
log "daemon start (pid $$)"
while :; do
    core=$(cat /tmp/CORENAME 2>/dev/null)
    game=""
    if [ "$core" = "BennuGD" ]; then
        game=$(resolve_game)
        if [ "$core" != "$last_core" ] && [ -z "$game" ]; then
            n=0   # an .mgl mounts its file after its delay: give it a few seconds
            while [ $n -lt 8 ] && [ -z "$game" ]; do sleep 0.5; n=$((n + 1)); game=$(resolve_game); done
        fi
    fi
    if [ "$core" != "$last_core" ] || [ "$game" != "$last_game" ]; then
        stop_game
        if [ "$core" = "BennuGD" ]; then
            # prof=: the built-in 1 kHz sampler, negligible cost, keeps only over-budget frames
            if [ -n "$game" ]; then
                "$BIN" --core "game=$game" prof=/media/fat/bennugd/prof.bin &
            else
                "$BIN" --core prof=/media/fat/bennugd/prof.bin &
            fi
            pid=$!
            log "core $core: started pid $pid game=${game:-<cfg default>}"
        else
            log "core $core: frontend stopped"
        fi
        last_core=$core
        last_game=$game
    fi
    sleep 0.5
done
