Nota: Esta documentación también está disponible en español: `README_es.md`

# 3DSpotify

Unofficial Spotify client for the Nintendo 3DS. The app connects directly to
Spotify over HTTPS.

<div style="display: flex; gap: 10px">
  <img src="https://i.imgur.com/BiwIS82.png" alt="Top playing" width="350">
  <img src="https://i.imgur.com/ppvuOb7.png" alt="Bottom playing" width="350">
</div>

## Overview
This repository contains:
- The native 3DS client, with its entry point in `source/main.c` and UI modules in [source/ui](source/ui), which pairs the account on-console, searches for tracks, and plays audio locally with album artwork.
- Independent protocol probes and build/test tooling in `source` and `tests`.

## Requirements
- Spotify **Premium** account.
- A Nintendo 3DS with Homebrew Launcher and Wi-Fi access.
- To build from source: devkitPro with devkitARM, libctru, citro2d, citro3d and 3dsxtool, plus Python 3.12 or newer.

## Building the native client

Install devkitPro with devkitARM, libctru, citro2d, citro3d and 3dsxtool, plus
Python 3.12 or newer. From the repository root:

```powershell
python source\build.py --fetch-deps
```

The default and only full-client target is `spotify3ds`. The pinned downloads
verify mbedTLS 3.6.7 and
`stb_vorbis`; the Vorbis header is generated from that same verified source.
Once dependencies have been downloaded, builds can omit `--fetch-deps`.

Run the build and integration tests from the repository root with:

```powershell
python -m unittest discover -s tests
```

The C tests for search encoding, queue boundaries, and cancellation run when a
host C compiler is available; otherwise unittest reports them as skipped.

The artifact is written to
`source\dist\3ds\spotify3ds\spotify3ds.3dsx`. Copy it to
`SD:/3ds/spotify3ds/` and launch it from the Homebrew Launcher, or send it
from the PC with:

```powershell
3dslink.exe source\dist\3ds\spotify3ds\spotify3ds.3dsx -a <CONSOLE_IP>
```
`3dslink` is part of devkitPro's tools.

To install it on the Home Menu instead, build the CIA as well (needs
`makerom` and `bannertool`, both part of devkitPro's tools):

```powershell
python source\build.py --cia
```

This writes `source\dist\3ds\spotify3ds\spotify3ds.cia`; copy it to the SD
card and install it with FBI or a similar CIA installer (requires custom
firmware). The CIA keeps its settings in `SD:/3ds/spotibacan/`.

The target starts a direct session: it refreshes the stored credential, offers
pairing when needed, and checks the access-point and library-session
connections. If no credential is stored, follow the pairing code shown on the
console from another device and authorize the account with Spotify. The refresh
token is saved to the SD card for later launches. On the home screen, tap
"Search songs" or press A to open the keyboard. Your last search appears
below it as a button that runs it again: tap it, or move to it with up/down
and press A. A tap acts when you lift the stylus inside the button; sliding
off cancels it. X reconnects, Y pairs another account, SELECT switches the
language, and START exits. Use up/down to move through results, A to select, B to return, and Y to enter
another query. B cancels a pending search after the current network request
finishes.

Press A on the selection screen to play the chosen track locally. During
playback, the top screen shows album artwork (or a neutral background when it
is unavailable) and the bottom screen shows track details and progress. A
pauses/resumes, left/right changes track, up/down adjusts local volume, B
returns to results, and START stops and exits. The queue follows search-result
order and does not wrap at either end. Complete playback, artwork, and
controls still need validation on real hardware. The probes remain available
as separate targets.

## Probes

The probes are small standalone apps, each testing one step of the client on
its own. They were used to build it up piece by piece and are still handy for
diagnosing problems on real hardware. Run them one after another in the order
below: each step builds on the previous ones (for example, the later probes use
the token that `probe_pair` stores), so the first one that fails points to the
problem.

| Target | What it checks |
|---|---|
| `probe_tls` | TLS 1.2 handshakes and an HTTPS request against Spotify's hosts, with timings. Writes `spotify-tls.log` beside the `.3dsx`. |
| `probe_pair` | Pairing the console through spotify.com/pair and storing the token on the SD card. |
| `probe_ap` | Logging in to Spotify's access point and keeping the session alive. |
| `probe_meta` | The full sign-in chain, then reading the library and a track's metadata. |
| `probe_audio` | Decoding Ogg Vorbis and playing it through NDSP (reads `test.ogg`; no network). |
| `probe_play` | One track end to end: audio key, download, decryption and playback. |

Build one with `--app`, for example:

```powershell
python source\build.py --app probe_tls --fetch-deps
```

The output goes to `source/dist/3ds/<target>/`; copy that folder to
`SD:/3ds/<target>/` and launch it from the Homebrew Launcher.

## License and credits
- License: Apache License 2.0 — see the `LICENSE` file at the repository root.
- Author: David Abejón.
- Based on [SpotiBacan](https://github.com/ohquebacan/spotibacan) by
  [ohquebacan](https://github.com/ohquebacan). The `source/platform3ds/`
  directory, the `source/probe_*/` directories and `source/build.py` come from
  that project and have since been modified for 3DSpotify. See `NOTICE`.
- Included libraries: stbi (stb_image) at `include/stb_image.h` and other dependencies pinned by the native build.