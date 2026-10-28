# SPDX-License-Identifier: AGPL-3.0-or-later
"""Opt-in live-device smoke test: python3 desktop_lifecycle.py ENGINE UI_BINARY.

Opens the default audio output, mutes the demo synth, exercises a KICK UI crash
and reconnect, and saves screenshots/logs in a private temporary directory.
Only processes started by this test are stopped. Requires a desktop session,
an available audio output, and permission to bind Unix sockets.
"""
import json, pathlib, socket, subprocess, tempfile, time

import argparse
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('engine', type=pathlib.Path)
parser.add_argument('ui', type=pathlib.Path)
args = parser.parse_args()
root = pathlib.Path(__file__).resolve().parents[3]
engine, ui = args.engine.resolve(), args.ui.resolve()
artifacts = pathlib.Path(tempfile.mkdtemp(prefix='oscairn-live-', dir='/tmp'))
endpoint = artifacts/'control.sock'
children = []
handles = []
connection = None
stream = None
ident = 0

def launch(binary, args, log):
    handle = open(artifacts/log, 'w')
    handles.append(handle)
    process = subprocess.Popen([str(binary), *args], cwd=root, stdout=handle, stderr=subprocess.STDOUT)
    children.append(process)
    return process

def rpc(method, params=None):
    global ident
    ident += 1
    request = {'jsonrpc':'2.0', 'id':ident, 'method':method}
    if params is not None:
        request['params'] = params
    stream.write((json.dumps(request)+'\n').encode())
    stream.flush()
    response = json.loads(stream.readline())
    assert response.get('id') == ident, response
    assert 'error' not in response, response
    return response['result']

def snapshot(name):
    process = launch(ui, ['--connect',str(endpoint),'--layout','kick','--snapshot',str(artifacts/name),'--play','0.7'],name+'.log')
    assert process.wait(timeout=15) == 0, (artifacts/(name+'.log')).read_text()
    assert (artifacts/name).stat().st_size > 1000

try:
    helper = launch(engine, ['--socket',str(endpoint)],'engine.log')
    deadline = time.monotonic()+15
    while not endpoint.exists():
        assert helper.poll() is None, (artifacts/'engine.log').read_text()
        if time.monotonic()>deadline:
            raise TimeoutError('engine socket startup')
        time.sleep(.05)
    connection = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    connection.settimeout(5)
    connection.connect(str(endpoint))
    stream = connection.makefile('rwb')
    assert rpc('session.hello')['protocolVersion'] == 1
    status = rpc('session.status')
    assert status['audioDevice'] and status['sampleRate'] > 0, status
    # Exercise the live device callback silently; no listening response is needed.
    rpc('params.set', {'values':[{'node':'synth','param':2,'value':0}]})
    rpc('transport.set', {'playing':True})
    snapshot('kick-before.png')
    controller = launch(ui, ['--connect',str(endpoint),'--layout','kick'], 'crashed-ui.log')
    time.sleep(1)
    assert controller.poll() is None, (artifacts/'crashed-ui.log').read_text()
    before = rpc('transport.get')['beat']
    controller.kill()
    assert controller.wait(timeout=5) == -9
    time.sleep(.4)
    after = rpc('transport.get')['beat']
    assert helper.poll() is None and after > before, (before, after)
    rpc('transport.set', {'tempo':143})
    snapshot('kick-after.png')
    state = rpc('transport.get')
    assert state['playing'] and state['tempo'] == 143 and state['beat'] > after, state
    rpc('transport.set', {'playing':False})
    print(json.dumps({'result':'passed','engine_pid':helper.pid,'audio_status':status,'beat_before_ui_crash':before,'beat_after_ui_crash':after,'reopened_state':state,'artifacts':str(artifacts)},indent=2),flush=True)
finally:
    if stream is not None:
        stream.close()
    if connection is not None:
        connection.close()
    for process in reversed(children):
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill(); process.wait(timeout=5)
    for handle in handles:
        handle.close()
    print('Logs and screenshots:',artifacts,flush=True)

assert helper.returncode == 0, (artifacts/"engine.log").read_text()
assert not endpoint.exists(), "helper did not remove its socket on orderly shutdown"
