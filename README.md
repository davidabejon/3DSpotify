Nota: Esta documentación también está disponible en español: `README_es.md`

# Spotify 3DS

Unofficial Spotify client for the Nintendo 3DS (proxy + 3DS application).

<div style="display: flex; gap: 10px">
  <img src="https://i.imgur.com/BiwIS82.png" alt="Top playing" width="350">
  <img src="https://i.imgur.com/ppvuOb7.png" alt="Bottom playing" width="350">
</div>

## Overview
This repository contains:
- A proxy server (FastAPI) that handles Spotify authentication and exposes endpoints for the console: [server/server.py](server/server.py). See also [server/Dockerfile](server/Dockerfile) and [server/spotify_config.json](server/spotify_config.json).
- The 3DS client application (source and assets) in the `client` folder: [client/Makefile](client/Makefile), [client/source/main.c](client/source/main.c) and the display interface [client/include/image_display.h](client/include/image_display.h) (e.g. `initNetwork`).

## Requirements
- Docker or Python (depending on whether you choose step 1.a or 1.b)
- To build the client from source: devkitARM/3DS homebrew toolchain (see `client/Makefile`)
- A Spotify Developer application (Client ID / Client Secret)

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

The artifact is written to
`client\source\dist\3ds\spotify3ds\spotify3ds.3dsx`. Copy it to
`SD:/3ds/spotify3ds/` and launch it from the Homebrew Launcher, or send it
from the PC with:

```powershell
C:\devkitPro\tools\bin\3dslink.exe client\source\dist\3ds\spotify3ds\spotify3ds.3dsx -a <CONSOLE_IP>
```

The target starts a direct session: it refreshes the stored credential, offers
pairing when needed, and checks the access-point and library-session
connections. On the session screen, A opens search, X reconnects, Y pairs
another account, and START exits. Use up/down to move through results, A to
select, B to return, and Y to enter another query. Search and metadata
requests run while progress is displayed. Selecting a track prepares the
queue but does not play audio yet. The probes remain available as separate
targets. It does not compile `fetch.c` or `parse.c` from the legacy flow.

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

## Quick start

### 1) Deploy the proxy server
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

### 2) 3DS Application
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