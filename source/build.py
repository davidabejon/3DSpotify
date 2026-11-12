"""Build native 3DS apps with devkitPro (devkitARM, libctru and 3dsxtool).

From the repository root:
    python source\\build.py --app 3dspotify --fetch-deps --debug

Add --cia to also package source/dist/3ds/3DSpotify/3DSpotify.cia for FBI and
similar installers (needs makerom and bannertool from devkitPro's tools).

Python 3.12+ is required. --fetch-deps downloads pinned dependencies when
missing; subsequent builds can run offline without that option. The native
application is the only supported client target; the legacy proxy client has
been removed.
"""
import argparse
import hashlib
import io
import os
import subprocess
import tarfile
import time
import urllib.request
import wave
from pathlib import Path

ROOT = Path(__file__).resolve().parent
ARCH = ["-march=armv6k", "-mtune=mpcore", "-mfloat-abi=hard", "-mtp=soft"]
MBEDTLS_VERSION = "3.6.7"
MBEDTLS_SHA256 = "a7e8bcbec0e6f761b4af24f25677626b35f762f68eef79c08677a363212d11f6"
STB_VORBIS_COMMIT = "d14d46f6b44456eb88beac58cba833a4135d4c89"
STB_VORBIS_SHA256 = "4c7cb2ff1f7011e9d67950446b7eb9ca044f2e464d76bfbb0b84dd2e23e65636"
# Draws the pairing QR on the login screen. Two files, each pinned on its own:
# the header is included by the app's sources, not only by qrcodegen.c.
QRCODEGEN_VERSION = "1.8.0"
QRCODEGEN_SHA256_C = "300eff07ee25baaa7578f20284411638154716379437391e7e689c0e6ce81403"
QRCODEGEN_SHA256_H = "e82df4bff37d18b5863b9e7486fe6bda1b6cda8c3b9ecebfec473907265cb589"

# Icon, banner and makerom settings for the installable app, kept in assets/
# at the repository root.
ASSETS = ROOT.parent / "assets"
# Files the app reads at run time as romfs:/ (the logo), packed into both the
# .3dsx and the .cia of 3dspotify.
ROMFS = ROOT.parent / "romfs"
APP_TITLE = "3DSpotify"
APP_DESCRIPTION = "Native Spotify client for Nintendo 3DS"
APP_PUBLISHER = "David Abejon"
# The Home Menu plays at most ~3 s of banner audio; a longer clip makes the
# banner too large and can hang the menu.
BANNER_AUDIO_SECONDS = 2.9
BANNER_FADE_SECONDS = 0.4

# name -> (source directories, extra defines, mbedTLS flavour or None,
#          extra single sources relative to this script)
APPS = {
    "3dspotify": (["ui", "lang", "platform3ds"], [], "release",
                   ["main.c", "deps/stb_vorbis.c", "deps/qrcodegen.c"]),
    "probe_tls": (["probe_tls", "platform3ds"], ["-DPROBE_BUILD"], "probe", []),
    "probe_pair": (["probe_pair", "platform3ds"], ["-DPROBE_BUILD"], "probe", []),
    "probe_ap": (["probe_ap", "platform3ds"], ["-DPROBE_BUILD"], "probe", []),
    "probe_meta": (["probe_meta", "platform3ds"], ["-DPROBE_BUILD"], "probe", []),
    "probe_audio": (["probe_audio"], [], None, ["deps/stb_vorbis.c"]),
    "probe_play": (["probe_play", "platform3ds"], ["-DPROBE_BUILD"], "probe",
                   ["deps/stb_vorbis.c"]),
    # F0 of the citro2d plan: throwaway hardware spike, removed after F1.
    "probe_ui": (["probe_ui", "platform3ds"], ["-DPROBE_BUILD"], "probe",
                 ["deps/stb_vorbis.c"]),
}


def find_devkitpro(explicit: Path | None) -> Path:
    candidates = [explicit] if explicit is not None else [
        Path(os.environ["DEVKITPRO"]) if os.environ.get("DEVKITPRO") else None,
        Path("C:\\devkitPro") if os.name == "nt" else Path("/opt/devkitpro"),
    ]
    for root in candidates:
        if root is not None and (root / "devkitARM").is_dir():
            return root.resolve()
    raise SystemExit("devkitPro not found; use --devkitpro PATH or set DEVKITPRO")


def require_tool(directory: Path, name: str) -> Path:
    path = directory / (name + (".exe" if os.name == "nt" else ""))
    if not path.is_file():
        raise SystemExit(f"Missing tool: {path}")
    return path


