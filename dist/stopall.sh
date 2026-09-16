for p in $(ps | grep "[b]ennugd-launcherd" | awk '{print $1}'); do kill $p 2>/dev/null; done
for p in $(pidof bennugd); do kill -TERM $p 2>/dev/null; done
sleep 2
echo "stopped: daemons $(ps | grep -c '[b]ennugd-launcherd') frontends $(pidof bennugd | wc -w)"
