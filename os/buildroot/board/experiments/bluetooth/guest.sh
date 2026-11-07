#!/bin/sh
# Runs as root on the booted image (measure.py): starts a virtual controller and the variant's
# Bluetooth daemons, then reports their memory, threads and CPU. $1 is the variant.
variant=$1
here=$(dirname "$0")
echo "== uptime at login (s): $(cut -d' ' -f1 /proc/uptime)"
mem() { grep -E '^(MemTotal|MemFree|MemAvailable):' /proc/meminfo | tr -s ' ' | tr '\n' ' '; echo; }
echo "== meminfo before: $(mem)"

if [ "$variant" != baseline ]; then
	# BlueZ's emulator: one BR/EDR + LE controller through /dev/vhci.
	"$here/btvirt" -l1 > /tmp/btvirt.log 2>&1 &
	sleep 5
	echo "== controllers: $(ls /sys/class/bluetooth)"
	case $variant in
	bluealsa)
		profiles="-p a2dp-source -p a2dp-sink"
		bluealsa --help 2>&1 | grep -q midi && profiles="$profiles -p midi"
		echo "== running: bluealsa $profiles"
		bluealsa $profiles > /tmp/bluealsa.log 2>&1 &
		;;
	pipewire)
		mkdir -p /etc/wireplumber/wireplumber.conf.d /run/pipewire
		cp "$here/bluetooth-only.conf" /etc/wireplumber/wireplumber.conf.d/
		export XDG_RUNTIME_DIR=/run/pipewire
		echo "== running: pipewire; wireplumber (with bluetooth-only.conf)"
		pipewire > /tmp/pipewire.log 2>&1 &
		sleep 3
		wireplumber > /tmp/wireplumber.log 2>&1 &
		;;
	esac
	sleep 30
	echo "== adapter (org.bluez GetManagedObjects, hci0 UUIDs and Powered):"
	dbus-send --system --print-reply --dest=org.bluez / org.freedesktop.DBus.ObjectManager.GetManagedObjects \
		| awk '/object path/ {print} /string "(Address|Powered)"/ {k = $0; getline; print k $0} /string "[0-9a-f]+-[0-9a-f]+-/ {print}' | tr -s ' ' | head -40
	[ "$variant" = pipewire ] && command -v pw-cli > /dev/null && { echo "== pw-cli ls, Bluetooth objects:"; pw-cli ls 2>&1 | grep -i -E 'bluez|midi'; }
	for f in /tmp/btvirt.log /tmp/bluealsa.log /tmp/pipewire.log /tmp/wireplumber.log; do
		[ -s "$f" ] && { echo "== $f (last 15 lines):"; tail -15 "$f"; }
	done
	echo "== bluetoothd in /var/log/messages:"; grep bluetoothd /var/log/messages | cut -d' ' -f5- | tail -25
fi

pids=$(for n in bluetoothd bluealsa pipewire wireplumber dbus-daemon btvirt; do pidof $n; done)
echo "== processes:"
for p in $pids; do
	pss=$(awk '/^Pss:/ {print $2}' /proc/$p/smaps_rollup 2>/dev/null)
	echo "pid $p $(cat /proc/$p/comm): $(grep VmRSS /proc/$p/status | tr -s ' \t' ' ') | Pss: $pss kB | cmdline: $(tr '\0' ' ' < /proc/$p/cmdline)"
	# fields after the command name: 17 = nice (19), 38 = rt_priority (40), 39 = policy (41)
	for t in /proc/$p/task/*; do
		sed 's/^.*) //' $t/stat | awk -v tid="${t##*/}" -v name="$(cat $t/comm)" \
			'{split("OTHER FIFO RR BATCH ISO IDLE DEADLINE", pol, " "); printf "    thread %s %-16s policy %s rtprio %s nice %s\n", tid, name, pol[$39+1], $38, $17}'
	done
done

# per process, all threads: utime + stime in ticks (USER_HZ 100; the kernel has no schedstats)
cputime() { for p in $pids; do echo "$p $(sed 's/^.*) //' /proc/$p/stat | awk '{print $12 + $13}')"; done; }
before=$(cputime); cpu0=$(head -1 /proc/stat)
sleep 60
after=$(cputime); cpu1=$(head -1 /proc/stat)
echo "== CPU over 60 s (percent of one core; 1 tick = 0.017 %):"
echo "$before" | while read p t0; do
	echo "$after" | awk -v p=$p -v t0=$t0 -v name="$(cat /proc/$p/comm)" \
		'$1 == p {printf "pid %s %s: %d ticks = %.3f %%\n", p, name, $2 - t0, ($2 - t0) / 60}'
done
echo "$cpu0
$cpu1" | awk '{t = 0; for (i = 2; i <= NF; i++) t += $i; tot[NR] = t; idle[NR] = $5 + $6}
	END {printf "whole system: %.1f %% busy of all cores\n", 100 * (1 - (idle[2] - idle[1]) / (tot[2] - tot[1]))}'
echo "== meminfo after: $(mem)"
