# SPDX-License-Identifier: AGPL-3.0-or-later
"""Exercise the on-disk contract through RPC, including independent process reopen."""
import copy
import json
import os
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
        assert manifest['format'] == 'oscairn.performance' and manifest['version'] == 11
        assert manifest['nodes'][0]['key'] == 'node-1'
        assert manifest['nodes'][0]['plugin']['id'] == 'org.oscairn.basic-synth'
        schema = Path(__file__).resolve().parents[3] / 'docs/performance.schema.json'
        try:
            import jsonschema
        except ImportError:
            if os.environ.get('CI') and sys.platform.startswith('linux'):
                raise  # Linux CI installs it, so the schemas are always checked there
            print('JSON Schema validator not installed; parser/roundtrip checks still run')
        else:
            jsonschema.Draft202012Validator.check_schema(json.loads(schema.read_text()))
            jsonschema.validate(manifest, json.loads(schema.read_text()))
            protocol = json.loads((schema.parent / 'control-protocol.schema.json').read_text())
            jsonschema.Draft202012Validator.check_schema(protocol)
            for method in ['performance.save', 'performance.load', 'performance.prepare']:
                jsonschema.validate({'jsonrpc':'2.0','id':1,'method':method,
                                     'params':{'directory':'/tmp/example'}}, protocol)
            for key in ['saved', 'loaded', 'prepared']:
                jsonschema.validate({'jsonrpc':'2.0','id':1,'result':{key:True}}, protocol)
            # A live graph.get with a part and its wires (RFC 0011).
            part = rpc('parts.add', {'name': 'Check'})['result']['part']
            split = rpc('parts.addSplit', {'part': part, 'index': 0})['result']['split']
            rpc('parts.insert', {'part': part, 'index': 0, 'node': 'synth', 'split': split, 'lane': 1})
            rpc('parts.lane', {'part': part, 'split': split, 'lane': 1, 'level': 0.5, 'zones': [{'keys': [60, 127]}]})
            # A scene holding a plugin off, with its scope, and a mapping off in it (RFC 0011).
            synth = next(n['id'] for n in rpc('graph.get')['result']['nodes'] if n['name'] == 'synth')
            rpc('graph.enable', {'node': synth, 'enabled': False})
            scene = rpc('scenes.add', {'name': 'Quiet'})['result']['scene']
            rpc('scenes.scope', {'scene': scene, 'scope': ['enabled', 'mappings']})
            control = rpc('controls.add', {'name': 'Knob'})['result']['control']
            fader, power = {'kind': 'db', 'min': -96, 'max': 0}, {'kind': 'power', 'exponent': 2}
            rpc('controls.targets', {'control': control, 'targets': [{'kind': 'parameter', 'node': synth, 'param': 1, 'offIn': [scene], 'curve': fader},
                                                                     {'kind': 'parameter', 'node': synth, 'param': 1, 'curve': power}]})
            for bad in [{'kind': 'db', 'min': 0, 'max': -6}, {'kind': 'db', 'min': -200, 'max': 0}, {'kind': 'power', 'exponent': 0},
                        {'kind': 'power'}, {'kind': 'db', 'min': -96, 'max': 0, 'exponent': 2}, 'square']:
                rpc('controls.targets', {'control': control, 'targets': [{'kind': 'parameter', 'node': synth, 'param': 1, 'curve': bad}]}, True)
            rpc('graph.enable', {'node': synth, 'enabled': True})
            rpc('master.set', {'level': 0.8, 'pan': 0.25})
            rpc('graph.role', {'node': synth, 'role': 'insertion-a'})
            rpc('graph.role', {'node': synth, 'role': 'chorus'}, True)
            jsonschema.validate({'jsonrpc': '2.0', 'id': 1, 'method': 'graph.role', 'params': {'node': synth, 'role': None}}, protocol)
            rpc('master.set', {'level': 3, 'pan': 0}, True)
            jsonschema.validate({'jsonrpc': '2.0', 'id': 1, 'method': 'master.set', 'params': {'level': 0.8, 'pan': 0.25}}, protocol)
            graph = rpc('graph.get')['result']
            assert graph['master'] == {'level': 0.8, 'pan': 0.25} and graph['scenes'][0]['master'] == {'level': 1, 'pan': 0}
            assert next(n for n in graph['nodes'] if n['id'] == synth)['role'] == "insertion-a"
            assert graph['scenes'][0]['off'] == [synth] and graph['scenes'][0]['scope'] == ['enabled', 'mappings'] and graph['controls'][0]['targets'][0]['offIn'] == [scene]
            assert graph['controls'][0]['targets'][0]['curve'] == fader and graph['controls'][0]['targets'][1]['curve'] == power
            assert graph['parts'][0]['name'] == 'Check' and any(w['owner'] == 'part' for w in graph['wires'])
            assert graph['parts'][0]['chain'] == [{'split': split}] and graph['parts'][0]['splits'][0]['lanes'][1]['level'] == 0.5
            jsonschema.validate({'jsonrpc':'2.0','id':1,'result':graph}, protocol)
            for method, params in [('parts.addSplit', {'part': part, 'index': 0}), ('parts.lane', {'part': part, 'split': split, 'lane': 0, 'mute': True}),
                                   ('parts.addLane', {'part': part, 'split': split}), ('parts.removeLane', {'part': part, 'split': split, 'lane': 0}),
                                   ('parts.removeSplit', {'part': part, 'split': split})]:
                jsonschema.validate({'jsonrpc':'2.0','id':1,'method':method,'params':params}, protocol)
            jsonschema.validate({'jsonrpc':'2.0','id':1,'result':{'split':1}}, protocol)
            # Saved with the split, the manifest matches the file schema.
            rpc('graph.commit')
            rpc('performance.save', {'directory': str(saved) + '-split'})
            split_manifest = json.loads((Path(str(saved) + '-split') / 'performance.json').read_text())
            jsonschema.validate(split_manifest, json.loads(schema.read_text()))
            assert [t['curve'] for t in split_manifest['controls'][0]['targets']] == [fader, power]
            assert split_manifest['master'] == {'level': 0.8, 'pan': 0.25}
            assert split_manifest['nodes'][0]['role'] == 'insertion-a'
            rpc('graph.role', {'node': synth, 'role': None})
            rpc('master.set', {'level': 1, 'pan': 0})
            rpc('parts.remove', {'part': part})
        # The folder remains usable after the saving engine exits and the folder moves.
        p.stdin.close()
        assert p.wait(timeout=10) == 0
        p = start()
        moved = root / 'moved'
        saved.rename(moved)
        # Prepared ahead, then loaded: the load only publishes it.
        assert rpc('performance.prepare', {'directory': str(moved)})['result'] == {'prepared': True}
        render()
        rpc('performance.load', {'directory': str(moved)})
        render()
        sounds = rpc('sounds.get', {'node': 'synth'})['result']['contexts']
        assert len(sounds) == 8
        for i in range(1, 8):
            assert sounds[i]['name'] == f'Patch {i}' and sounds[i]['selectedChannels'] == 1 << i
        again = root / 'again'
        rpc('performance.save', {'directory': str(again)})
        resaved = json.loads((again / 'performance.json').read_text())
        assert resaved['info']['parent'] == manifest['info']['revision'] != resaved['info']['revision']
        def without_revision(m):
            m = copy.deepcopy(m)
            del m['info']['revision'], m['info']['parent']
            return m
        assert without_revision(resaved) == without_revision(manifest)
        before = (again / 'performance.json').read_bytes()
        rpc('performance.save', {'directory': str(again)}, True)
        assert (again / 'performance.json').read_bytes() == before

        mutations = []
        def changed(edit):
            value = copy.deepcopy(manifest)
            edit(value)
            mutations.append(json.dumps(value))
        changed(lambda m: m.update(version=2))
        changed(lambda m: m.pop('info'))
        changed(lambda m: m['info'].update(id=''))
        changed(lambda m: m['info'].update(extra=1))
        changed(lambda m: m['info'].update(tags=['']))
        changed(lambda m: m['info'].update(color='#12345'))
        changed(lambda m: m['info'].pop('color'))
        changed(lambda m: m.update(unknown=True))
        changed(lambda m: m['master'].update(level=3))
        changed(lambda m: m.pop('master'))
        changed(lambda m: m['nodes'][0].update(key='../escape'))
        changed(lambda m: m['nodes'][0]['outputs'][0].update(channels=1))
        changed(lambda m: m['wires'].append({**m['wires'][0], 'owner': 'part', 'part': 99}))
        changed(lambda m: m['wires'][0].update(kind='cv'))
        changed(lambda m: m['wires'][0].update(level=-1))
        changed(lambda m: m['wires'][0].update(channels=[[0]]))
        changed(lambda m: m['nodes'].append(copy.deepcopy(m['nodes'][0])))
        changed(lambda m: m['wires'][0].update(**{'from': 'missing'}))
        changed(lambda m: m['parts'].append({'id': 1, 'name': 'Twice', 'input': {'kind': 'notes', 'port': 0}, 'chain': ['node-1', 'node-1'], 'output': 0}))
        changed(lambda m: m.update(audio=[]))
        mutations += ['{"version":1,"version":1}', '[' * 33 + '0' + ']' * 33, '{"format":']
        live = rpc('graph.get')['result']
        for i, text in enumerate(mutations):
            bad = root / f'bad-{i}'
            shutil.copytree(moved, bad)
            (bad / 'performance.json').write_text(text)
            rpc('performance.load', {'directory': str(bad)}, True)
            assert rpc('graph.get')['result'] == live
        # A plugin not here, or of another version, opens as a stand-in that says so (RFC 0006).
        for i, change in enumerate([lambda m: m['nodes'][0]['plugin'].update(version='incompatible'), lambda m: m['nodes'][0]['plugin'].update(id='missing')]):
            stand = root / f'stand-in-{i}'
            shutil.copytree(moved, stand)
            manifest = json.loads((moved / 'performance.json').read_text())
            change(manifest)
            (stand / 'performance.json').write_text(json.dumps(manifest, separators=(',', ':')))
            rpc('performance.load', {'directory': str(stand)})
            failures = rpc('session.status')['result']['failures']
            assert len(failures) == 1 and 'not here' in failures[0]['reason'], failures
        rpc('performance.load', {'directory': str(moved)})
        live = rpc('graph.get')['result']
        bad = root / 'bad-state'
        shutil.copytree(moved, bad)
        (bad / 'nodes/node-1/state/state.clap').write_text('oscairn-synth-2\n1 "invalid" NaN')
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
