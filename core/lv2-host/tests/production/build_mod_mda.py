#!/usr/bin/env python3
"""Build selected unmodified MOD MDA sources using the inputs in their wscript.

The bundled Waf launcher does not parse on current Python. This replaces only the
build invocation; it does not patch plugin code or remove required features.
"""
import argparse
import os
from pathlib import Path
import platform
import shlex
import shutil
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument("source", type=Path)
parser.add_argument("output", type=Path)
parser.add_argument("--mod-data", type=Path)
args = parser.parse_args()
source = args.source.resolve()
flags = shlex.split(subprocess.check_output(["pkg-config", "--cflags", "lv2"], text=True))
compiler = shlex.split(os.environ.get("CXX", "c++"))
for name in ("JX10", "DX10", "EPiano", "Ambience", "DubDelay", "Overdrive"):
    bundle = args.output.resolve() / f"mod-mda-{name}.lv2"
    bundle.mkdir(parents=True, exist_ok=True)
    original = source / "bundles" / bundle.name
    for ttl in original.glob("*.ttl"):
        shutil.copy2(ttl, bundle / ttl.name)
    (bundle / "manifest.ttl").write_text((original / "manifest.ttl.in").read_text().replace("@LIB_EXT@", ".so"))
    subprocess.run(compiler + ["-O2", "-std=c++11", "-fPIC",
        "-dynamiclib" if platform.system() == "Darwin" else "-shared",
        "-I.", "-Ilvz", "-Isrc", *flags,
        f"-DPLUGIN_CLASS=mda{name}", '-DURI_PREFIX="http://moddevices.com/plugins/mda/"',
        f'-DPLUGIN_URI_SUFFIX="{name}"', f'-DPLUGIN_HEADER="src/mda{name}.h"',
        f"src/mda{name}.cpp", "lvz/wrapper.cpp", "-o", str(bundle / f"{name}.so")], cwd=source, check=True)
    if args.mod_data:
        overlay = args.mod_data / "plugins-fixed" / bundle.name
        if not overlay.is_dir():
            raise RuntimeError(f"Missing MOD overlay: {overlay}")
        for ttl in overlay.glob("*.ttl"):
            shutil.copy2(ttl, bundle / ttl.name)
    print(bundle)
