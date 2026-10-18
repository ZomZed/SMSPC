#!/bin/sh
# One-command install of the Super Mario Sunshine PC port lobby server on a Linux VM
# (Ubuntu / Debian / Oracle Linux). Run from this folder:   sudo sh install.sh
# Optional: PORT=7700 (UDP port), NO_RELAY=1 (rendezvous only, saves bandwidth).
# What it does: installs python3 if missing, copies the server to /opt/sms-lobby, creates
# the unprivileged user "smslobby", installs and starts the systemd service "sms-lobby", and
# opens the UDP port in the VM's own firewall (ufw / firewalld / iptables). The cloud
# provider's firewall (Oracle security list, Google VPC rule) is separate: see DEPLOY.md.
set -e
PORT="${PORT:-7700}"
HERE="$(cd "$(dirname "$0")" && pwd)"

if [ "$(id -u)" != "0" ]; then
	echo "Run with sudo:  sudo sh install.sh" >&2
	exit 1
fi
if ! command -v python3 >/dev/null 2>&1; then
	echo "Installing python3 ..."
	if command -v apt-get >/dev/null 2>&1; then apt-get update -q && apt-get install -y -q python3
	elif command -v dnf >/dev/null 2>&1; then dnf install -y python3
	elif command -v yum >/dev/null 2>&1; then yum install -y python3
	else echo "Please install python3 and run again." >&2; exit 1; fi
fi

id smslobby >/dev/null 2>&1 || useradd --system --no-create-home --shell /usr/sbin/nologin smslobby 2>/dev/null \
	|| useradd -r -M -s /sbin/nologin smslobby
mkdir -p /opt/sms-lobby
cp "$HERE/sms_lobby_server.py" /opt/sms-lobby/
chmod 755 /opt/sms-lobby /opt/sms-lobby/sms_lobby_server.py

OPTS="--port $PORT"
[ -n "$NO_RELAY" ] && OPTS="$OPTS --no-relay"
sed "s|^ExecStart=.*|ExecStart=$(command -v python3) -u /opt/sms-lobby/sms_lobby_server.py $OPTS|" \
	"$HERE/sms-lobby.service" > /etc/systemd/system/sms-lobby.service

# the VM's own firewall
if command -v ufw >/dev/null 2>&1 && ufw status 2>/dev/null | grep -q "Status: active"; then
	ufw allow "$PORT/udp"
elif command -v firewall-cmd >/dev/null 2>&1 && firewall-cmd --state >/dev/null 2>&1; then
	firewall-cmd --permanent --add-port="$PORT/udp" && firewall-cmd --reload
elif command -v iptables >/dev/null 2>&1; then
	# Oracle's Ubuntu images ship iptables rules that reject everything but SSH
	iptables -C INPUT -p udp --dport "$PORT" -j ACCEPT 2>/dev/null || iptables -I INPUT 1 -p udp --dport "$PORT" -j ACCEPT
	if command -v netfilter-persistent >/dev/null 2>&1; then netfilter-persistent save
	elif [ -d /etc/iptables ]; then iptables-save > /etc/iptables/rules.v4; fi
fi

systemctl daemon-reload
systemctl enable --now sms-lobby
sleep 1
systemctl --no-pager --lines=5 status sms-lobby || true
echo
echo "Lobby server installed on UDP port $PORT."
echo "Players set in port_multiplayer.ini:   server = <this VM's public IP or DNS name>:$PORT"
echo "Logs: journalctl -u sms-lobby -f      Stop: sudo systemctl stop sms-lobby"
echo "Uninstall: sudo systemctl disable --now sms-lobby && sudo rm -rf /opt/sms-lobby /etc/systemd/system/sms-lobby.service"
