# Contributing to 3DSpotify

Thanks for your interest in 3DSpotify. This guide explains how to report
problems, propose changes and send code that is easy to review and merge.

3DSpotify is an unofficial, independent project. It is not affiliated with or
endorsed by Spotify. Contributions must respect that (see
[Scope and legal](#scope-and-legal)).

## Before you start

- Read the [README](README.md) (or [README_es.md](README_es.md)) to set up the
  toolchain and learn how the client and the probes are built.
- Search the [existing issues](https://github.com/davidabejon/3DSpotify/issues)
  before opening a new one.
- For anything bigger than a small fix, open an issue first and describe what
  you plan to do. It avoids duplicate work and lets us agree on the approach
  before you write the code.

## Reporting bugs and requesting features

Issues use the template in `.github/ISSUE_TEMPLATE/default.md`. Fill in every
section that applies. For bugs, include:

- What you did, what you expected and what happened instead.
- Console model (Old/New 3DS, 2DS, XL) and system version.
- How you launched the app: `.3dsx` from the Homebrew Launcher, `3dslink`, or
  the installed `.cia`.
- The commit or release you built from.
- Logs, if any. When the problem is network-related, run the probes in order
  (`probe_tls`, `probe_pair`, `probe_ap`, `probe_meta`, `probe_audio`,
  `probe_play`) and report the first one that fails. `probe_tls` writes
  `spotify-tls.log` next to the `.3dsx`.

**Never paste credentials.** Remove refresh tokens, access tokens, pairing
codes, account names and anything read from the SD card's settings folder
before sharing logs or screenshots.

For security problems (for example, a way to leak the stored token), do not
open a public issue; contact the maintainer privately through GitHub.

## Development setup

You need devkitPro with devkitARM, libctru, citro2d, citro3d and 3dsxtool, plus
Python 3.12 or newer. From the repository root:

```powershell
python source\build.py --app 3dspotify --fetch-deps --debug
python source\build.py --app probe_tls       # build a probe
python source\build.py --debug               # -O0 with assertions
python source\build.py --cia                 # also package the CIA
python -m unittest discover -s tests         # run the test suite
```

`--fetch-deps` downloads mbedTLS, `stb_vorbis` and `qrcodegen` and verifies
them against pinned checksums. Do not commit anything under `source/dist/`,
`source/deps/mbedtls-*` or other build output; `.gitignore` already covers it.

## Project layout

| Path | Contents |
|---|---|
| `source/main.c` | Entry point of the `3dspotify` client. |
| `source/ui/` | Screens, rendering, artwork and player logic. |
| `source/lang/` | User-facing text in every supported language. |
| `source/platform3ds/` | TLS, sockets, Spotify protocol and 3DS platform code. |
| `source/probe_*/` | Standalone diagnostic apps, one per step of the client. |
| `source/build.py` | Build script for every target. |
| `tests/` | Python tests for the build and host-compiled C tests for pure logic. |
| `assets/` | Banner, icon and packaging assets. |

## Making changes

### Branches and commits

- Branch off `main` and keep one topic per branch and pull request.
- Keep commits focused. A refactor and a behaviour change belong in separate
  commits.
- Commit messages use a short lowercase verb prefix followed by a period, then
  an imperative summary:

  | Prefix | Use for |
  |---|---|
  | `add.` | New features, files or documentation |
  | `fix.` | Bug fixes |
  | `upd.` | Changes to existing behaviour, config or docs |
  | `rm.` | Removing code, files or assets |
  | `mv.` | Moving or renaming files |

  Examples: `fix. reduce audio crackling during playback`,
  `mv. move app packaging assets to assets directory`.

  This is a convention, not a strict rule. A similar style, or any clear and
  readable message that says what the commit does, is fine.

### C code style

These are style guidelines, not hard rules. Try to match the surrounding code;
small deviations are fine as long as the code stays clear and consistent:

- C99, tabs for indentation, opening brace of a function on its own line.
- `snake_case` for functions, variables and types; `UPPER_CASE` for macros and
  enum constants.
- Mark file-local functions and data `static`.
- Use `/* ... */` comments that explain *why*, not *what*.
- Check every allocation, file and network call, and release resources on every
  error path.
- Keep logic that does not need 3DS APIs (queue handling, encoding, parsing) in
  headers or modules without 3DS dependencies, such as
  `source/ui/player_logic.h` or `source/lang/`, so the host tests can compile
  it.
- Do not add busy loops that redraw every frame; the UI renders on demand.
- Remember the target hardware: limited RAM and CPU, especially on Old 3DS.
  Avoid large stack buffers and unneeded allocations in the playback path.

### User-facing text and translations

All text shown on screen lives in `source/lang/`. Never hard-code strings in the
UI; `tests/test_build.py` checks for this.

To add or change a string:

1. Add the identifier to the `str_id` enum in `source/lang/lang.h`.
2. Add the text to **both** `lang_en.c` and `lang_es.c`, in the same position.
3. For `_FMT` entries, use the same `printf` conversion specifiers in the same
   order in every language.
4. `STR_KEYS_` entries are button hints and must fit on one line of the
   40-column bottom-screen console.
5. The console font is ASCII only: write Spanish text without accents or `ñ`.

If you don't speak one of the languages, add your best attempt and say so in the
pull request so someone can review it.

### Dependencies

New third-party code needs a good reason, a license compatible with Apache 2.0,
and a pinned version with a checksum verified by `build.py`. Credit it in the
README and, if it is copied into the repository, in `NOTICE`.

## Testing

- Run `python -m unittest discover -s tests` before every pull request. The C
  tests run when a host C compiler is available; otherwise they are reported as
  skipped. Mention in the PR if they were skipped for you.
- Add or update tests for logic you change. Pure logic belongs in
  `tests/native_logic_test.c`; build and wiring checks belong in
  `tests/test_build.py`.
- Make sure `python source\build.py` builds `3dspotify` without new warnings,
  and that any probe you touched still builds with `--app`.
- **Test on real hardware** whenever you change networking, audio, rendering,
  input or file handling. Say in the PR which console you tested on; if you
  could not test on hardware, say that too.

## Documentation

- Update `README.md` when you change build steps, controls, requirements or
  behaviour that users see.
- Keep `README_es.md` in sync with `README.md`. If you can't write the Spanish
  version, note it in the PR.

## Pull requests

1. Make sure your branch is up to date with `main` and the tests pass.
2. Open the pull request against `main` and link the related issue
   (`Closes #123`).
3. In the description, explain what changed and why, how you tested it (host
   tests, hardware model) and any known limitations.
4. Add screenshots or photos for visible UI changes.
5. Keep the pull request small enough to review. Large changes are easier to
   merge when split into a series.

Reviews may ask for changes; push them as new commits to the same branch.

## Scope and legal

- 3DSpotify requires a Spotify Premium account. Contributions that bypass
  Premium checks, remove DRM, save decrypted audio to the SD card or otherwise
  enable piracy will not be accepted.
- Do not add Spotify logos or other trademarked assets.
- Code in `source/platform3ds/`, `source/probe_*/` and `source/build.py` is
  derived from [SpotiBacan](https://github.com/ohquebacan/spotibacan). Keep the
  attribution in `NOTICE` intact when you modify those files.

## License

3DSpotify is licensed under the [Apache License 2.0](LICENSE). By submitting a
contribution, you agree that it is licensed under the same terms, as described
in section 5 of the license.
