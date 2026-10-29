# SPDX-License-Identifier: AGPL-3.0-or-later
"""Exercise the on-disk contract through RPC, including independent process reopen."""
import copy
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


def start():
    return subprocess.Popen([sys.argv[1], sys.argv[2], '--stdio'], stdin=subprocess.PIPE,
                            stdout=subprocess.PIPE, text=True, encoding='utf-8')


def rpc(method, params=None, expect_error=False):
    p.stdin.write(json.dumps({'jsonrpc': '2.0', 'id': 1, 'method': method, 'params': params or {}}) + '\n')
    p.stdin.flush()
    response = json.loads(p.stdout.readline())
    assert ('error' in response) == expect_error, response
    return response


def render():
    p.stdin.write('!drain\n')
    p.stdin.flush()
    assert json.loads(p.stdout.readline()) == {}


p = start()
try:
    with tempfile.TemporaryDirectory(prefix='oscairn-performance-') as temporary:
        root = Path(temporary)
        saved = root / 'saved'
        # Save all eight native contexts, including a selection not rendered yet.
        for i in range(1, 8):
            context = rpc('sounds.prepare', {'node': 'synth', 'name': f'Patch {i}',
                                             'parameters': [{'id': 1, 'value': i / 10}]})['result']['context']
            rpc('sounds.select', {'node': 'synth', 'context': context, 'channel': i})
        rpc('performance.save', {'directory': str(saved)})
        manifest = json.loads((saved / 'performance.json').read_text())
        assert manifest['format'] == 'oscairn.performance' and manifest['version'] == 1
        assert manifest['nodes'][0]['key'] == 'node-1'
        assert manifest['nodes'][0]['plugin']['id'] == 'org.oscairn.basic-synth'
        schema = Path(__file__).resolve().parents[3] / 'docs/performance.schema.json'
        try:
            import jsonschema
        except ImportError:
            print('JSON Schema validator not installed; parser/roundtrip checks still run')
        else:
            jsonschema.Draft202012Validator.check_schema(json.loads(schema.read_text()))
            jsonschema.validate(manifest, json.loads(schema.read_text()))
            protocol = json.loads((schema.parent / 'control-protocol.schema.json').read_text())
            jsonschema.Draft202012Validator.check_schema(protocol)
            for method in ['performance.save', 'performance.load']:
                jsonschema.validate({'jsonrpc':'2.0','id':1,'method':method,
                                     'params':{'directory':'/tmp/example'}}, protocol)
            for key in ['saved', 'loaded']:
                jsonschema.validate({'jsonrpc':'2.0','id':1,'result':{key:True}}, protocol)
        # The folder remains usable after the saving engine exits and the folder moves.
        p.stdin.close()
        assert p.wait(timeout=10) == 0
        p = start()
        moved = root / 'moved'
        saved.rename(moved)
        rpc('performance.load', {'directory': str(moved)})
        render()
        sounds = rpc('sounds.get', {'node': 'synth'})['result']['contexts']
        assert len(sounds) == 8
        for i in range(1, 8):
            assert sounds[i]['name'] == f'Patch {i}' and sounds[i]['selectedChannels'] == 1 << i
        again = root / 'again'
        rpc('performance.save', {'directory': str(again)})
        assert json.loads((again / 'performance.json').read_text()) == manifest
        before = (again / 'performance.json').read_bytes()
        rpc('performance.save', {'directory': str(again)}, True)
        assert (again / 'performance.json').read_bytes() == before

        mutations = []
        def changed(edit):
            value = copy.deepcopy(manifest)
            edit(value)
            mutations.append(json.dumps(value))
        changed(lambda m: m.update(version=2))
        changed(lambda m: m.update(unknown=True))
        changed(lambda m: m['nodes'][0].update(key='../escape'))
        changed(lambda m: m['nodes'][0]['plugin'].update(version='incompatible'))
        changed(lambda m: m['nodes'][0]['plugin'].update(id='missing'))
        changed(lambda m: m['nodes'][0]['outputs'][0].update(channels=1))
        changed(lambda m: m['audio'].append(copy.deepcopy(m['audio'][0])))
        changed(lambda m: m['notes'].append(copy.deepcopy(m['notes'][0])))
        changed(lambda m: m['nodes'].append(copy.deepcopy(m['nodes'][0])))
        changed(lambda m: m['audio'][0]['from'].update(node='missing'))
        changed(lambda m: m['notes'].append({'from':'node-1','to':'node-1'}))
        mutations += ['{"version":1,"version":1}', '[' * 33 + '0' + ']' * 33, '{"format":']
        live = rpc('graph.get')['result']
        for i, text in enumerate(mutations):
            bad = root / f'bad-{i}'
            shutil.copytree(moved, bad)
            (bad / 'performance.json').write_text(text)
            rpc('performance.load', {'directory': str(bad)}, True)
            assert rpc('graph.get')['result'] == live
        bad = root / 'bad-state'
        shutil.copytree(moved, bad)
        (bad / 'nodes/node-1/state/state.clap').write_text('oscairn-synth-1\n1 "invalid" NaN')
        rpc('performance.load', {'directory': str(bad)}, True)
        assert rpc('graph.get')['result'] == live
        (bad / 'performance.json').write_bytes(b'{\"format\":\"\xff\"}')
        rpc('performance.load', {'directory': str(bad)}, True)
        rpc('performance.load', {'directory': str(moved), 'extra': 1}, True)
        rpc('performance.save', {}, True)
        assert not list(root.glob('.oscairn-state-*')), 'abandoned staging folders'
    print('ok: Performance contract, all native contexts, process restart, schema and malformed input')
finally:
    if p.poll() is None:
        p.stdin.close()
        p.wait(timeout=10)
    assert p.returncode == 0
