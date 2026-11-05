Nota: Esta documentación también está disponible en español: `README_es.md`

# Spotify 3DS

Unofficial Spotify client for the Nintendo 3DS. The native client connects
directly to Spotify over HTTPS; the older proxy mode is retained as historical
reference.

<div style="display: flex; gap: 10px">
  <img src="https://i.imgur.com/BiwIS82.png" alt="Top playing" width="350">
  <img src="https://i.imgur.com/ppvuOb7.png" alt="Bottom playing" width="350">
</div>

## Overview
This repository contains:
- The native 3DS client in [client/source/native](client/source/native), which pairs the account on-console, searches for tracks, and plays audio locally with album artwork.
- The legacy FastAPI proxy and proxy-mode client, retained for reference: [server/server.py](server/server.py) and [client/Makefile](client/Makefile).

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

The default target is `spotify3ds`, separate from the legacy proxy application
built with `client\Makefile`. The pinned downloads verify mbedTLS 3.6.7 and
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
hardware. The probes remain available as separate targets. The native target
does not compile `fetch.c` or `parse.c` from the legacy flow.

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

## Legacy proxy client (historical)

The following instructions describe the older proxy-based client. They are
not required to build or use `spotify3ds`; its server, configuration, and
credentials are separate from the native client.

### Deploy the proxy server
You can deploy the server with Docker (recommended) or run it directly with Python.

### 1.a) Proxy server with Docker
1. Download the release `spotify3ds-proxy.tar` from the project's Releases.
2. Load the Docker image:
   ```sh
   docker load -i spotify3ds-proxy.tar
   ```
3. Run the container:
   ```sh
   docker run -d --name spotify3ds-proxy -p 8000:8000 spotify3ds-proxy
   ```
4. Check container status:
   ```sh
   docker ps
   ```
5. Enter the container to edit configuration:
   ```sh
   docker exec -it spotify3ds-proxy bash
   apt update && apt install nano -y
   nano spotify_config.json
   ```
   Fill `client_id` and `client_secret` (from the Spotify dashboard) and save.
6. On the first run the server will try to open the authorization URL from the container host; if it doesn't open you will see the URL in the container or host console. Open it in a browser and authorize. After authorization the server will store the `code`/`refresh_token` in `spotify_config.json`.

### 1.b) Proxy server directly with Python

1. Clone the repo and enter the server folder:
   ```sh
   git clone https://github.com/davidabejon/spotify-3ds.git
   cd spotify-3ds/server
   ```

2. Create and activate a virtual environment (Windows):
   ```ps
   python -m venv .venv
   .venv\Scripts\activate
   ```

3. Install dependencies:
   ```ps
   pip install -r requirements.txt
   ```
   (If there is no requirements.txt, install FastAPI and Uvicorn: `pip install fastapi uvicorn requests`.)

4. Edit `spotify_config.json` and add `client_id` and `client_secret` from the Spotify Dashboard. Optionally adjust `redirect_uri`.

5. Run the server with Uvicorn (from the `server` folder):
   ```ps
   uvicorn server:app --host 0.0.0.0 --port 8000 --reload --reload-include spotify_config.json
   ```
   - If the main module name differs adjust `module:app` accordingly.
   - On first run the server will show or open the authorization URL; complete the flow in the browser so the `refresh_token` is saved to `spotify_config.json`.

### Legacy proxy-mode application
- Easiest: download `Spotify3DS.cia` from [Releases](https://github.com/davidabejon/spotify-3ds/releases), copy to the SD card and install with FBI or another CIA installer
- Alternatively download `spotify-3ds.zip` package from [Releases](https://github.com/davidabejon/spotify-3ds/releases): extract into `/3ds` on the SD card and run via the Homebrew Launcher.

On first run the app will prompt you to enter the proxy server's IP address; after submitting it will attempt to connect automatically.

To change the IP later, press Y to reopen the on-screen keyboard and enter a new address.

#### Controls
 - A — Play / Pause
 - Y — Open IP entry (on-screen keyboard)
 - D-Pad Right — Next track
 - D-Pad Left — Previous track
 - D-Pad Up — Volume up
 - D-Pad Down — Volume down
 - START — Exit the app

## License and credits
- License: Apache License 2.0 — see the `LICENSE` file at the repository root.
- Original author: David Abejón.
- Included libraries: stbi (stb_image) at `client/include/stb_image.h` and other dependencies listed in Docker/requirements.