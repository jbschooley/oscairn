# SPDX-License-Identifier: AGPL-3.0-or-later
"""Opt-in live-device test of automatic desktop helper ownership.

Pass the UI executable. --interactive-recovery waits up to 60 seconds for the
Restart engine button to be clicked; otherwise reopening the UI starts recovery.
The synth is muted before playback. Only this test's UI/helper processes are stopped.
"""
import argparse
import json
import os
from pathlib import Path
import signal
import socket
import subprocess
import tempfile
import time

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('ui', type=Path)
parser.add_argument('--interactive-recovery', action='store_true')
args = parser.parse_args()
ui = args.ui.resolve()
assert ui.with_name('oscairn-engine').is_file(), 'helper must be bundled beside UI'
assert (ui.parent/'plugins/oscairn-basic-synth.clap').is_file(), 'demo plugins must be bundled'
artifacts = Path(tempfile.mkdtemp(prefix='oscairn-managed-', dir='/tmp'))
runtime = artifacts/'runtime'
runtime.mkdir(mode=0o700)
env = dict(os.environ, OSCAIRN_RUNTIME_DIR=str(runtime))
children, logs = [], []
helper_pid = None
endpoint = None
connection = stream = None
ident = 0

def launch(name, snapshot=False):
    log = open(artifacts/(name+'.log'), 'w'); logs.append(log)
    command = [str(ui), '--layout', 'kick']
    if snapshot:
        command += ['--snapshot', str(artifacts/(name+'.png'))]
    child = subprocess.Popen(command, env=env, stdout=log, stderr=subprocess.STDOUT)
    children.append(child)
    return child

def wait_for(condition, timeout=15):
    deadline = time.monotonic()+timeout
    while time.monotonic()<deadline:
        value = condition()
        if value:
            return value
        time.sleep(.03)
    raise TimeoutError('condition not reached; see '+str(artifacts))

def pid():
    try:
        return int(Path(str(endpoint)+'.lock').read_text().strip())
    except (FileNotFoundError, ValueError):
        return None

def disconnect():
    global connection, stream
    if stream is not None: stream.close()
    if connection is not None: connection.close()
    connection = stream = None

def connect():
    global connection, stream
    disconnect()
    connection = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    connection.settimeout(5)
    connection.connect(str(endpoint))
    stream = connection.makefile('rwb')

def ready():
    try:
        connect()
        return True
    except (FileNotFoundError, ConnectionRefusedError):
        return False

def rpc(method, params=None):
    global ident
    ident += 1
    request = {'jsonrpc':'2.0','id':ident,'method':method}
    if params is not None: request['params'] = params
    stream.write((json.dumps(request)+'\n').encode()); stream.flush()
    response = json.loads(stream.readline())
    assert response.get('id')==ident and 'error' not in response, response
    return response['result']

def finish_snapshot(child, name):
    assert child.wait(timeout=15)==0, (artifacts/(name+'.log')).read_text()
    assert (artifacts/(name+'.png')).stat().st_size>1000

try:
    first = launch('initial', snapshot=True)
    def startup_endpoint():
        assert first.poll() is None, (artifacts/'initial.log').read_text()
        return next(iter(runtime.glob('*.sock')), None)
    endpoint = wait_for(startup_endpoint)
    helper_pid = wait_for(pid)
    connect()
    assert rpc('session.hello')['protocolVersion']==1
    rpc('params.set', {'values':[{'node':'synth','param':2,'value':0}]})
    rpc('transport.set', {'tempo':137,'playing':True})
    finish_snapshot(first, 'initial')
    assert rpc('transport.get')['tempo']==137, 'UI exit stopped or replaced helper'
    reopened = launch('reopened', snapshot=True)
    finish_snapshot(reopened, 'reopened')
    assert pid()==helper_pid and rpc('transport.get')['playing'], 'reopening must attach to the same engine'
    crash = launch('crashed-ui')
    time.sleep(.8)
    assert crash.poll() is None, 'UI failed to start'
    before = rpc('transport.get')['beat']
    crash.kill(); assert crash.wait(timeout=5)==-9
    time.sleep(.2)
    assert rpc('transport.get')['beat']>before, 'audio callback stopped with UI'
    survivor = launch('recovery-ui')
    time.sleep(.8)
    assert survivor.poll() is None, 'recovery UI failed to start'
    dead_pid = helper_pid
    os.kill(dead_pid, signal.SIGKILL); helper_pid = None
    disconnect()
    time.sleep(2)
    assert survivor.poll() is None, 'UI exited with engine'
    if args.interactive_recovery:
        print(json.dumps({'awaiting':'Restart engine button', 'runtime':str(runtime), 'artifacts':str(artifacts)}), flush=True)
        helper_pid = wait_for(lambda: (p if (p := pid()) and p!=dead_pid else None), 60)
    else:
        recovered = launch('recovered', snapshot=True)
        helper_pid = wait_for(lambda: (p if (p := pid()) and p!=dead_pid else None))
        finish_snapshot(recovered, 'recovered')
    wait_for(ready)
    state = rpc('transport.get')
    assert not state['playing'] and state['tempo']==120, ('recovery must start fresh, without replay',state)
    rpc('params.set', {'values':[{'node':'synth','param':2,'value':0}]})
    rpc('transport.set', {'tempo':151,'playing':True})
    final = launch('final', snapshot=True)
    finish_snapshot(final, 'final')
    assert pid()==helper_pid and rpc('transport.get')['tempo']==151
    rpc('transport.set', {'playing':False})
    print(json.dumps({'result':'passed','original_engine':dead_pid,'recovered_engine':helper_pid,'artifacts':str(artifacts)},indent=2), flush=True)
finally:
    disconnect()
    for child in reversed(children):
        if child.poll() is None:
            child.terminate()
            try: child.wait(timeout=5)
            except subprocess.TimeoutExpired: child.kill(); child.wait(timeout=5)
    if helper_pid is None and endpoint is not None:
        helper_pid = pid()
    if helper_pid:
        try: os.kill(helper_pid, signal.SIGTERM)
        except ProcessLookupError: pass
    for log in logs: log.close()
    print('Logs/screenshots:', artifacts, flush=True)
if endpoint is not None:
    wait_for(lambda: not endpoint.exists(), 5)
