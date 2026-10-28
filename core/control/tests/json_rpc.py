# SPDX-License-Identifier: AGPL-3.0-or-later
"""Independent JSON decoder and protocol/error/queue contract checks."""
import json
import subprocess
import sys

proc = subprocess.Popen([sys.argv[1], sys.argv[2], '--stdio'], stdin=subprocess.PIPE,
                        stdout=subprocess.PIPE, text=True, encoding='utf-8')


def raw(text):
    proc.stdin.buffer.write((text.encode('utf-8') if isinstance(text, str) else text) + b'\n')
    proc.stdin.buffer.flush()
    line = proc.stdout.readline()
    assert line, f'driver exited: {proc.poll()}'
    return json.loads(line) if line.strip() else None


def rpc(method, params=None, ident=1):
    value = {'jsonrpc': '2.0', 'id': ident, 'method': method}
    if params is not None:
        value['params'] = params
    return raw(json.dumps(value))


def error(response, code, ident=1):
    assert response['jsonrpc'] == '2.0' and response['id'] == ident, response
    assert response['error']['code'] == code and 'result' not in response, response


def value(param):
    return rpc('params.get', {'values': [{'node': 'synth', 'param': param}]})['result']['values'][0]['value']


try:
    hello = rpc('session.hello')['result']
    assert hello['protocolVersion'] == 1 and 'notes.send' in hello['capabilities']
    assert 'graph.add' in hello['capabilities'] and 'subscribe' in hello['capabilities']
    for ident in [None, 0, -1, 2**63 + 7, 'hello\x00\n"\\', 'é🎹', 1.5]:
        assert rpc('transport.get', ident=ident)['id'] == ident
    # The numeric ID token is echoed without rounding, even beyond machine integers.
    assert raw('{"jsonrpc":"2.0","id":123456789012345678901234567890,"method":"session.hello"}')['id'] == 123456789012345678901234567890
    for bad in ['', '{', '{"a":1,}', '// nope', '{"x":NaN}', '{"x":01}', 'true false', '{"a":"\x01"}', '{"a":"\\ud800"}']:
        error(raw(bad), -32700, None)
    for bad in [b'\xff', b'"\xc0\xaf"', b'"\xed\xa0\x80"', b'"\xf4\x90\x80\x80"', b'"\xe2"']:
        error(raw(bad), -32700, None)
    complete = '{"jsonrpc":"2.0","id":"🎹","method":"session.hello"}'.encode('utf-8')
    for length in range(len(complete)):
        error(raw(complete[:length]), -32700, None)
    for bad in ['null', '1', '[]', '{}', '{"jsonrpc":"1.0","method":"session.hello"}',
                '{"jsonrpc":"2.0","id":true,"method":"session.hello"}',
                '{"jsonrpc":"2.0","method":"session.hello","params":null}',
                '{"jsonrpc":"2.0","jsonrpc":"2.0","method":"session.hello"}']:
        error(raw(bad), -32600, None)
    error(rpc('graph.unknown'), -32601)
    error(rpc('params.list', []), -32602)
    error(rpc('params.list', {'node': 100}), -32001)
    error(rpc('params.list', {'node': 'missing'}), -32001)
    error(rpc('params.list', {'node': True}), -32602)
    error(rpc('params.list', {'node': -1}), -32602)
    error(rpc('params.list', {'node': 2**40}), -32602)
    error(rpc('params.get', {'values': [{'node': 'synth', 'param': 999}]}), -32002)
    error(rpc('notes.send', {'on': True, 'key': 60, 'channel': 16}), -32602)
    error(rpc('notes.send', {'on': 1, 'key': 60}), -32602)
    error(rpc('notes.send', {'on': True, 'key': 60.5}), -32602)
    error(rpc('notes.send', {'on': True, 'key': 60, 'velocity': 1.01}), -32602)
    error(rpc('transport.set', {'tempo': '100'}), -32602)
    error(raw('{"jsonrpc":"2.0","id":1,"method":"transport.set","params":{"tempo":1e999}}'), -32602)
    error(rpc('transport.set', {'tempo': 0}), -32602)
    error(rpc('transport.set', {'tempo': 99, 'clockSource': 'typo'}), -32602)
    raw('!render')
    assert rpc('transport.get')['result']['tempo'] == 120  # rejected transport list changed nothing
    meta = rpc('params.list', {'node': 'synth'})['result']
    assert meta['node'] == 1 and len(meta['parameters']) == 3
    assert meta['parameters'][2]['name'] == 'Pressure' and meta['parameters'][2]['defaultValue'] == 0.5
    assert rpc('params.text', {'node': 1, 'param': 3, 'value': 0.5})['result']['text'] == '50%'
    original = value(1)
    error(rpc('params.set', {'values': [{'node': 1, 'param': 1, 'value': 0.91}, {'node': 1, 'param': 999, 'value': 1}]}), -32002)
    assert value(1) == original
    error(raw('{"jsonrpc":"2.0","id":1,"method":"transport.set","params":{"tempo":99,"tempo":101}}'), -32602)
    error(rpc('params.set', {'values': [{'node': 1, 'param': 1, 'value': 1}, {'node': 'synth', 'param': 1, 'value': 0}]}), -32602)
    assert value(1) == original
    assert rpc('params.set', {'values': [{'node': 1, 'param': 3, 'value': 2}]})['result']['values'][0]['value'] == 1
    assert raw('{"jsonrpc":"2.0","method":"transport.set","params":{"tempo":93,"playing":true}}') is None
    assert raw('{"jsonrpc":"2.0","method":"unknown"}') is None
    assert raw('{"jsonrpc":"2.0","method":"notes.send","params":{"key":999}}') is None
    raw('!render')
    state = rpc('transport.get')['result']
    assert state['tempo'] == 93 and state['playing'] is True
    batch = raw('[{"jsonrpc":"2.0","id":"a","method":"transport.get"},17,{"jsonrpc":"2.0","method":"unknown"},{"jsonrpc":"2.0","id":null,"method":"unknown"}]')
    assert len(batch) == 3 and batch[0]['id'] == 'a'
    error(batch[1], -32600, None)
    error(batch[2], -32601, None)
    assert raw('[{"jsonrpc":"2.0","method":"transport.get"}]') is None
    error(raw(' ' * 65537), -32006, None)
    error(raw('[' * 33 + '0' + ']' * 33), -32006, None)
    # Reject over-limit batch before even its first mutation runs.
    over_batch = [{'jsonrpc': '2.0', 'method': 'transport.set', 'params': {'tempo': 222}}] * 65
    error(raw(json.dumps(over_batch)), -32006, None)
    raw('!render')
    assert rpc('transport.get')['result']['tempo'] == 93
    for _ in range(128):
        assert rpc('notes.send', {'on': False, 'key': 60})['result']['accepted']
    error(rpc('notes.send', {'on': False, 'key': 60}), -32005)
    raw('!render')
    assert rpc('notes.send', {'on': False, 'key': 60})['result']['accepted']
    # Repeated values coalesce instead of filling the plugin queue.
    for i in range(5000):
        assert 'result' in rpc('params.set', {'values': [{'node': 1, 'param': 1, 'value': (i % 100) / 100}]})
    assert value(1) == 0.99
    raw('!drain')
    # Ordered gesture commands cannot coalesce. Leave one pending slot for a value.
    for _ in range(4095):
        assert 'result' in rpc('params.gesture', {'node': 1, 'param': 1, 'begin': True})
    assert 'result' in rpc('params.set', {'values': [{'node': 1, 'param': 1, 'value': 0.5}]})
    response = rpc('params.set', {'values': [{'node': 1, 'param': 1, 'value': 0.6}, {'node': 1, 'param': 2, 'value': 0.7}]})
    error(response, -32005)
    assert response['error']['data'] == {'accepted': 1, 'index': 1}, response
    assert value(1) == 0.6
    raw('!drain')
    assert 'result' in rpc('params.set', {'values': [{'node': 1, 'param': 2, 'value': 0.7}]})
    assert 'result' in rpc('subscribe', {'topics': ['params.changed', 'transport', 'graph.changed']})
    rpc('params.set', {'values': [{'node': 1, 'param': 3, 'value': 0.4}]})
    notifications = raw('!poll 0')
    assert any(n['method'] == 'params.changed' for n in notifications), notifications
    assert any(n['method'] == 'transport' for n in notifications), notifications
    assert raw('!poll 1') == []
    assert 'result' in rpc('unsubscribe', {'topics': ['params.changed', 'transport']})
    error(rpc('subscribe', {'topics': ['bad']}), -32602)
    graph = rpc('graph.get')['result']
    assert graph['revision'] == 1 and not graph['staged'] and len(graph['nodes']) == 1
    error(rpc('graph.add', {'path': '/missing/no-plugin.clap'}), -32004)
    error(rpc('graph.add', {'path': sys.argv[2], 'name': 'synth'}), -32602)
    delay = rpc('graph.add', {'path': sys.argv[3], 'name': 'delay'})['result']['node']
    assert rpc('graph.get')['result']['staged']
    assert 'result' in rpc('graph.connect', {'kind': 'audio', 'from': 1, 'to': delay})
    assert 'result' in rpc('graph.connect', {'kind': 'audio', 'from': delay, 'to': delay})
    error(rpc('graph.commit'), -32003)
    assert rpc('graph.get')['result']['revision'] == 1
    assert raw('!poll 2') == []
    rpc('graph.disconnect', {'kind': 'audio', 'from': delay, 'to': delay})
    assert rpc('graph.commit')['result']['revision'] == 2
    assert raw('!poll 3')[0]['method'] == 'graph.changed'
    assert 'result' in rpc('graph.map', {'from': delay, 'lane': 0, 'to': 1, 'param': 3})
    assert len(rpc('graph.get')['result']['mappings']) == 1
    assert 'result' in rpc('graph.unmap', {'from': delay, 'lane': 0})
    assert 'result' in rpc('graph.remove', {'node': delay})
    assert len(rpc('graph.get')['result']['nodes']) == 1
    error(rpc('params.list', {'node': 'delay'}), -32001)
    rpc('graph.commit')
    print('ok: JSON-RPC envelopes, IDs, batches, notifications, validation, limits and queue errors')
finally:
    proc.stdin.close()
    proc.wait(timeout=5)
    assert proc.returncode == 0
