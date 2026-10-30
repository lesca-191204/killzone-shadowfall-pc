# Private multiplayer (beta, testing)

Multiplayer between friends without the official servers: one PC hosts a room and the others
join it over a virtual LAN (Radmin VPN, Tailscale, ZeroTier or a home network).

Based on the **KZSF private multiplayer prototype** by **Marakesch** (GPL-2.0-or-later, built on this
port's v0.6.0 and shared with us on 2026-10-30). From it: the native network/PSN adapters
(`src/runtime_network.c`, `runtime_http.c`, `runtime_rudp.c`, `runtime_np_webapi.c`, the
identity part of `runtime_services.c`), the room server, bridge and profile services
(`server/`, `connection_bundle.py`, `file_lock.py`, `private_session.py`), the free map pack
mounting (`scripts/dlc_cache.py`) and the HUD Visibility / Full Unlocks patches.
Changed here: any virtual LAN instead of Tailscale only (`server/room_host.py`: Radmin VPN found
by itself, `--host-ip` for others, private ranges only), English messages, our own small
host/join script and launches through the port's launcher. The Insert/L2+R3 menu change was
not taken.

```text
game -> local bridge (127.0.0.1:17800-17802) -> TLS on the host's virtual-LAN IP, TCP 17803
     -> room gateway -> profile service + match relay (Turbine messages forwarded unchanged)
```

| Script | Use |
|---|---|
| `local_test.py` | ONE PC: the services on loopback and two copies of the game (Player1, Player2) |
| `kz_online.py host` | Host a room on this PC's virtual-LAN address, start the game |
| `kz_online.py join` | Join a room with the host's `Host-connection.zip` and the room code |

Everything is off unless these scripts start the game (`KZSF_PRIVATE_HTTP` and the other
`KZSF_*` variables); the campaign is unchanged. Tests: `python -B -m unittest discover -s server`.

Never share `room-host/`: it holds the host's private key. `Host-connection.zip` is public.
