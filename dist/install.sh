#!/bin/sh
# Run on the MiSTer as root from the folder holding bennugd, bennugd.cfg
# and bennugd-launcherd.sh:   sh install.sh [BennuGD_yyyymmdd.rbf]
# Installs the frontend, the launcher daemon (started at boot from
# user-startup.sh, and right now), and optionally the core.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
D=/media/fat/bennugd
mkdir -p "$D" /media/fat/games/BennuGD /media/fat/_Other /media/fat/linux
for f in bennugd bennugd-launcherd.sh; do
    [ "$HERE/$f" -ef "$D/$f" ] || cp "$HERE/$f" "$D/$f"
    chmod +x "$D/$f"
done
[ -f "$D/bennugd.cfg" ] || cp "$HERE/bennugd.cfg" "$D/bennugd.cfg"
[ -n "$1" ] && cp "$1" /media/fat/_Other/
# the old main= handoff must not be present: Main would exec us and skip its video setup
INI=/media/fat/MiSTer.ini
sed -i '/^\[BennuGD\]$/,/^main=bennugd\/bennugd$/d' "$INI"
US=/media/fat/linux/user-startup.sh
[ -f "$US" ] || printf '#!/bin/sh\n' > "$US"
grep -q bennugd-launcherd "$US" || printf '\n# BennuGD core: start the frontend when the core is loaded\n%s/bennugd-launcherd.sh >/dev/null 2>&1 &\n' "$D" >> "$US"
chmod +x "$US"
for p in $(pidof -x bennugd-launcherd.sh 2>/dev/null); do kill "$p"; done
"$D/bennugd-launcherd.sh" >/dev/null 2>&1 < /dev/null &
echo "installed; launcher daemon running. Game data: /media/fat/games/BennuGD/<Game>/, game= in $D/bennugd.cfg"