def stb_vorbis_source(fetch: bool) -> Path:
    source = ROOT / "deps" / "stb_vorbis.c"
    if source.is_file():
        data = source.read_bytes()
    else:
        if not fetch:
            raise SystemExit("stb_vorbis.c is missing; rerun with --fetch-deps")
        url = ("https://raw.githubusercontent.com/nothings/stb/"
               f"{STB_VORBIS_COMMIT}/stb_vorbis.c")
        print("Downloading stb_vorbis.c...", flush=True)
        with urllib.request.urlopen(url, timeout=90) as response:
            data = response.read()
    if hashlib.sha256(data).hexdigest() != STB_VORBIS_SHA256:
        raise SystemExit("stb_vorbis.c checksum mismatch")
    if not source.is_file():
        source.parent.mkdir(parents=True, exist_ok=True)
        source.write_bytes(data)
    marker = b"//  HEADER ENDS HERE"
    header_end = data.find(marker)
    if header_end < 0:
        raise SystemExit("stb_vorbis.c does not contain its public header")
    header = source.with_name("stb_vorbis.h")
    header.write_bytes(data[:header_end].rstrip() + b"\n")
    return source


def qrcodegen_source(fetch: bool) -> Path:
    """Returns deps/qrcodegen.c, with qrcodegen.h beside it, both verified.

    Nothing is written unless both files match: a half-updated pair would
    compile against the wrong header."""
    deps = ROOT / "deps"
    pinned = {"qrcodegen.c": QRCODEGEN_SHA256_C,
              "qrcodegen.h": QRCODEGEN_SHA256_H}
    contents = {}
    for name, digest in pinned.items():
        path = deps / name
        if path.is_file():
            data = path.read_bytes()
        else:
            if not fetch:
                raise SystemExit(f"{name} is missing; rerun with --fetch-deps")
            url = ("https://raw.githubusercontent.com/nayuki/QR-Code-generator/"
                   f"v{QRCODEGEN_VERSION}/c/{name}")
            print(f"Downloading {name}...", flush=True)
            with urllib.request.urlopen(url, timeout=90) as response:
                data = response.read()
        if hashlib.sha256(data).hexdigest() != digest:
            raise SystemExit(f"{name} checksum mismatch")
        contents[name] = data
    for name, data in contents.items():
        path = deps / name
        if not path.is_file():
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
    return deps / "qrcodegen.c"


def mbedtls_tree(fetch: bool) -> Path:
    name = f"mbedtls-{MBEDTLS_VERSION}"
    tree = ROOT / "deps" / name
    if (tree / "include" / "mbedtls" / "ssl.h").is_file():
        return tree
    if not fetch:
        raise SystemExit("mbedTLS is missing; rerun with --fetch-deps")
    if tree.exists():
        raise SystemExit(f"Incomplete mbedTLS tree: {tree}; move it aside and retry")
    url = (f"https://github.com/Mbed-TLS/mbedtls/releases/download/{name}/"
           f"{name}.tar.bz2")
    print(f"Downloading {name}...", flush=True)
    with urllib.request.urlopen(url, timeout=90) as response:
        archive = response.read()
    if hashlib.sha256(archive).hexdigest() != MBEDTLS_SHA256:
        raise SystemExit("mbedTLS archive checksum mismatch")
    with tarfile.open(fileobj=io.BytesIO(archive), mode="r:bz2") as tar:
        if any(not member.name.startswith(name + "/") for member in tar.getmembers()):
            raise SystemExit("Unexpected paths in mbedTLS archive")
        tar.extractall(ROOT / "deps", filter="data")
    return tree


def build_mbedtls(cc: Path, ar: Path, tree: Path, out: Path,
                  flags: list[str], includes: list[str]) -> Path:
    objects = []
    dep_out = out / "mbedtls"
    dep_out.mkdir(parents=True, exist_ok=True)
    # Always rebuild with the app's exact configuration, including PROBE_BUILD
    # and debug flags, so cached archives cannot silently use a different ABI.
    for src in sorted((tree / "library").glob("*.c")):
        obj = dep_out / (src.stem + ".o")
        subprocess.run([str(cc), *ARCH, *flags, "-w", *includes,
                        "-c", str(src), "-o", str(obj)], check=True)
        objects.append(obj)
    if not objects:
        raise SystemExit(f"No mbedTLS sources in {tree}")
    lib = dep_out / "libmbedtls.a"
    lib.unlink(missing_ok=True)
    # Keep commands below Windows' command-line length limit.
    for start in range(0, len(objects), 32):
        subprocess.run([str(ar), "rcs", str(lib),
                        *map(str, objects[start:start + 32])], check=True)
    return lib


