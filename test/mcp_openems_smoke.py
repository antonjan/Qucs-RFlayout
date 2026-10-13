#!/usr/bin/env python3
"""Run an actual Octave/OpenEMS fixture through MCP and check its artifacts."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import time

binary, octave, script, prefix = sys.argv[1:]
with tempfile.TemporaryDirectory(prefix='qrfl-openems-') as directory:
    with tempfile.TemporaryFile() as stderr:
        server = subprocess.Popen([binary, '--octave', octave, '--openems-prefix', prefix],
                                  stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=stderr, text=True)
        def request(method, params=None, notification=False):
            body={'jsonrpc':'2.0','method':method}
            if params is not None: body['params']=params
            if not notification: body['id']=1
            server.stdin.write(json.dumps(body)+'\n'); server.stdin.flush()
            if not notification:
                return json.loads(server.stdout.readline())['result']
        def call(name, **arguments):
            result=request('tools/call',{'name':name,'arguments':arguments})
            assert not result['isError'], result
            return json.loads(result['content'][0]['text'])
        try:
            request('initialize',{'protocolVersion':'2025-06-18','capabilities':{},'clientInfo':{'name':'openems-test','version':'1'}})
            request('notifications/initialized',notification=True)
            job=call('start_simulation',script=script,output_directory=str(Path(directory)/'job'),timeout_seconds=50)
            deadline=time.monotonic()+55
            while time.monotonic()<deadline:
                status=call('simulation_status',job_id=job['job_id'],log_tail_lines=100)
                if status['state'] not in ('starting','running','cancelling'): break
                time.sleep(.1)
            assert status['state']=='completed',status
            assert 'OPENEMS_SMOKE_OK' in status['log_tail'],status
            results=call('simulation_results',job_id=job['job_id'])
            assert not results['partial']
            kinds={file['kind'] for file in results['files']}
            assert {'plot','s_parameters','data','geometry'} <= kinds,results
            print('Octave + actual OpenEMS through MCP: solver, probes, plot, S parameters and artifacts passed')
        finally:
            server.stdin.close()
            try:
                assert server.wait(timeout=8)==0
            finally:
                if server.poll() is None:
                    server.kill(); server.wait()
