cd /media/fat/bennugd
for p in $(ps | grep "[b]ennugd-launcherd" | awk '{print $1}'); do kill $p 2>/dev/null; done
sleep 1
[ -f bennugd-launcherd.sh.new ] && mv bennugd-launcherd.sh.new bennugd-launcherd.sh
chmod +x bennugd-launcherd.sh
setsid ./bennugd-launcherd.sh >/dev/null 2>&1 < /dev/null &
sleep 3
tail -2 launcherd.log