def banner_audio(source: Path, dest: Path) -> Path:
    """Copies the start of the banner jingle, faded out, short enough for the
    Home Menu."""
    with wave.open(str(source), "rb") as src:
        if src.getsampwidth() != 2:
            raise SystemExit(f"{source} must be 16-bit PCM")
        channels, rate = src.getnchannels(), src.getframerate()
        frames = min(src.getnframes(), int(rate * BANNER_AUDIO_SECONDS))
        data = bytearray(src.readframes(frames))
    fade = min(frames, int(rate * BANNER_FADE_SECONDS))
    samples = memoryview(data).cast("h")
    for frame in range(frames - fade, frames):
        gain = (frames - frame) / fade
        for channel in range(channels):
            index = frame * channels + channel
            samples[index] = int(samples[index] * gain)
    samples.release()
    with wave.open(str(dest), "wb") as out:
        out.setnchannels(channels)
        out.setsampwidth(2)
        out.setframerate(rate)
        out.writeframes(bytes(data))
    return dest


def make_smdh(bannertool: Path, out: Path) -> Path:
    smdh = out / "icon.smdh"
    subprocess.run([str(bannertool), "makesmdh",
                    "-s", APP_TITLE, "-l", APP_DESCRIPTION,
                    "-p", APP_PUBLISHER, "-i", str(ASSETS / "icon.png"),
                    "-o", str(smdh)], check=True, stdout=subprocess.DEVNULL)
    return smdh


def require_romfs(romfs: Path) -> Path:
    if not romfs.is_dir():
        raise SystemExit(f"The romfs directory is missing: {romfs}. Create it "
                         "at the repository root with gfx/logo.png inside.")
    return romfs.resolve()


def pack_command(packer: Path, elf: Path, dest: Path,
                 smdh: Path | None, romfs: Path | None) -> list[str]:
    command = [str(packer), str(elf), str(dest)]
    if smdh is not None:
        command.append(f"--smdh={smdh}")
    if romfs is not None:
        command.append(f"--romfs={romfs}")
    return command


def makerom_command(makerom: Path, elf: Path, smdh: Path, banner: Path,
                    dest: Path, romfs: Path) -> list[str]:
    # The RSF reads the romfs root from $(APP_ROMFS): an absolute path, so
    # it does not depend on the working directory. Forward slashes keep
    # makerom's YAML parser away from Windows backslashes.
    return [str(makerom), "-f", "cia", "-o", str(dest),
            "-target", "t", "-exefslogo",
            "-elf", str(elf), "-rsf", str(ASSETS / "template.rsf"),
            "-icon", str(smdh), "-banner", str(banner),
            f"-DAPP_ROMFS={romfs.as_posix()}"]


def output_name(app: str) -> str:
    """Base name of the output folder and of the .3dsx and .cia files: the
    app's title for the real client, the target name for the probes."""
    return APP_TITLE if app == "spotify3ds" else app


