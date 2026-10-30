# SPDX-License-Identifier: AGPL-3.0-or-later
# Read-only MODX M/ESP .pfm inventory; prints counts only. Offsets and the
# waveform-reference signature come from YSFC Forge (Johan Adolfsson, MIT),
# docs/Y2L_FORMAT_SPECIFICATION.md and serializer/ysfc_serializer.py @ 845cf468.
import collections, json, os, sys

ENGINES = {0: 'AWM2', 1: 'Drum', 2: 'FM-X', 3: 'AN-X'}


def waveform_banks(blob):
    """Banks of SIG_A waveform references: 1 = user (YSFC-verified), 0 = factory (extrapolated)."""
    found = set()
    for i in range(len(blob) - 13):
        if (blob[i:i + 4] == b'\0\0\0\x28' and blob[i + 4] in (0, 1) and blob[i + 7] == 0
                and blob[i + 9] == 0 and blob[i + 10:i + 13] == b'\x01\0\x01'):
            found.add(blob[i + 4])
    return found


stats = collections.Counter()
for name in sorted(os.listdir(sys.argv[1])):
    if not name.endswith('.pfm'):
        continue
    blob = open(os.path.join(sys.argv[1], name), 'rb').read()
    stats['performances'] += 1
    if len(blob) < 6701:
        stats['too short'] += 1
        continue
    stats['part 1: ' + ENGINES.get(blob[6700], 'unknown')] += 1
    if blob[6695] > 1:
        stats['more than one Part'] += 1
    banks = waveform_banks(blob)
    if 0 in banks:
        stats['factory waveform reference'] += 1
    if 1 in banks:
        stats['user waveform reference'] += 1
print(json.dumps(stats, indent=1))
