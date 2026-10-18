# Running the lobby server on a free cloud VM

The lobby server (`sms_lobby_server.py`, one file, Python 3 standard library only) lets
players join each other **by lobby name**: it keeps the list of open lobbies, introduces
players to the host so they connect directly (UDP hole punching), and relays the game
traffic when a direct connection is impossible. It stores nothing on disk and logs no
player addresses (only short hashed tags) unless started with `--log-addresses`.

**You** create the cloud account and the VM; nothing here creates accounts, enters
credentials or goes public on its own. Until you run it and put its address in the game,
"Server" mode works on your home network only (players on the same LAN find the lobby by
name without a server).

What you need: a free-tier Linux VM with a public IP, one open **UDP** port (default
**7700**), and these four files from `port/server/`: `sms_lobby_server.py`, `install.sh`,
`sms-lobby.service`, `DEPLOY.md`.

---

## Option A: Oracle Cloud "Always Free" (recommended)

Always Free includes small AMD (`VM.Standard.E2.1.Micro`) and Arm (`VM.Standard.A1.Flex`)
VMs and 10 TB/month of outbound traffic, plenty for relaying.

1. Sign up at cloud.oracle.com (a card is needed for verification; Always Free resources
   are not charged). Pick a home region close to your players.
2. **Create the VM:** Menu > Compute > Instances > Create instance.
   - Image: **Canonical Ubuntu 22.04 or 24.04** (minimal is fine).
   - Shape: `VM.Standard.E2.1.Micro` (Always Free) or `VM.Standard.A1.Flex` with 1 OCPU / 1 GB.
   - Networking: keep "Assign a public IPv4 address" checked.
   - SSH keys: upload your public key or let it generate one (download the private key).
   - Create, then note the **Public IP address** on the instance page.
3. **Open UDP 7700 in the VCN security list:** on the instance page click the subnet >
   Security Lists > the default list > **Add Ingress Rules**:
   - Source CIDR `0.0.0.0/0`, IP Protocol **UDP**, Destination Port Range **7700**.
   - (Leave the SSH rule for TCP 22 as it is.)
4. **Copy the files and install** (from the folder containing `port/server`, on your PC):
   ```
   scp -i <your-key> -r port/server ubuntu@<PUBLIC_IP>:~/sms-server
   ssh -i <your-key> ubuntu@<PUBLIC_IP>
   cd ~/sms-server && sudo sh install.sh
   ```
   `install.sh` also opens the port in Ubuntu's own iptables rules (Oracle's images reject
   everything except SSH by default) and makes that survive reboots.
5. Check: `systemctl status sms-lobby` shows "active (running)", and
   `journalctl -u sms-lobby -f` shows `listening on UDP 0.0.0.0:7700`.

## Option B: Google Cloud free tier (e2-micro)

One `e2-micro` VM is free in `us-west1`, `us-central1` or `us-east1`, with a standard
persistent disk up to 30 GB. **Outbound traffic is only 1 GB/month free**: relayed games
use roughly 20-70 MB per hour per relayed player, so on Google either run with
`NO_RELAY=1` (rendezvous only; most home routers still connect directly) or watch billing.

1. console.cloud.google.com > create a project > Compute Engine > VM instances > Create.
   - Region one of the three above, machine type **e2-micro**, boot disk **Ubuntu 22.04/24.04**
     (standard persistent disk, 10-30 GB).
   - Networking > Network tags: add `sms-lobby`. External IPv4: ephemeral (or reserve a
     static one; a static IP attached to a running VM is free).
2. **Firewall rule:** VPC network > Firewall > Create firewall rule:
   name `sms-lobby`, direction Ingress, targets "Specified target tags" `sms-lobby`,
   source IPv4 ranges `0.0.0.0/0`, protocols and ports: **UDP 7700**.
   (CLI: `gcloud compute firewall-rules create sms-lobby --allow udp:7700 --target-tags sms-lobby`.)
3. Copy and install (the "SSH" button opens a browser shell; upload the four files with its
   "Upload file" menu, or use `gcloud compute scp --recurse port/server <vm>:~/sms-server`):
   ```
   cd ~/sms-server && sudo NO_RELAY=1 sh install.sh      # or without NO_RELAY=1
   ```

## Point the game at it

On every player's PC, in `port_multiplayer.ini` (next to `port_settings.ini`):
```
server = <PUBLIC_IP or DNS name>:7700
```
or in game: Multiplayer menu > "Lobby server address" > type it > Apply. The menu then shows
**Lobby server: online**. Create a lobby with connection type "Server (lobby name)"; friends
pick "Server (lobby name)" on the Join page and type the same name.

## Operating it

- Logs: `journalctl -u sms-lobby -f` (lobby opened/closed, relay sessions, stats every 5 min).
- Restart / stop: `sudo systemctl restart sms-lobby` / `sudo systemctl stop sms-lobby`.
- Options (edit `ExecStart` in `/etc/systemd/system/sms-lobby.service`, then
  `sudo systemctl daemon-reload && sudo systemctl restart sms-lobby`):
  `--port N`, `--no-relay`, `--max-lobbies 500`, `--max-per-ip 3` (lobbies per address),
  `--max-relays 200`, `--relay-kbps 256` (per session and direction), `--rate 20`
  (control packets per second per address), `--log-addresses` (debugging only).
- Update: copy the new `sms_lobby_server.py` and run `sudo sh install.sh` again.
- Uninstall: `sudo systemctl disable --now sms-lobby && sudo rm -rf /opt/sms-lobby /etc/systemd/system/sms-lobby.service`,
  then delete the VM in the cloud console.

## What the server does and does not do

- Keeps open lobbies in memory: name (unique, case-insensitive), host endpoint, LAN
  endpoints the host reported, player count, whether a password is set (only a hash of it).
  A lobby disappears 30 s after its host stops sending heartbeats (unless a player of the lobby takes it over as the new host), or at once when the host
  leaves. Nothing is written to disk; a restart forgets everything (hosts re-register).
- Answers name lookups, tells both sides each other's address so they connect directly,
  and relays a session only when asked (sessions end after 30 s without traffic).
- Limits: datagrams over 1450 bytes are dropped; 20 control packets/s per address; 3
  lobbies per address; 500 lobbies; 200 relay sessions; 256 KiB/s per relay direction.
- Relayed game traffic is forwarded as is and never stored or logged; there are no accounts.

## Test it locally first

`python port/tools/mp_server_test.py` runs the server on 127.0.0.1 with several game
instances and checks unique names, joining by name, the relay, expiry and the LAN fallback.
