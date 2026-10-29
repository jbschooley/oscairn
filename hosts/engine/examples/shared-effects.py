#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Configure a fresh demo engine with two sound buses and shared Delay contexts.

Run against a disposable engine session; this edits its graph and prepared sounds.
Use --play for a 26-second listening demonstration. Otherwise configure only.
"""
import argparse
import json
import socket
import time

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('socket')
parser.add_argument('--play', action='store_true')
parser.add_argument('--play-only', action='store_true', help='Replay an already configured demo')
args = parser.parse_args()
with socket.socket(socket.AF_UNIX) as connection:
    connection.settimeout(10)
    connection.connect(args.socket)
    with connection.makefile('rwb') as stream:
        serial = 0

        def rpc(method, params=None):
            global serial
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

        graph = rpc('graph.get')
        # Validate the intended demo nodes before mutating a running session.
        nodes = {n['name']: n for n in graph['nodes']}
        assert nodes['synth']['plugin']['id'] == 'org.oscairn.basic-synth', 'Requires Basic Synth'
        assert nodes['delay']['plugin']['id'] == 'org.oscairn.delay', 'Requires native Delay'
        if not args.play_only:
            assert 'long-echo' not in nodes, 'Use a fresh engine session'
            rpc('transport.set', {'playing': False, 'tempo': 120})
            rpc('graph.unmap', {'from': 'sequencer', 'lane': 0})
            sibling = rpc('effects.create', {'source': 'delay', 'name': 'long-echo'})['node']
            rpc('graph.connect', {'kind': 'audio', 'from': 'synth', 'fromPort': 1,
                                 'to': 'delay', 'toPort': 0, 'fromChannel': 0, 'toChannel': 0, 'channels': 2})
            rpc('graph.connect', {'kind': 'audio', 'from': 'synth', 'fromPort': 2,
                                 'to': sibling, 'toPort': 0, 'fromChannel': 0, 'toChannel': 0, 'channels': 2})
            rpc('graph.connect', {'kind': 'audio', 'from': sibling, 'to': graph['output']})
            rpc('graph.commit')
            rpc('params.set', {'values': [
                {'node': 'delay', 'param': 1, 'value': .25},
                {'node': 'delay', 'param': 2, 'value': .25},
                {'node': 'delay', 'param': 3, 'value': .6},
                {'node': sibling, 'param': 1, 'value': 1},
                {'node': sibling, 'param': 2, 'value': .7},
                {'node': sibling, 'param': 3, 'value': .65}]})
            patches = [
                ('Saws / short echo', [(1, .65), (2, .4), (4, 1), (5, 7), (8, 1)]),
                ('Square / long echo', [(1, .8), (2, .35), (4, 2), (6, 18), (7, 70), (8, 2)])]
            sounds = [rpc('sounds.prepare', {'node': 'synth', 'name': name,
                       'parameters': [{'id': p, 'value': v} for p, v in values]})['context']
                      for name, values in patches]
            rpc('sounds.select', {'node': 'synth', 'context': sounds[0]})
        else:
            patches = [('Saws / short echo', None), ('Square / long echo', None)]
            prepared = {c['name']: c['id'] for c in rpc('sounds.get', {'node':'synth'})['contexts']}
            sounds = [prepared[p[0]] for p in patches]
        print(json.dumps({'sounds': dict(zip((p[0] for p in patches), sounds)),
                          'routes': rpc('graph.get')['connections'],
                          'parameters': rpc('params.get', {'values': [
                              {'node': name, 'param': p} for name in ('delay', 'long-echo') for p in (1, 2, 3)]}),
                          'effects': {name: rpc('effects.resources', {'node': name})
                                      for name in ('delay', 'long-echo')}}, indent=2), flush=True)
        if args.play or args.play_only:
            try:
                for index, key in [(0, 48), (1, 60), (0, 55), (1, 67)]:
                    rpc('sounds.select', {'node': 'synth', 'context': sounds[index]})
                    rpc('notes.send', {'on': True, 'key': key, 'velocity': .7})
                    time.sleep(2)
                    # Change the next sound while the old note remains held.
                    rpc('sounds.select', {'node': 'synth', 'context': sounds[1-index]})
                    rpc('notes.send', {'on': True, 'key': key + 7, 'velocity': .6})
                    time.sleep(1)
                    rpc('notes.send', {'on': False, 'key': key})
                    rpc('notes.send', {'on': False, 'key': key + 7})
                    time.sleep(2)
                time.sleep(6)
            finally:
                for key in (48, 55, 60, 62, 67, 74):
                    rpc('notes.send', {'on': False, 'key': key})
                rpc('sounds.select', {'node': 'synth', 'context': sounds[0]})
            print(json.dumps(rpc('session.status'), indent=2), flush=True)
