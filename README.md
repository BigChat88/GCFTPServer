# GC FTP Server

A minimal FTP server for the Nintendo GameCube, shipped as a single `.dol` and built with [libogc2](https://github.com/extremscorner/libogc2).

## Hardware

### Network

The first adapter found is used, in this order:

| Adapter | Ports probed | Status |
|---|---|---|
| Broadband Adapter (DOL-015) and clones | Serial Port 1 | Tested |
| ETH2GC with W6100 / W6300 | SP1, SP2, Slot A, Slot B | Untested |
| ETH2GC with W5500 | SP1, SP2, Slot A, Slot B | Untested |
| ETH2GC with ENC28J60 | SP1, SP2, Slot A, Slot B | Untested |

The adapter in use is shown on screen.

### Storage

| Device | FTP folder |
|---|---|
| SD2SP2 (Serial Port 2) | `/sd` |
| SD Gecko in Slot A | `/carda` |
| SD Gecko in Slot B | `/cardb` |
| GC Loader | `/gcloader` |

Each mounted device appears as a folder in the FTP root.

A network adapter and an SD adapter cannot share the same port. For example, an ETH2GC in SP2 rules out an SD2SP2.

## Usage

1. Copy `gcftpserver.dol` to your SD card and boot it from Swiss (or your loader of choice).
2. The screen shows the address obtained via DHCP, e.g. `ftp://192.168.1.50:21`.
3. Connect with FileZilla, WinSCP or `ftp` in **passive mode**. The user is `cube` and any password is accepted.
4. Press **START** to exit.

### Optional config

Copy [gcftp.ini.example](gcftp.ini.example) as `gcftp.ini` to the root of any mounted device. It lets you set the user, password, port or a static IP.

### Firewall

- Control port: 21 (configurable)
- Passive ports: 50000–50999

## Performance

Measured with the official Broadband Adapter at 100 Mbit/s, an SD2SP2, and FileZilla on a PC over WiFi 6:

| Direction | Speed |
|---|---|
| Download (GameCube → PC) | ~2.2 MB/s |
| Upload (PC → GameCube) | ~950 KB/s |

Downloads are close to the limit of the EXI bus. Uploads are limited by the BBA's tiny receive buffer (see below), which makes them sensitive to network latency. A wired PC should upload faster than WiFi.

## Download

Every push builds the DOL on GitHub Actions. The result can be downloaded from the run's **Artifacts** section (requires a GitHub login).

Pushing a tag such as `v1.0.0` also publishes a **Release** with `gcftpserver.dol` and a zip containing the DOL, the README and the example config:

```sh
git tag v1.0.0
git push origin v1.0.0
```

## Building

**With Docker** (no devkitPro install needed):

```powershell
.\build.ps1
```

**With devkitPro:** install libogc2 from its [pacman repository](https://github.com/extremscorner/libogc2#installing) and run `make`. It must be libogc2 commit `f8f62e1` (see below).

### Network stack

[netstack/](netstack/) holds a copy of libogc2's network stack (lwIP plus the BBA and ETH2GC drivers), taken from libogc2 commit `f8f62e1`. It is built as `libgcnet.a` and linked in place of `-lbba`. Changes from upstream:

- **BBA interrupts on every received frame** instead of every 2 ([gcif.c](netstack/lwip/arch/gc/netif/gcif.c)). Before, a single frame in flight waited for the BBA's interrupt timer. This gave downloads a ~2.5x speedup.
- **The receive window is reopened as soon as data is queued in the stack**, instead of when the application reads it (up to 16 frames queued per connection). See `EARLY_RECVED_LIMIT` in [network.c](netstack/lwip/network.c).

The TCP receive window stays at 2 segments, as upstream: the BBA's receive ring holds only ~3.8KB, so a larger window causes dropped frames and retransmission stalls.

The stack uses libogc2 internal headers, so it must match the libogc2 it is built against. `build.ps1` and the GitHub workflow both pin the Docker image (by digest) that has exactly that commit. To update libogc2, re-sync `netstack/` from the new commit and update the digest in both places.

## Supported FTP commands

`USER PASS QUIT SYST NOOP FEAT OPTS AUTH(rejected) PWD CWD CDUP TYPE MODE STRU PASV EPSV PORT LIST NLST RETR STOR APPE REST DELE MKD RMD SIZE MDTM RNFR RNTO ALLO ABOR STAT HELP`

## Limitations

- Up to 3 simultaneous clients. Setting FileZilla to 1–2 connections per server gives the smoothest results.
- No TLS: use it on your local network only.
- `ABOR` does not interrupt a transfer in progress.
- The network can only be initialized once per boot. If it fails, restart the DOL.
