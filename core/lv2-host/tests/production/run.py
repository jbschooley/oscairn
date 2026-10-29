#!/usr/bin/env python3
"""Opt-in production LV2 checks. No downloads, installs, or metadata modifications."""
import argparse
import array
import hashlib
import json
import math
from pathlib import Path
import platform
import struct
import subprocess
import sys


def samples(path):
    data = path.read_bytes()
    assert data[:4] == b"RIFF" and data[8:12] == b"WAVE"
    offset = 12
    while offset + 8 <= len(data):
        tag, size = struct.unpack_from("<4sI", data, offset)
        chunk = data[offset + 8:offset + 8 + size]
        if tag == b"fmt ":
            assert struct.unpack_from("<H", chunk)[0] == 3  # IEEE float
        if tag == b"data":
            result = array.array("f")
            result.frombytes(chunk)
            if sys.byteorder != "little":
                result.byteswap()
            return result
        offset += 8 + size + size % 2
    raise ValueError("No WAV data")


def difference(a, b):
    assert len(a) == len(b)
    return math.sqrt(sum((x - y) ** 2 for x, y in zip(a, b)) / len(a))


def invoke(command):
    try:
        run = subprocess.run(list(map(str, command)), capture_output=True, text=True, timeout=30)
        return {"exit": run.returncode, "stdout": run.stdout, "stderr": run.stderr}
    except subprocess.TimeoutExpired:
        return {"exit": -1, "stdout": "", "stderr": "30-second timeout"}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("probe", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--mod-root", type=Path, help="Directory containing the six mod-mda-*.lv2 bundles")
    parser.add_argument("--system-root", type=Path, help="Directory containing ZamComp, fil4 and Guitarix bundles")
    parser.add_argument("--renderer", type=Path, help="Also test complete mixed-format chains using oscairn-render")
    parser.add_argument("--sequencer", type=Path, help="Oscairn CLAP step sequencer for chain tests")
    parser.add_argument("--prepared-probe", type=Path, help="Also compare instrument switches with independent reference instances")
    parser.add_argument("--only", nargs="+", help="Restrict individual-plugin tests to these case names")
    args = parser.parse_args()
    if not (args.mod_root or args.system_root):
        parser.error("Specify at least one bundle root")
    if args.renderer and not (args.sequencer and args.mod_root):
        parser.error("--renderer requires --sequencer and --mod-root")
    cases = []
    if args.mod_root:
        for name, kind, param, value in [
            ("JX10", "synth", 6, .2), ("DX10", "synth", 5, 80),
            ("EPiano", "synth", 3, 40), ("Ambience", "effect", 2, 20),
            ("DubDelay", "effect", 5, 10), ("Overdrive", "effect", 0, 5),
        ]:
            cases.append((name, args.mod_root / f"mod-mda-{name}.lv2",
                          f"http://moddevices.com/plugins/mda/{name}", kind, param, value, None))
    if args.system_root:
        for name, bundle, uri, param, value, rejection in [
            ("ZamComp", "ZamComp", "urn:zamaudio:ZamComp", 7, -30, None),
            ("GxChorus", "gx_chorus", "http://guitarix.sourceforge.net/plugins/gx_chorus_stereo#_chorus_stereo", 2, .8, None),
            ("GxCompressor", "gx_compressor", "http://guitarix.sourceforge.net/plugins/gx_compressor#_compressor", 2, -40, None),
            ("GxZita", "gx_zita_rev1", "http://guitarix.sourceforge.net/plugins/gx_zita_rev1_stereo#_zita_rev1_stereo", 9, 1, None),
            ("x42-fil4-stereo", "fil4", "http://gareus.org/oss/lv2/fil4#stereo", 3, -12, None),
        ]:
            cases.append((name, args.system_root / f"{bundle}.lv2", uri, "effect", param, value, rejection))
    if args.only:
        missing=set(args.only)-{case[0] for case in cases}
        if missing:
            parser.error("Unknown/unavailable cases: " + ", ".join(sorted(missing)))
        cases=[case for case in cases if case[0] in args.only]
    args.output.mkdir(parents=True, exist_ok=True)
    report = {"platform": platform.platform(), "python": platform.python_version(), "plugins": []}
    failed = False
    for name, bundle, uri, kind, param, value, rejection in cases:
        result = {"name": name, "bundle": str(bundle), "uri": uri, "runs": []}
        report["plugins"].append(result)
        inspection = invoke([args.probe, bundle, uri])
        result["inspection"] = inspection
        if inspection["exit"]:
            expected = rejection and rejection in inspection["stderr"] and inspection["exit"] == 1
            result["status"] = "unsupported" if expected else "failed"
            failed |= not expected
            print(name, result["status"], inspection["stderr"].strip(), flush=True)
            continue
        try:
            directory = args.output / name
            directory.mkdir(exist_ok=True)
            baseline = None
            for rate in (44100, 48000):
                for block in (37, 512):
                    for mode in ("offline", "realtime"):
                        label = f"{rate}-{block}-{mode}"
                        path = directory / (label + ".wav")
                        run = invoke([args.probe, bundle, uri, kind, rate, block, mode, -1, 0, path])
                        run["case"] = label
                        result["runs"].append(run)
                        if run["exit"]:
                            raise RuntimeError(run["stderr"] or run["stdout"])
                        audio = samples(path)
                        assert len(audio) > 0 and all(math.isfinite(x) for x in audio)
                        run["sha256"] = hashlib.sha256(path.read_bytes()).hexdigest()
                        if rate == 48000 and block == 512 and mode == "offline":
                            baseline = audio
            # A fresh instance each time catches initialization/repeat differences.
            for label, parameter, setting in (("repeat", -1, 0), ("edited", param, value)):
                path = directory / (label + ".wav")
                run = invoke([args.probe, bundle, uri, kind, 48000, 512, "offline", parameter, setting, path])
                run.update(case=label, parameter=parameter, value=setting)
                result["runs"].append(run)
                if run["exit"]:
                    raise RuntimeError(run["stderr"] or run["stdout"])
                delta = difference(baseline, samples(path))
                result[label + "_difference_rms"] = delta
            result["block_difference_rms"] = difference(baseline, samples(directory / "48000-37-offline.wav"))
            if result["edited_difference_rms"] <= max(1e-6, result["repeat_difference_rms"] * 5):
                raise RuntimeError("Parameter change not distinguishable from repeat variation")
            if kind == "synth":
                if args.prepared_probe:
                    transition = invoke([args.prepared_probe, "--production", bundle, uri, param, value])
                    transition["case"] = "prepared-instance-switch"
                    result["runs"].append(transition)
                    if transition["exit"]:
                        raise RuntimeError(transition["stderr"] or transition["stdout"])
                channels = len(baseline) // 96000
                assert max(map(abs, baseline[:4800 * channels])) < 1e-7, "Audio before first note"
                path = directory / "held.wav"
                run = invoke([args.probe, bundle, uri, "held", 48000, 512, "offline", -1, 0, path])
                run["case"] = "held"
                result["runs"].append(run)
                if run["exit"]:
                    raise RuntimeError(run["stderr"] or run["stdout"])
                held = samples(path)
                tail_start = 86400 * channels
                result["released_tail_rms"] = math.sqrt(sum(x*x for x in baseline[tail_start:]) / len(baseline[tail_start:]))
                result["held_tail_rms"] = math.sqrt(sum(x*x for x in held[tail_start:]) / len(held[tail_start:]))
                assert result["released_tail_rms"] < result["held_tail_rms"] * .9, "Note-offs did not reduce the tail"
            result["status"] = "passed"
        except Exception as error:
            result.update(status="failed", error=str(error))
            failed = True
        print(name, result["status"], result.get("error", ""), flush=True)
    if args.renderer:
        report["chains"] = []
        chains = [("JX10-Ambience", ["JX10", "Ambience"]),
                  ("DX10-DubDelay", ["DX10", "DubDelay"]),
                  ("EPiano-Overdrive", ["EPiano", "Overdrive"])]
        for label, names in chains:
            for block in (37, 512):
                path = args.output / f"chain-{label}-{block}.wav"
                plugins = [f"{args.mod_root}/mod-mda-{n}.lv2#http://moddevices.com/plugins/mda/{n}" for n in names]
                run = invoke([args.renderer, path, "--seconds", 2, "--block", block, args.sequencer, *plugins])
                run.update(case=label, block=block)
                try:
                    assert run["exit"] == 0 and "reported an error" not in run["stderr"]
                    audio = samples(path)
                    assert audio and all(math.isfinite(x) for x in audio) and max(map(abs, audio)) > 1e-7
                    run["status"] = "passed"
                except Exception as error:
                    run.update(status="failed", error=str(error))
                    failed = True
                report["chains"].append(run)
                print("chain", label, block, run["status"], flush=True)
    (args.output / "results.json").write_text(json.dumps(report, indent=2) + "\n")
    return int(failed)


if __name__ == "__main__":
    sys.exit(main())
