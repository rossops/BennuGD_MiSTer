#!/bin/sh
# Push the HPS binary (and optionally the game data) to the MiSTer.
#   tools/deploy.sh bin            copy hps/out/bennugd to /media/fat/bennugd/
#   tools/deploy.sh game <dir>     copy a game folder to /media/fat/games/BennuGD/<name>/
#                                  (only .dat/.dcb, mod/, palettes/, savegame/; no Windows binaries)
#   tools/deploy.sh run "<cmd>"    run a command on the MiSTer
# MISTER_HOST / MISTER_PW as in tools/mister_ssh.sh.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
SSH="$HERE/mister_ssh.sh"
case "$1" in
  bin)
    "$SSH" run "mkdir -p /media/fat/bennugd"
    "$SSH" put "$HERE/../hps/out/bennugd" /media/fat/bennugd/bennugd
    "$SSH" run "chmod +x /media/fat/bennugd/bennugd && ls -l /media/fat/bennugd"
    ;;
  game)
    src=$2; name=$(basename "$src")
    dst="/media/fat/games/BennuGD/$name"
    "$SSH" run "mkdir -p $dst"
    for f in "$src"/*.dat "$src"/*.dcb; do [ -e "$f" ] && "$SSH" put "$f" "$dst/"; done
    for d in mod palettes savegame; do
      [ -d "$src/$d" ] || continue
      tmp=$(mktemp -t bennugd).tar.gz
      tar -C "$src" --exclude='*.dll' --exclude='*.exe' -czf "$tmp" "$d"
      # stage on the SD card: /tmp on the MiSTer is a small tmpfs
      "$SSH" put "$tmp" "$dst/.$d.tar.gz"
      "$SSH" run "tar -C $dst -oxzf $dst/.$d.tar.gz && rm $dst/.$d.tar.gz"
      rm -f "$tmp"
    done
    "$SSH" run "ls -la $dst"
    ;;
  run) "$SSH" run "$2" ;;
  *) sed -n 2,7p "$0"; exit 2 ;;
esac