def make_cia(sdk: Path, bannertool: Path, elf: Path, smdh: Path,
             out: Path, dest: Path, romfs: Path) -> Path:
    makerom = require_tool(sdk / "tools" / "bin", "makerom")
    audio = banner_audio(ASSETS / "banner.wav", out / "banner.wav")
    banner = out / "banner.bnr"
    subprocess.run([str(bannertool), "makebanner",
                    "-i", str(ASSETS / "banner.png"), "-a", str(audio),
                    "-o", str(banner)], check=True, stdout=subprocess.DEVNULL)
    subprocess.run(makerom_command(makerom, elf, smdh, banner, dest, romfs),
                   check=True)
    return dest


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--app", choices=sorted(APPS), default="3dspotify")
    ap.add_argument("--devkitpro", "--toolchain", dest="devkitpro", type=Path,
                    help="devkitPro root (not a Melee shim)")
    ap.add_argument("--fetch-deps", action="store_true",
                    help="download and verify pinned mbedTLS/stb_vorbis/qrcodegen "
                         "dependencies")
    ap.add_argument("--debug", action="store_true", help="-O0 with assertions")
    ap.add_argument("--cia", action="store_true",
                    help="also build an installable .cia (3dspotify only)")
    args = ap.parse_args()

    sdk = find_devkitpro(args.devkitpro)
    cc = require_tool(sdk / "devkitARM" / "bin", "arm-none-eabi-gcc")
    ar = require_tool(sdk / "devkitARM" / "bin", "arm-none-eabi-ar")
    packer = require_tool(sdk / "tools" / "bin", "3dsxtool")
    if not (sdk / "libctru" / "include" / "3ds.h").is_file():
        raise SystemExit(f"libctru is missing under {sdk}")

    app = args.app
    if args.cia and app != "3dspotify":
        raise SystemExit("--cia is only supported for 3dspotify")
    # Checked before compiling, so a missing romfs fails fast.
    romfs = require_romfs(ROMFS) if app == "3dspotify" else None
    srcdirs, defines, mbed, extra_sources = APPS[app]
    sources = []
    for directory in srcdirs:
        files = sorted((ROOT / directory).glob("*.c"))
        if not files:
            raise SystemExit(f"No sources under {ROOT / directory}")
        sources.extend((directory, src) for src in files)
    pinned_sources = {"deps/stb_vorbis.c": stb_vorbis_source,
                      "deps/qrcodegen.c": qrcodegen_source}
    for rel in extra_sources:
        path = (pinned_sources[rel](args.fetch_deps)
                if rel in pinned_sources else ROOT / rel)
        if not path.is_file():
            raise SystemExit(f"Required source is missing: {path}")
        sources.append((path.parent.name, path))

    out = ROOT / "build" / app
    out.mkdir(parents=True, exist_ok=True)
    stamp = time.strftime("%H:%M:%S")
    flags = ["-std=gnu11", "-D__3DS__", f'-DBUILD_STAMP="{stamp}"',
             "-O0" if args.debug else "-O2", "-g", "-mword-relocations",
             "-ffunction-sections", "-fdata-sections", *defines]
    includes = ["-I" + str(sdk / "libctru" / "include"),
                "-I" + str(ROOT / "deps"), "-I" + str(ROOT / "ui"),
                "-I" + str(ROOT / "lang"),
                "-I" + str(ROOT / "platform3ds")]
    extra = []
    if mbed:
        tree = mbedtls_tree(args.fetch_deps)
        flags.append('-DMBEDTLS_CONFIG_FILE="mbedtls_config_3ds.h"')
        includes.insert(0, "-I" + str(tree / "include"))
        print(f"Building mbedTLS {MBEDTLS_VERSION} ({mbed})...", flush=True)
        extra.append(build_mbedtls(cc, ar, tree, out, flags, includes))

    objects = []
    for directory, src in sources:
        obj = out / f"{directory.replace('/', '_')}_{src.stem}.o"
        print(f"Compiling {src.name}", flush=True)
        subprocess.run([str(cc), *ARCH, *flags,
                        *(["-w"] if "deps" in src.parts else ["-Wall", "-Wextra"]),
                        *includes, "-c", str(src), "-o", str(obj)], check=True)
        objects.append(obj)

    elf = out / f"{app}.elf"
    subprocess.run([str(cc), *ARCH, "-specs=3dsx.specs", "-g",
                    "-Wl,-Map," + str(out / f"{app}.map"),
                    "-o", str(elf), *map(str, objects), *map(str, extra),
                    "-L" + str(sdk / "libctru" / "lib"),
                    "-Wl,--start-group", "-lcitro2d", "-lcitro3d", "-lctru",
                    "-lm", "-Wl,--end-group"], check=True)

    output_name = APP_TITLE if app == "3dspotify" else app
    dest = ROOT / "dist" / "3ds" / output_name / f"{output_name}.3dsx"
    dest.parent.mkdir(parents=True, exist_ok=True)
    bannertool = smdh = None
    if app == "3dspotify":
        bannertool = require_tool(sdk / "tools" / "bin", "bannertool")
        smdh = make_smdh(bannertool, out)
    subprocess.run(pack_command(packer, elf, dest, smdh, romfs), check=True)
    print(f"\nBuilt {dest}  ({dest.stat().st_size:,} bytes)")
    if args.cia:
        cia = make_cia(sdk, bannertool, elf, smdh, out,
                       dest.with_suffix(".cia"), romfs)
        print(f"Built {cia}  ({cia.stat().st_size:,} bytes)")
        print("Copy it to the SD card and install it with FBI or a similar "
              "CIA installer.")
    if app == "3dspotify":
        print(f"Copy {dest.parent} to SD:/3ds/ and launch {dest.name} from "
              "Homebrew Launcher, or send it with 3dslink.")


if __name__ == "__main__":
    main()
