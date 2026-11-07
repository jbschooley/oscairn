# SPDX-License-Identifier: AGPL-3.0-or-later
"""Installing from a package repository (RFC 0031 step 1), through RPC.

usage: repository.py RPC-SERVER PLUGIN OSCAIRN-SIGN FIXTURE.clap BUILD-INDEX.py

Makes throwaway keys, packs plugin packages with oscairn-sign, builds a repository with the
package repository template's index builder, signs its root, and installs from it as a folder:
an untrusted root, code from an unknown publisher, an incompatible build, a version needing a
newer system, a tampered blob, a root whose signature does not verify and an older root are each
refused, leaving nothing behind. An unpacked package folder installs, and one with a changed
file is refused.
"""
import hashlib
import json
import os
import platform as host
import shutil
import subprocess
import sys
import tempfile
import time
import zipfile
from pathlib import Path

try:
    import jsonschema
    protocol = json.loads((Path(__file__).resolve().parents[3] / 'docs/control-protocol.schema.json').read_text())
    request_validator = jsonschema.Draft202012Validator({**protocol, '$ref': '#/$defs/Request'})
except ImportError:
    jsonschema = None
    print('JSON Schema validator not installed; requests are not checked against the schema')

server, plugin, tool, fixture, builder = (Path(a).resolve() for a in sys.argv[1:6])
machine = {'arm64': 'arm64', 'aarch64': 'arm64', 'x86_64': 'x86_64', 'AMD64': 'x86_64'}[host.machine()]
here = ('macos' if sys.platform == 'darwin' else 'windows' if sys.platform == 'win32' else 'linux') + '-' + machine
elsewhere = 'linux-arm64' if here != 'linux-arm64' else 'macos-arm64'
failures = 0


def check(ok, what):
    global failures
    print(('ok   ' if ok else 'FAIL ') + what)
    failures += not ok


def run(*command):
    out = subprocess.run([str(c) for c in command], capture_output=True, text=True, timeout=300)
    assert out.returncode == 0, (command, out.stderr)
    return out.stdout


def rpc(method, params=None, expect_error=False):
    request = {'jsonrpc': '2.0', 'id': 1, 'method': method, 'params': params or {}}
    if jsonschema and not expect_error:
        request_validator.validate(request)
    p.stdin.write(json.dumps(request) + '\n')
    p.stdin.flush()
    response = json.loads(p.stdout.readline())
    assert ('error' in response) == expect_error, response
    return response.get('error') if expect_error else response.get('result')


def job(method, params):
    """Starts a package job and asks about it until it ends."""
    number = rpc(method, params)['job']
    for _ in range(600):
        state = rpc('packages.job', {'job': number})
        if state['state'] != 'running':
            return state
        time.sleep(0.05)
    raise TimeoutError(method)


