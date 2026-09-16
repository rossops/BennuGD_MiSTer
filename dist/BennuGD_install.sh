#!/bin/sh
# Run from the MiSTer OSD: Scripts -> BennuGD_install. Finishes the install
# of the BennuGD core after the release zip has been unpacked onto the card.
if [ ! -f /media/fat/bennugd/install.sh ]; then
    echo "Unpack the BennuGD release zip onto the SD card first (it creates /media/fat/bennugd)."
    exit 1
fi
sh /media/fat/bennugd/install.sh
echo
echo "Now put your game under /media/fat/games/BennuGD/ and pick it from Other -> BennuGD."
