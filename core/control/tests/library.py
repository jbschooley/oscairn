# SPDX-License-Identifier: AGPL-3.0-or-later
"""The library through RPC (RFC 0021): info, save and load by ID, query, favourites, recents, the cache."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile


try:
    import jsonschema
    protocol = json.loads((Path(__file__).resolve().parents[3] / 'docs/control-protocol.schema.json').read_text())
except ImportError:
    jsonschema = None
    print('JSON Schema validator not installed; requests are not checked against the schema')


def rpc(method, params=None, expect_error=False):
    request = {'jsonrpc': '2.0', 'id': 1, 'method': method, 'params': params or {}}
    if jsonschema and not expect_error:
        jsonschema.validate(request, {**protocol, '$ref': '#/$defs/Request'})
    p.stdin.write(json.dumps(request) + '\n')
    p.stdin.flush()
    response = json.loads(p.stdout.readline())
    assert ('error' in response) == expect_error, response
    return response.get('result')


with tempfile.TemporaryDirectory(prefix='oscairn-library-') as temporary:
    home = Path(temporary)
    p = subprocess.Popen([sys.argv[1], sys.argv[2], '--stdio'], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                         text=True, encoding='utf-8', env={**os.environ, 'OSCAIRN_DATA_HOME': temporary})
    try:
        assert rpc('library.query') == {'items': [], 'tags': [], 'total': 0}
        assert rpc('performance.info') == {'id': '', 'name': '', 'description': '', 'tags': [], 'color': ''}
        rpc('performance.setInfo', {'name': 'Warm Pad', 'description': 'slow strings', 'tags': ['pad', 'warm'], 'color': '#3a7bd5'})
        pad = rpc('library.save')['id']
        assert len(pad) == 16 and rpc('performance.info')['id'] == pad
        assert (home / 'Performances/Warm Pad.oscairn/performance.json').is_file()
        # Saving again keeps the ID and the folder.
        assert rpc('library.save')['id'] == pad and len(list((home / 'Performances').iterdir())) == 1
        # Save with a name renames it in place: the same ID and folder.
        assert rpc('library.save', {'name': 'Warm Pad'})['id'] == pad
        # Save As New is a copy with its own ID and folder.
        copy = rpc('library.save', {'name': 'Warm Pad 2', 'new': True})['id']
        assert copy != pad and rpc('library.query')['total'] == 2
        assert (home / 'Performances/Warm Pad 2.oscairn/performance.json').is_file()
        rpc('library.load', {'id': pad})
        assert rpc('performance.info')['id'] == pad
        manifest = home / 'Performances/Warm Pad 2.oscairn'
        for f in sorted(manifest.rglob('*'), reverse=True):
            f.unlink() if f.is_file() else f.rmdir()
        manifest.rmdir()
        assert rpc('library.rescan') == {'items': 1}
        # Info set but not saved is gone when the saved one is opened again.
        rpc('performance.setInfo', {'name': 'Bright Lead', 'description': '', 'tags': ['lead']})
        rpc('performance.load', {'directory': str(home / 'Performances/Warm Pad.oscairn')})
        assert rpc('performance.info')['name'] == 'Warm Pad'

        everything = rpc('library.query')
        assert everything['total'] == 1 and everything['items'][0]['name'] == 'Warm Pad' and everything['items'][0]['color'] == '#3a7bd5'
        assert everything['tags'] == [{'tag': 'pad', 'count': 1}, {'tag': 'warm', 'count': 1}]
        assert rpc('library.query', {'text': 'STRINGS'})['total'] == 1
        assert rpc('library.query', {'tags': ['pad', 'lead']})['total'] == 0
        assert rpc('library.query', {'favourites': True})['total'] == 0
        rpc('library.favourite', {'id': pad, 'favourite': True})
        assert rpc('library.query', {'favourites': True})['items'][0]['favourite'] is True
        assert rpc('library.query', {'recent': True})['items'][0]['recent'] == 1
        rpc('library.favourite', {'id': 'missing', 'favourite': True}, True)
        rpc('library.load', {'id': 'missing'}, True)
        rpc('library.query', {'limit': 0}, True)
        rpc('performance.setInfo', {'name': 'x', 'description': '', 'tags': ['']}, True)
        rpc('performance.setInfo', {'name': 'x', 'description': '', 'tags': [], 'color': 'blue'}, True)

        # A file changed on disk is read again; one the library cannot read is skipped, and the cache lives.
        manifest = home / 'Performances/Warm Pad.oscairn/performance.json'
        value = json.loads(manifest.read_text())
        value['info']['tags'] = ['pad', 'dark']
        manifest.write_text(json.dumps(value))
        (home / 'Performances/Broken.oscairn').mkdir()
        (home / 'Performances/Broken.oscairn/performance.json').write_text('{')
        assert rpc('library.rescan') == {'items': 1}
        assert rpc('library.query', {'tags': ['dark']})['total'] == 1
        assert json.loads((home / 'library/index.json').read_text())['items'][0]['tags'] == ['pad', 'dark']
        rpc('library.load', {'id': pad})
        assert rpc('performance.info')['tags'] == ['pad', 'dark']
        # A part saved as a Sound (RFC 0021), listed apart, loaded into a part and as a new part.
        part = rpc('parts.add', {'name': 'Lead'})['part']
        rpc('parts.insert', {'part': part, 'index': 0, 'node': 'synth'})
        rpc('graph.commit')
        tone = rpc('controls.add', {'name': 'Tone', 'part': part})['control']
        sound = rpc('parts.saveSound', {'part': part, 'name': 'Lead Sound'})
        # The harness wires its synth to the input and output by hand: those wires cross the part's edge.
        assert sound['left'] == ['the input to synth', 'synth to the output'] and (home / 'Sounds/Lead Sound.osound/sound.json').is_file()
        assert [i['id'] for i in rpc('library.query', {'kind': 'sound'})['items']] == [sound['id']]
        assert rpc('library.query', {'kind': 'performance'})['total'] == 1
        added = rpc('parts.fromSound', {'id': sound['id']})['part']
        loaded = rpc('parts.loadSound', {'part': part, 'id': sound['id']})
        assert loaded == {'accepted': True, 'flagged': ['the wire from the input to synth was removed', 'the wire from synth to the output was removed']}, loaded
        graph = rpc('graph.get')
        assert [p['name'] for p in graph['parts'] if p['id'] in (part, added)] == ['Lead Sound', 'Lead Sound']
        source = next(p for p in graph['parts'] if p['id'] == part)['from']
        assert source == {'sound': sound['id'], 'revision': sound['revision'], 'name': 'Lead Sound', 'modified': False}, source
        rpc('graph.commit')
        forked = rpc('parts.saveSound', {'part': part, 'name': 'Lead Fork', 'new': True})
        assert forked['id'] != sound['id']
        assert next(p for p in rpc('graph.get')['parts'] if p['id'] == part)['from']['sound'] == forked['id']
        owners = sorted(c['part'] for c in graph['controls'] if c['name'] == 'Tone')
        assert owners == sorted([part, added]) and tone not in [c['id'] for c in graph['controls']]
        rpc('parts.loadSound', {'part': part, 'id': pad}, True)
        rpc('library.query', {'kind': 'preset'}, True)
        # Importing a Performance (RFC 0021): what it holds, then one chosen part.
        exported = home / 'export.oscairn'
        rpc('performance.save', {'directory': str(exported)})
        contents = rpc('performance.contents', {'directory': str(exported)})
        held = contents['parts']
        assert contents['scenes'] == []
        assert [p['name'] for p in held] == [p['name'] for p in rpc('graph.get')['parts']] and not any(p['bus'] for p in held)
        count = len(rpc('graph.get')['parts'])
        imported = rpc('performance.import', {'directory': str(exported), 'parts': [held[0]['id']]})
        assert len(imported['parts']) == 1 and len(rpc('graph.get')['parts']) == count + 1, imported
        rpc('performance.import', {'directory': str(exported), 'parts': [9999]}, True)
        user = json.loads((home / 'library/user.json').read_text())
        assert user['favourites'] == [pad] and user['recent'][-2:] == [pad, copy]  # a gone item's stays, for if it returns
    finally:
        p.stdin.close()
        p.wait(timeout=10)
    assert p.returncode == 0
print('ok: library info, save and load by ID, query, favourites, recents, rescan, Sounds and importing')