with tempfile.TemporaryDirectory(prefix='oscairn-repository-') as temporary:
    temp = Path(temporary)
    keys = {}
    for name in ('publisher', 'root', 'index', 'other'):
        _, key_id, public = run(tool, 'key', temp / f'{name}.key').split()
        keys[name] = (key_id, public)

    # Plugin packages, each build and archive signed by the publisher: the synth for this
    # platform at 1.0.0 and 2.0.0 (which will need a newer system), and another plugin whose only
    # build is for another platform.
    repo = temp / 'submissions'
    shutil.copytree(builder.parents[1], repo)
    releases = []

    def package(plugin_id, version, platform, requires=None):
        build = temp / 'builds' / plugin_id / version / platform / 'Synth.clap'
        build.parent.mkdir(parents=True)
        shutil.copyfile(fixture, build)
        run(tool, 'build', temp / 'publisher.key', 'clap', plugin_id, version, platform, build)
        folder = repo / 'packages' / plugin_id
        folder.mkdir(parents=True, exist_ok=True)
        olib = folder / f'{version}.olib'
        run(tool, 'plugin-package', '--key', temp / 'publisher.key', olib, 'clap', plugin_id, version, 'Test Synth', f'{platform}={build}')
        data = olib.read_bytes()
        record = {'format': 'oscairn.repository-release', 'version': 1, 'id': plugin_id, 'release': version, 'publisher': 'org.kick',
                  'summary': 'A test synth', 'license': 'MIT', 'tags': ['synth'], 'platforms': [platform],
                  'archive': {'path': olib.name, 'sha256': hashlib.sha256(data).hexdigest(), 'size': len(data)}, 'yanked': None}
        if requires:
            record['requires'] = {'oscairn': requires}
        (folder / f'{version}.json').write_text(json.dumps(record))
        releases.append(olib)
        return olib

    synth = package('org.kick.test.synth', '1.0.0', here)
    package('org.kick.test.synth', '2.0.0', here, requires='99')
    package('org.kick.other', '1.0.0', elsewhere)
    (temp / 'index-key.pub').write_text(keys['index'][1] + '\n')
    run(tool, 'file', temp / 'root.key', temp / 'index-key.pub', temp / 'index-key.sig')
    config = json.loads((repo / 'repository-config.json').read_text())
    config.update(repository='org.example.test', name='Test repository', rootKey=keys['root'][1],
                  indexKey={'key': keys['index'][1], 'signature': json.loads((temp / 'index-key.sig').read_text())[0]['signature']})
    (repo / 'repository-config.json').write_text(json.dumps(config))
    (repo / 'publishers' / 'org.kick.json').write_text(json.dumps({
        'format': 'oscairn.repository-publisher', 'version': 1, 'id': 'org.kick', 'name': 'KICK', 'key': keys['publisher'][1],
        'verification': 'https://kick.example/.well-known/oscairn-publisher.json'}))

    def site(name, serial):
        """The repository as the index builder makes it, signed, with its blobs under blobs/<ab>/."""
        out = temp / name
        built = subprocess.run([sys.executable, '-I', builder, '--repo', repo, '--oscairn-sign', tool, '--cache', temp / 'cache',
                                '--out', out, '--serial', str(serial)], capture_output=True, text=True, timeout=300)
        assert built.returncode == 0, built.stderr
        run(tool, 'file', temp / 'index.key', out / 'repository.json', out / 'repository.sig')
        for blob in (out / 'blobs.txt').read_text().split():
            (out / 'blobs' / blob[:2]).mkdir(parents=True, exist_ok=True)
            shutil.copyfile(temp / 'cache' / blob, out / 'blobs' / blob[:2] / blob)
        return out

    good = site('site', 7)
    home = temp / 'home'
    home.mkdir()
    p = subprocess.Popen([server, plugin, '--stdio'], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, encoding='utf-8',
                         env={**os.environ, 'OSCAIRN_DATA_HOME': str(home)})

    def leftovers():
        return sorted(e.name for e in home.iterdir() if e.name.startswith('.install-'))

    def store():
        return rpc('plugins.list')['plugins']

    try:
        # Not trusted yet: refused, naming its root key so the user can choose to trust it.
        state = job('packages.list', {'source': str(good)})
        check(state['state'] == 'failed' and 'not a repository this device trusts' in state['error'] and keys['root'][0] in state['error'] and state['rootKey'] == keys['root'][1],
              'an untrusted repository is refused, naming its root key: ' + state['error'])
        rpc('trust.set', {'key': keys['root'][1], 'name': 'Test repository', 'scope': ['repositories']})
        state = job('packages.list', {'source': str(good)})
        listed = {x['id']: x for x in state['packages']}
        check(state['state'] == 'done' and state['repository'] == 'org.example.test' and state['serial'] == 7 and set(listed) == {'org.kick.test.synth', 'org.kick.other'},
              'once trusted, it lists what it offers: ' + json.dumps(state)[:300])
        versions = {v['version']: v for v in listed['org.kick.test.synth']['versions']}
        check(versions['1.0.0']['incompatible'] == '' and versions['1.0.0']['installed'] >= fixture.stat().st_size and 'system update' in versions['2.0.0']['incompatible'],
              'each version says whether it fits this device: ' + json.dumps(versions))
        check('no build for this device' in listed['org.kick.other']['versions'][0]['incompatible'] and listed['org.kick.other']['publisherKey'] == keys['publisher'][0],
              'a package without a build for this device says so')

        # Code from a publisher not trusted for plugins needs a yes, as an archive's does.
        state = job('packages.install', {'source': str(good), 'id': 'org.kick.test.synth'})
        check(state['state'] == 'failed' and 'unknown source' in state['error'] and store() == [] and leftovers() == [],
              'code by an unknown key is refused without a yes, leaving nothing: ' + state['error'])
        rpc('trust.set', {'key': keys['publisher'][1], 'name': 'KICK', 'scope': ['plugins']})
        state = job('packages.install', {'source': str(good), 'id': 'org.kick.test.synth'})
        plugins = store()
        check(state['state'] == 'done' and state['version'] == '1.0.0' and state['installed']['code'][0]['status'] == 'trusted' and leftovers() == [],
              'the newest version this device can install installs: ' + json.dumps(state)[:300])
        check(len(plugins) == 1 and plugins[0]['id'] == 'org.kick.test.synth' and plugins[0]['platforms'] == [here], 'its build is in the store: ' + json.dumps(plugins))

        # Refused, each with its reason, installing nothing.
        state = job('packages.install', {'source': str(good), 'id': 'org.kick.other'})
        check(state['state'] == 'failed' and 'no build for this device' in state['error'] and len(store()) == 1, 'a build this device cannot run is not installed: ' + state['error'])
        state = job('packages.install', {'source': str(good), 'id': 'org.kick.test.synth', 'version': '2.0.0'})
        check(state['state'] == 'failed' and 'system update' in state['error'], 'a version needing a newer system is not installed: ' + state['error'])
        state = job('packages.install', {'source': str(good), 'id': 'org.example.none'})
        check(state['state'] == 'failed' and 'no package' in state['error'], 'a package it does not have: ' + state['error'])

        tampered = temp / 'tampered'
        shutil.copytree(good, tampered)
        blob = next(b for b in (tampered / 'blobs').rglob('*') if b.is_file() and b.read_bytes() == synth.read_bytes())
        data = bytearray(blob.read_bytes())
        data[len(data) // 2] ^= 1
        blob.write_bytes(bytes(data))
        rpc('plugins.remove', {'format': 'clap', 'id': 'org.kick.test.synth', 'version': '1.0.0'})
        state = job('packages.install', {'source': str(tampered), 'id': 'org.kick.test.synth'})
        check(state['state'] == 'failed' and 'not the one the index names' in state['error'] and store() == [] and leftovers() == [],
              'a tampered blob is refused, leaving nothing: ' + state['error'])
        blob.unlink()
        state = job('packages.install', {'source': str(tampered), 'id': 'org.kick.test.synth'})
        check(state['state'] == 'failed' and 'missing' in state['error'], 'a blob not on the drive is refused: ' + state['error'])

        forged = temp / 'forged'
        shutil.copytree(good, forged)
        root = json.loads((forged / 'repository.json').read_text())
        root['name'] = 'Forged'
        (forged / 'repository.json').write_text(json.dumps(root))
        state = job('packages.list', {'source': str(forged)})
        check(state['state'] == 'failed' and 'signature does not verify' in state['error'], 'a root whose signature does not verify is refused: ' + state['error'])
        run(tool, 'file', temp / 'other.key', forged / 'repository.json', forged / 'repository.sig')
        state = job('packages.list', {'source': str(forged)})
        check(state['state'] == 'failed' and 'not signed by its index key' in state['error'], 'a root signed by another key is refused: ' + state['error'])

        older = site('older', 6)
        state = job('packages.list', {'source': str(older)})
        check(state['state'] == 'failed' and 'older than one this device has seen' in state['error'], 'an older root is refused: ' + state['error'])

        # An unpacked package folder: the same rules as the archive; hidden files are skipped.
        unpacked = temp / 'unpacked'
        with zipfile.ZipFile(synth) as z:
            z.extractall(unpacked)
        (unpacked / '.DS_Store').write_text('desktop junk')
        contents = rpc('library.contents', {'file': str(unpacked)})
        check(contents['kind'] == 'plugin' and contents['code'][0]['status'] == 'trusted', 'an unpacked package folder is read: ' + json.dumps(contents)[:200])
        changed = temp / 'changed'
        shutil.copytree(unpacked, changed)
        bundle = next(f for f in changed.rglob('*.clap') if f.is_file())  # the fixture is one file
        bundle.write_bytes(bundle.read_bytes() + b'changed')
        error = rpc('library.install', {'file': str(changed)}, expect_error=True)
        check('not what the manifest says' in error['message'] and store() == [] and leftovers() == [], 'a folder package with a changed file is refused, leaving nothing: ' + error['message'])
        installed = rpc('library.install', {'file': str(unpacked)})
        check(installed['code'][0]['status'] == 'trusted' and len(store()) == 1 and leftovers() == [] and unpacked.is_dir() and any(unpacked.rglob('*.clap')),
              'and the unpacked one installs, read in place and left as it was')
    finally:
        p.stdin.close()
        p.wait(timeout=30)
sys.exit(1 if failures else 0)
