Nota: Esta documentación también está disponible en español: `README_es.md`

# 3DSpotify

Unofficial Spotify client for the Nintendo 3DS. The app connects directly to
Spotify over HTTPS.

**Website:** [davidabejon.github.io/3DSpotify](https://davidabejon.github.io/3DSpotify/)
(download, FBI QR code, features and controls). Its source lives in
[site/](site/) and is published to GitHub Pages by
[.github/workflows/pages.yml](.github/workflows/pages.yml) on every push to
`main` that changes it.

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

Audio preparation exchanges OAuth for reusable AP credentials, then
authenticates a new AP connection before requesting the audio key, following
librespot's login flow. These AP credentials stay in memory and are never logged
or saved. A key refusal displays the server code; it does not by itself prove
that the account lacks Premium.

## Building the native client

Install devkitPro with devkitARM, libctru, citro2d, citro3d and 3dsxtool, plus
Python 3.12 or newer. From the repository root:

```powershell
python source\build.py --app 3dspotify --fetch-deps --debug
```

The default and only full-client target is `3dspotify`. Omit `--debug` for an
optimized release build. The pinned downloads
verify mbedTLS 3.6.7, `stb_vorbis` and `qrcodegen` 1.8.0 (the pairing QR code);
the Vorbis header is generated from that same verified source.
Once dependencies have been downloaded, builds can omit `--fetch-deps`.

Run the build and integration tests from the repository root with:

```powershell
python -m unittest discover -s tests
```

The C tests for search encoding, queue boundaries, and cancellation run when a
host C compiler is available; otherwise unittest reports them as skipped.

The artifact is written to
`source\dist\3ds\3DSpotify\3DSpotify.3dsx`. Copy it to
`SD:/3ds/3DSpotify/` and launch it from the Homebrew Launcher, or send it
from the PC with:

```powershell
3dslink.exe source\dist\3ds\3DSpotify\3DSpotify.3dsx -a <CONSOLE_IP>
```
`3dslink` is part of devkitPro's tools.

To install it on the Home Menu instead, build the CIA as well (needs
`makerom` and `bannertool`, both part of devkitPro's tools):

```powershell
python source\build.py --cia
```

This writes `source\dist\3ds\3DSpotify\3DSpotify.cia`; copy it to the SD
card and install it with FBI or a similar CIA installer (requires custom
firmware). The app creates `SD:/3ds/3DSpotify/` if needed and keeps its
settings and local data there.

Both the `.3dsx` and the `.cia` pack the repository's `romfs/` directory,
which the app reads as `romfs:/`; the build stops if it is missing. It holds
the logo of the top screen, `romfs/gfx/logo.png`:

- your own artwork, with no Spotify name or logo;
- an 8-bit RGBA PNG of at most 256×128 px, drawn 1:1 without scaling (a
  larger image is not loaded); its alpha channel is kept, soft edges
  included.

Without a usable logo the top screen shows the app name as text instead.

The target starts a direct session: it refreshes the stored credential, offers
pairing when needed, and checks the access-point and library-session
connections. The refresh token is saved to the SD card for later launches.

### Signing in

While it connects, the top screen shows the logo and a spinner, and the bottom
screen shows the current step ("Step n of N"). **SELECT** switches the language
on every sign-in screen.

If no credential is stored, or it has expired, the console asks you to link the
account:

- **Type the code:** open `spotify.com/pair` on a phone or computer, log in,
  enter the code shown on the top screen and accept.
- **Or scan the QR code** on the bottom screen: it opens the same page with the
  code already filled in.
- **B** cancels pairing and leaves the stored credential untouched.

If the session cannot start, the bottom screen explains why:

- **A** (or the **Retry** button) tries again with the stored credential. **A**
  presses whichever button has the focus; left/right moves the focus.
- **Y** (or the **Pair** button) pairs again with a new code.
- **START** exits.

Without a network (Wi-Fi off, or the console's network or TLS setup failing),
only **START** and **SELECT** remain.

### Tabs

Once the session is up, the top of the bottom screen is a bar of four tabs, each shown by its
icon:

| Icon | Tab | What it holds |
|---|---|---|
| House | **Home** | Search for songs and repeat the last search. |
| Magnifying glass | **Search** | The latest results, the search in progress, or its error. |
| Equalizer | **Playing** | The current track and its controls. |
| Gear | **Settings** | Language, reconnect and pair another account. |

- Tap a tab to open it (it switches when you lift the stylus inside it).
- **L/R** move to the previous or next tab from any screen; **ZL/ZR** (New 3DS only) do the
  same.
- Music keeps playing on every tab, and a search runs in the background while you move
  between them.
- **SELECT** switches the language from any tab and **START** exits.

### Top screen

- With nothing playing, or after a playback error, every tab shows the logo.
- While a track loads, plays (paused too) or has just finished, **Home**,
  **Search** and **Settings** show the now-playing card: cover, title, artist,
  album, the time and the progress bar with the download ahead of it. The
  **Playing** tab shows the cover large instead.
- While a search runs, the **Search** tab adds a floating "Searching…" bubble
  with the loading indicator over the logo or the card.

### Home

Tap **"Search songs"** or press **A** to open the keyboard. Your last search appears below as a secondary button; tap it or move to it with up/down and press **A**. Once confirmed, the app moves to the **Search** tab.

**Touch controls:**
- Tap a button to highlight it; the button shows a pressed state when you lift the stylus inside it.
- Sliding the stylus off a button before lifting cancels the tap.

**Button hints:** Glyphs with colored backgrounds show available actions:
- <span style="background-color: #FF4F45; color: white; padding: 2px 4px; border-radius: 2px;">A</span> — opens the search keyboard

While the keyboard is open, the playing track pauses; it resumes once the keyboard closes.

### Search

Results show four songs at a time. The **bottom screen** shows the list with thumbnails under the tab bar; the top screen follows [Top screen](#top-screen).

**Navigation:**
- Use up/down to move through songs; the highlighted one gets a brighter green background.
- Press left/right to jump four songs at a time.
- Tap a song to highlight it; tap it again (or press **A**) to play it: the app moves to the **Playing** tab.
- Drag the list with the stylus to scroll; the bar on the right edge shows your position.

**Button hints:**
- <span style="background-color: #FF4F45; color: white; padding: 2px 4px; border-radius: 2px;">A</span> — plays the highlighted song
- <span style="background-color: #FFD23F; color: black; padding: 2px 4px; border-radius: 2px;">B</span> — returns to the Home tab
- <span style="background-color: #39D97A; color: black; padding: 2px 4px; border-radius: 2px;">Y</span> — enters a new search
- Left/right — jump four songs at a time

**Touch gestures:**
- **Tap a row:** selects the song; the row highlights.
- **Tap again:** plays immediately with the remaining results as the queue.
- **Drag vertically:** scrolls the list; rows can be cut off by the tab bar and the button bar.
- **Drag beyond 8 pixels from the starting point:** commits to scrolling; the row tap is cancelled.

**Search status:**
- Before the first search, the tab offers a **"Search songs"** button.
- If the search fails, press **A** (or tap the on-screen button) to retry.
- Press **B** to cancel a pending search after the current network request finishes.
- While the queue from these results plays, the highlighted row follows the playing track.

### Playing

The chosen track plays locally with the rest of the results as the queue, and keeps playing when you switch tabs.
- The **top screen** shows the album artwork large (a note placeholder until it arrives).
- The **bottom screen** shows the title, artist and place in the queue ("3 of 24"), the previous, play/pause and next buttons, the time, the progress bar (with the download ahead of it) and the volume bar.

**Playback controls:**
- <span style="background-color: #FF4F45; color: white; padding: 2px 4px; border-radius: 2px;">A</span> — pauses/resumes; once the track has finished, plays it again; after an error, retries
- Left/right, or the on-screen buttons — changes track
- Up/down — adjusts local volume
- <span style="background-color: #FFD23F; color: black; padding: 2px 4px; border-radius: 2px;">B</span> — returns to the Search tab without stopping the music
- **START** — stops and exits

The queue follows search-result order and does not wrap at either end: there, the previous or next button shows as disabled. Complete playback, artwork,
and controls still need validation on real hardware. The probes remain available as separate targets.

### Settings

- **Language** — shows the current language and switches it (so does **SELECT** from any tab).
- **Reconnect** — opens the session again with the saved credential.
- **Pair another account** — starts pairing with a new code.

Choose with up/down and **A**, or by tapping the button. Reconnecting and pairing stop playback. **B** returns to the Home tab.

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