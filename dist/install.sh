#!/bin/sh
# Run on the MiSTer as root from the folder holding bennugd, bennugd.cfg,
# bennugd-launcherd.sh and mgl/:   sh install.sh [BennuGD_yyyymmdd.rbf]
# Installs the frontend and the launcher daemon (started at boot from
# user-startup.sh, and right now), puts the core and the game entries under
# _Other/BennuGD, and sets the MiSTer.ini options the daemon relies on.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
D=/media/fat/bennugd
CORES=/media/fat/_Other/_BennuGD
mkdir -p "$D" /media/fat/games/BennuGD "$CORES" /media/fat/linux
for f in bennugd bennugd-launcherd.sh; do
    [ "$HERE/$f" -ef "$D/$f" ] || cp "$HERE/$f" "$D/$f"
    chmod +x "$D/$f"
done
[ -f "$D/bennugd.cfg" ] || cp "$HERE/bennugd.cfg" "$D/bennugd.cfg"
if [ -n "$1" ]; then
    cp "$1" "$CORES/"
    rm -f /media/fat/_Other/BennuGD_*.rbf      # older layout: the core next to the folder
fi
[ -d "$HERE/mgl" ] && cp "$HERE"/mgl/*.mgl "$CORES/"
INI=/media/fat/MiSTer.ini
sed -i '/^\[BennuGD\]$/,/^main=bennugd\/bennugd$/d' "$INI"    # the abandoned main= handoff
if ! grep -q '^\[BennuGD\]' "$INI"; then
    printf '\n[BennuGD]\nlog_file_entry=1\n' >> "$INI"          # Main records the chosen game in /tmp/FULLPATH
fi
US=/media/fat/linux/user-startup.sh
[ -f "$US" ] || printf '#!/bin/sh\n' > "$US"
grep -q bennugd-launcherd "$US" || printf '\n# BennuGD core: start the frontend when the core is loaded\n%s/bennugd-launcherd.sh >/dev/null 2>&1 &\n' "$D" >> "$US"
chmod +x "$US"
# stop every running daemon (busybox pidof -x does not see shell scripts) and frontend
for p in $(ps | grep "[b]ennugd-launcherd" | awk '{print $1}'); do kill "$p" 2>/dev/null; done
for p in $(pidof bennugd 2>/dev/null); do kill "$p" 2>/dev/null; done     # the new daemon restarts the game
sleep 1
"$D/bennugd-launcherd.sh" >/dev/null 2>&1 < /dev/null &
echo "installed. Menu: Other -> BennuGD -> <game>. Game data: /media/fat/games/BennuGD/<Game>/"
