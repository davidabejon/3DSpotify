Nota: Esta documentación también está disponible en español: `README_es.md`

# Spotify 3DS

Unofficial Spotify client for the Nintendo 3DS. The app connects directly to
Spotify over HTTPS.

<div style="display: flex; gap: 10px">
  <img src="https://i.imgur.com/BiwIS82.png" alt="Top playing" width="350">
  <img src="https://i.imgur.com/ppvuOb7.png" alt="Bottom playing" width="350">
</div>

## Overview
This repository contains:
- The native 3DS client in [client/source/app](client/source/app), which pairs the account on-console, searches for tracks, and plays audio locally with album artwork.
- Independent protocol probes and build/test tooling in `client/source` and `client/tests`.

## Requirements
- A Nintendo 3DS with Homebrew Launcher and Wi-Fi access.
- To build from source: devkitPro with devkitARM, libctru, citro2d, citro3d and 3dsxtool, plus Python 3.12 or newer.
- The native client does not require the proxy to be configured or running.

## Building the native client

Install devkitPro with devkitARM, libctru, citro2d, citro3d and 3dsxtool, plus
Python 3.12 or newer. From the repository root:

```powershell
python client\source\build.py --fetch-deps
```

The default and only full-client target is `spotify3ds`. The pinned downloads
verify mbedTLS 3.6.7 and
`stb_vorbis`; the Vorbis header is generated from that same verified source.
Once dependencies have been downloaded, builds can omit `--fetch-deps`.

Run the build and integration tests from the repository root with:

```powershell
python -m unittest discover -s client\tests
```

The C tests for search encoding, queue boundaries, and cancellation run when a
host C compiler is available; otherwise unittest reports them as skipped.

The artifact is written to
`client\source\dist\3ds\spotify3ds\spotify3ds.3dsx`. Copy it to
`SD:/3ds/spotify3ds/` and launch it from the Homebrew Launcher, or send it
from the PC with:

```powershell
C:\devkitPro\tools\bin\3dslink.exe client\source\dist\3ds\spotify3ds\spotify3ds.3dsx -a <CONSOLE_IP>
```

The target starts a direct session: it refreshes the stored credential, offers
pairing when needed, and checks the access-point and library-session
connections. If no credential is stored, follow the pairing code shown on the
console from another device and authorize the account with Spotify. The refresh
token is saved to the SD card for later launches. On the session screen, A
opens search, X reconnects, Y pairs another account, and START exits. Use
up/down to move through results, A to select, B to return, and Y to enter
another query. B cancels a pending search after the current network request
finishes.

Press A on the selection screen to play the chosen track locally. During
playback, the top screen shows album artwork (or a neutral background when it
is unavailable) and the bottom screen shows track details and progress. A
pauses/resumes, left/right changes track, up/down adjusts local volume, B
returns to results, and START stops and exits. The queue follows search-result
order and does not wrap at either end. No remote-control proxy is involved.
Complete playback, artwork, and controls still need validation on real
hardware. The probes remain available as separate targets. The old proxy
client and its server source have been removed.

## Building the native TLS probe

Install devkitPro with devkitARM, libctru, citro2d, citro3d and 3dsxtool, plus
Python 3.12 or newer. No Melee toolchain, Clang, or CMake is needed.
From the repository root:

```powershell
python client\source\build.py --app probe_tls --fetch-deps
```

The first build downloads mbedTLS 3.6.7 and verifies its SHA-256 checksum.
Later builds can omit `--fetch-deps` and run offline. The library is compiled
with the same configuration as the selected app.
Use `--devkitpro "C:\devkitPro"` to select the SDK explicitly; otherwise the
script checks `DEVKITPRO`, then the standard installation location.
`--debug` selects an unoptimized build.

Copy `client/source/dist/3ds/probe_tls/` to `SD:/3ds/probe_tls/` and launch
`probe_tls.3dsx` from the Homebrew Launcher. Intermediate objects and the ELF
are in `client/source/build/probe_tls/`. Other app targets still require their
corresponding source files and any stb dependencies to be present.

The TLS probe enables socket metadata tracing in `spotify-tls.log` beside the
executable (fallback: `sdmc:/spotify-tls.log`). Entries prefixed `[socket]`
record the selected IPv4 address, syscall results, immediate failure `errno`,
raw SOC `SO_ERROR`, poll event masks, requested byte counts and operation times.
They also mark handshakes and whether a cached session was offered. No payloads,
keys or credentials are logged. `errno=0` means the operation did not fail;
`recv rc=0` means EOF. On libctru, `POLLOUT=0x10` and `POLLHUP=0x04`.
TCP connection polls use intervals of at most 100 ms within the shared DNS/TCP
deadline. After each interval, even without a poll event, the peer is checked.
`op=tcp_confirmed poll_rc=0` means the peer was confirmed without a readiness
notification; `total_ms` measures time since TCP setup began.
Tracing adds overhead and is opt-in through `tls_set_trace()`; other apps do not
enable it by default. Copy the full log after a run, before launching the probe
again: each run replaces it. Verbose events stay off the console screen.

## License and credits
- License: Apache License 2.0 — see the `LICENSE` file at the repository root.
- Original author: David Abejón.
- Included libraries: stbi (stb_image) at `client/include/stb_image.h` and other dependencies pinned by the native build.