#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Configure two tracks after shared-effects.py has configured a disposable engine.

No playback by default. --play runs a 26-second demonstration after configuration;
--play-only replays the saved configuration without rewriting notes or sounds.
"""
import argparse
import json
import socket
import time


def parameters():
    values = [(1, 0), (10, 0), (20, 1), (11, 0), (21, 0), (13, 1), (23, 1)]
    for pattern in range(2):
        for track in range(2):
            base = pattern * 10000 + track * 1000
            values.append((base + 50, (16 if track == 0 else 12) if pattern == 0 else (8 if track == 0 else 5)))
            melody = ([48, 55, 60, 55] if track == 0 else [72, 76, 79, 83])
            for step in range(16):
                values.extend([(base + 100 + step, int(step % 4 != 3)),
                               (base + 200 + step, melody[step % 4] + pattern * 5),
                               (base + 300 + step, .5)])
    assert len(values) == 203 and len({p for p, _ in values}) == len(values)
    return [{'node': 'sequencer', 'param': p, 'value': v} for p, v in values]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('socket')
    parser.add_argument('--play', action='store_true')
    parser.add_argument('--play-only', action='store_true')
    args = parser.parse_args()
    with socket.socket(socket.AF_UNIX) as connection:
        connection.settimeout(10)
        connection.connect(args.socket)
        with connection.makefile('rwb') as stream:
            serial = 0

            def rpc(method, params=None):
                nonlocal serial
                serial += 1
                request = {'jsonrpc': '2.0', 'id': serial, 'method': method}
                if params is not None:
                    request['params'] = params
                stream.write((json.dumps(request) + '\n').encode())
                stream.flush()
                response = json.loads(stream.readline())
                if response.get('id') != serial or 'error' in response:
                    raise RuntimeError(response)
                return response['result']

            nodes = {n['name']: n for n in rpc('graph.get')['nodes']}
            assert nodes['sequencer']['plugin']['id'] == 'org.oscairn.step-sequencer'
            supported = {p['id'] for p in rpc('params.list', {'node': 'sequencer'})['parameters']}
            assert {p['param'] for p in parameters()} <= supported, 'Requires the two-track sequencer'
            sounds = {c['name']: c['id'] for c in rpc('sounds.get', {'node': 'synth'})['contexts']}
            names = ('Saws / short echo', 'Square / long echo')
            assert all(name in sounds for name in names), 'Run shared-effects.py first'
            if not args.play_only:
                rpc('transport.set', {'playing': False, 'tempo': 120})
                for lane in range(2):
                    rpc('graph.unmap', {'from': 'sequencer', 'lane': lane})
                rpc('graph.commit')
                for channel, name in enumerate(names):
                    rpc('sounds.select', {'node': 'synth', 'context': sounds[name], 'channel': channel})
                values = parameters()
                for start in range(0, len(values), 128):
                    rpc('params.set', {'values': values[start:start + 128]})
            print('Configured: Track 1 saws / MIDI 1, Track 2 square / MIDI 2; A lengths 16/12, B lengths 8/5.', flush=True)
            if args.play or args.play_only:
                def set_param(param, value):
                    rpc('params.set', {'values': [{'node': 'sequencer', 'param': param, 'value': value}]})

                try:
                    rpc('transport.set', {'playing': False})
                    set_param(1, 0)
                    set_param(21, 0)
                    time.sleep(.1)
                    rpc('transport.set', {'playing': True})
                    time.sleep(5)
                    print('Request B; wait for next bar.', flush=True)
                    set_param(1, 1)
                    time.sleep(7)
                    print('Mute square track; echoes may continue.', flush=True)
                    set_param(21, 1)
                    time.sleep(3)
                    set_param(21, 0)
                    time.sleep(3)
                    print('Request A; wait for next bar.', flush=True)
                    set_param(1, 0)
                    time.sleep(4)
                finally:
                    rpc('transport.set', {'playing': False})
                    set_param(21, 0)
                    set_param(1, 0)
                time.sleep(4)
                print(json.dumps(rpc('session.status'), indent=2), flush=True)


if __name__ == '__main__':
    main()
