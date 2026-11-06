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
token is saved to the SD card for later launches.

### Home screen

On the home screen, tap **"Search songs"** or press **A** to open the keyboard. Your last search
appears below as a secondary button; tap it or move to it with up/down and press **A**.

**Touch controls:**
- Tap a button to highlight it; the button shows a pressed state when you lift the stylus inside it.
- Sliding the stylus off a button before lifting cancels the tap.

**Button hints:** Glyphs with colored backgrounds show available actions:
- <span style="background-color: #FF4F45; color: white; padding: 2px 4px; border-radius: 2px;">A</span> — opens the search keyboard
- <span style="background-color: #3FA2FF; color: white; padding: 2px 4px; border-radius: 2px;">X</span> — reconnects to Spotify
- <span style="background-color: #39D97A; color: black; padding: 2px 4px; border-radius: 2px;">Y</span> — pairs another account
- **SELECT** — switches the language
- **START** — exits

### Search results

Results show four songs at a time. The **top screen** displays the song card: title, artist, album,
and duration of the highlighted song. The **bottom screen** shows the list with thumbnails.

**Navigation:**
- Use up/down to move through songs; the highlighted one gains a green border (`lima`) and a
  brighter background.
- Press L/R to jump four songs at a time.
- Tap a song to highlight it; tap it again (or press **A**) to play it directly.
- Drag the list with the stylus to scroll; the bar on the right edge shows your position.

**Button hints:**
- <span style="background-color: #FF4F45; color: white; padding: 2px 4px; border-radius: 2px;">A</span> — plays the highlighted song
- <span style="background-color: #FFD23F; color: black; padding: 2px 4px; border-radius: 2px;">B</span> — returns to the home screen
- <span style="background-color: #39D97A; color: black; padding: 2px 4px; border-radius: 2px;">Y</span> — enters a new search
- L/R — jump four songs at a time

**Touch gestures:**
- **Tap a row:** selects the song; the row highlights and its card appears on the top screen.
- **Tap again:** plays immediately with the remaining results as the queue.
- **Drag vertically:** scrolls the list; rows can be cut off by the screen edges. The highlighted
  song and its card remain visible on the top screen while you scroll.
- **Drag beyond 8 pixels from the starting point:** commits to scrolling; the row tap is cancelled.

**Search status:**
- If the search fails, press **A** (or tap the on-screen button) to retry.
- Press **B** to cancel a pending search after the current network request finishes.

### Playback

The chosen track plays locally with the rest of the results as the queue. During playback:
- The **top screen** shows the album artwork (or a neutral background if unavailable) with the title,
  artist, and album on the right.
- The **bottom screen** shows track number, progress bar, and volume control.

**Playback controls:**
- <span style="background-color: #FF4F45; color: white; padding: 2px 4px; border-radius: 2px;">A</span> — pauses/resumes
- Left/right — changes track
- Up/down — adjusts local volume
- <span style="background-color: #FFD23F; color: black; padding: 2px 4px; border-radius: 2px;">B</span> — returns to results
- **START** — stops and exits

The queue follows search-result order and does not wrap at either end. Complete playback, artwork,
and controls still need validation on real hardware. The probes remain available as separate targets.

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