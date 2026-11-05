"""Build native 3DS apps with devkitPro (devkitARM, libctru and 3dsxtool).

From the repository root:
    python client/source/build.py --app spotify3ds --fetch-deps

Python 3.12+ is required. --fetch-deps downloads pinned dependencies when
missing; subsequent builds can run offline without that option. The native
application is separate from the legacy proxy client built with client/Makefile.
"""
import argparse
import hashlib
import io
import os
import subprocess
import tarfile
import time
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent
ARCH = ["-march=armv6k", "-mtune=mpcore", "-mfloat-abi=hard", "-mtp=soft"]
MBEDTLS_VERSION = "3.6.7"
MBEDTLS_SHA256 = "a7e8bcbec0e6f761b4af24f25677626b35f762f68eef79c08677a363212d11f6"
STB_VORBIS_COMMIT = "d14d46f6b44456eb88beac58cba833a4135d4c89"
STB_VORBIS_SHA256 = "4c7cb2ff1f7011e9d67950446b7eb9ca044f2e464d76bfbb0b84dd2e23e65636"

# name -> (source directories, extra defines, mbedTLS flavour or None,
#          extra single sources relative to this script)
APPS = {
    "spotify3ds": (["native", "platform3ds"], [], "release", []),
    "probe_tls": (["probe_tls", "platform3ds"], ["-DPROBE_BUILD"], "probe", []),
    "probe_pair": (["probe_pair", "platform3ds"], ["-DPROBE_BUILD"], "probe", []),
    "probe_ap": (["probe_ap", "platform3ds"], ["-DPROBE_BUILD"], "probe", []),
    "probe_meta": (["probe_meta", "platform3ds"], ["-DPROBE_BUILD"], "probe", []),
    "probe_audio": (["probe_audio"], [], None, ["deps/stb_vorbis.c"]),
    "probe_play": (["probe_play", "platform3ds"], ["-DPROBE_BUILD"], "probe",
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


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--app", choices=sorted(APPS), default="spotify3ds")
    ap.add_argument("--devkitpro", "--toolchain", dest="devkitpro", type=Path,
                    help="devkitPro root (not a Melee shim)")
    ap.add_argument("--fetch-deps", action="store_true",
                    help="download and verify pinned mbedTLS/stb_vorbis dependencies")
    ap.add_argument("--debug", action="store_true", help="-O0 with assertions")
    args = ap.parse_args()

    sdk = find_devkitpro(args.devkitpro)
    cc = require_tool(sdk / "devkitARM" / "bin", "arm-none-eabi-gcc")
    ar = require_tool(sdk / "devkitARM" / "bin", "arm-none-eabi-ar")
    packer = require_tool(sdk / "tools" / "bin", "3dsxtool")
    if not (sdk / "libctru" / "include" / "3ds.h").is_file():
        raise SystemExit(f"libctru is missing under {sdk}")

    app = args.app
    srcdirs, defines, mbed, extra_sources = APPS[app]
    sources = []
    for directory in srcdirs:
        files = sorted((ROOT / directory).glob("*.c"))
        if not files:
            raise SystemExit(f"No sources under {ROOT / directory}")
        sources.extend((directory, src) for src in files)
    for rel in extra_sources:
        path = (stb_vorbis_source(args.fetch_deps)
                if rel == "deps/stb_vorbis.c" else ROOT / rel)
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
                "-I" + str(ROOT / "deps"), "-I" + str(ROOT / "platform3ds")]
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

    dest = ROOT / "dist" / "3ds" / app / f"{app}.3dsx"
    dest.parent.mkdir(parents=True, exist_ok=True)
    pack = [str(packer), str(elf), str(dest)]
    smdh = ROOT / "assets" / "icon.smdh"
    if app == "spotify3ds" and smdh.exists():
        pack.append(f"--smdh={smdh}")
    subprocess.run(pack, check=True)
    print(f"\nBuilt {dest}  ({dest.stat().st_size:,} bytes)")
    if app == "spotify3ds":
        print(f"Copy {dest.parent} to SD:/3ds/ and launch {app}.3dsx from "
              "Homebrew Launcher, or send it with 3dslink.")


if __name__ == "__main__":
    main()
