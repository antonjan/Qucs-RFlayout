#!/usr/bin/env python3
"""Test the real MCP process and real Octave lifecycle, without a long RF solve."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time

binary, octave = map(Path, sys.argv[1:])

class Client:
    def __init__(self, *options):
        self.log = tempfile.TemporaryFile()
        self.process = subprocess.Popen([str(binary), '--octave', str(octave), *options],
                                        stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                        stderr=self.log, text=True)
        self.request = 0
        self.send('initialize', {'protocolVersion': '2025-06-18', 'capabilities': {},
                                'clientInfo': {'name': 'test', 'version': '1'}})
        self.send('notifications/initialized', notification=True)
    def send(self, method, params=None, notification=False):
        message = {'jsonrpc': '2.0', 'method': method}
        if params is not None:
            message['params'] = params
        if not notification:
            self.request += 1
            message['id'] = self.request
        self.process.stdin.write(json.dumps(message) + '\n')
        self.process.stdin.flush()
        if not notification:
            response = json.loads(self.process.stdout.readline())
            assert response['id'] == self.request, response
            return response
    def call(self, name, **args):
        return self.send('tools/call', {'name': name, 'arguments': args})['result']
    def data(self, name, **args):
        result = self.call(name, **args)
        assert not result['isError'], result
        return json.loads(result['content'][0]['text'])
    def wait(self, job, expected, timeout=12):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            result = self.data('simulation_status', job_id=job)
            if result['state'] not in ('starting', 'running', 'cancelling'):
                assert result['state'] == expected, result
                return result
            time.sleep(.05)
        raise AssertionError('Job did not stop: ' + str(result))
    def close(self):
        self.process.stdin.close()
        try:
            assert self.process.wait(timeout=8) == 0
        finally:
            if self.process.poll() is None:
                self.process.kill()
                self.process.wait()
            self.log.close()

with tempfile.TemporaryDirectory(prefix='qrfl octave test ') as directory:
    root = Path(directory)
    # Spaces, apostrophes and shell metacharacters are literal paths.
    script = root / "filter's $HOME;literal.m"
    script.write_text("""clear; close all;
args = argv();
assert(any(strcmp(args, '--batch')));
assert(any(strcmp(args, '--no-gui')));
disp('SCRIPT_STARTED'); fflush(stdout);
pause(0.4);
mkdir('results');
fid=fopen('results/filter.s2p', 'w'); fprintf(fid, '# Hz S RI R 50\\n1e9 0 0 1 0 1 0 0 0\\n'); fclose(fid);
fid=fopen('results/response.csv', 'w'); fprintf(fid, 'frequency,s21\\n1e9,1\\n'); fclose(fid);
if any(strcmp(args, '--only-preprocess')); disp('PREPROCESS_ONLY'); endif
""")
    failing = root / 'failed.m'
    failing.write_text("error('intentional fixture failure');\n")
    sleeping = root / 'long.m'
    sleeping.write_text("disp('WAITING'); fflush(stdout); pause(30);\n")
    client = Client()
    try:
        names = {tool['name'] for tool in client.send('tools/list')['result']['tools']}
        assert {'convert_layout', 'start_simulation', 'simulation_status', 'simulation_results', 'cancel_simulation'} <= names
        for invalid in [dict(script=str(script), output_directory=str(root)),
                        dict(script=str(root/'missing.m'), output_directory=str(root/'missing')),
                        dict(script=str(script), output_directory=str(root/'bad'), timeout_seconds=0),
                        dict(script=str(script), output_directory=str(root/'bad'), timeout_seconds=True),
                        dict(script=str(script), output_directory=str(root/'bad'), timeout_seconds=2.5),
                        dict(script=str(script), output_directory=str(root/'bad'), preprocess_only='yes'),
                        dict(script=str(script), output_directory=str(root/'bad'), executable='sh')]:
            assert client.call('start_simulation', **invalid)['isError']
        assert client.call('simulation_status', job_id='unknown')['isError']
        assert not (root/'bad').exists()
        begin = time.monotonic()
        job = client.data('start_simulation', script=str(script), output_directory=str(root/"job's results"))
        assert time.monotonic()-begin < 2, job
        assert job['state'] in ('starting', 'running')
        assert client.send('ping')['result'] == {}
        assert client.call('start_simulation', script=str(script), output_directory=str(root/'overlap'))['isError']
        assert not (root/'overlap').exists()
        final = client.wait(job['job_id'], 'completed')
        assert final['exit_code'] == 0 and 'SCRIPT_STARTED' in final['log_tail']
        assert final['elapsed_seconds'] < 8
        results = client.data('simulation_results', job_id=job['job_id'])
        assert not results['partial'] and {file['kind'] for file in results['files']} >= {'s_parameters', 'data'}
        assert all(Path(file['path']).is_file() for file in results['files'])
        assert json.loads((root/"job's results"/'job.json').read_text())['state'] == 'completed'
        assert client.data('cancel_simulation', job_id=job['job_id'])['state'] == 'completed'
        assert client.data('simulation_status', job_id=job['job_id'], log_tail_lines=0)['log_tail'] == ''
        assert client.call('simulation_status', job_id=job['job_id'], log_tail_lines=1000)['isError']
        # A script error becomes a failed job, not a successful completion.
        failure = client.data('start_simulation', script=str(failing), output_directory=str(root/'failure'))
        assert 'intentional fixture failure' in client.wait(failure['job_id'], 'failed')['log_tail']
        assert client.data('simulation_results', job_id=failure['job_id'])['partial']
        pre = client.data('start_simulation', script=str(script), output_directory=str(root/'pre'), preprocess_only=True)
        assert 'PREPROCESS_ONLY' in client.wait(pre['job_id'], 'completed')['log_tail']
        assert client.data('simulation_results', job_id=pre['job_id'])['preprocess_only']
        timed = client.data('start_simulation', script=str(sleeping), output_directory=str(root/'timeout'), timeout_seconds=1)
        assert client.wait(timed['job_id'], 'timed_out')['stop_reason'] == 'timed_out'
        cancelled = client.data('start_simulation', script=str(sleeping), output_directory=str(root/'cancel'))
        time.sleep(.1)
        assert client.data('cancel_simulation', job_id=cancelled['job_id'])['state'] in ('cancelling', 'cancelled')
        client.wait(cancelled['job_id'], 'cancelled')
        # Do not follow links outside the output directory when listing results.
        if os.name == 'posix':
            outside = root/'external.csv'; outside.write_text('outside\n')
            (root/"job's results"/'link.csv').symlink_to(outside)
            assert all(not file['path'].endswith('link.csv') for file in client.data('simulation_results', job_id=job['job_id'])['files'])
            # The solver child must stop as well as Octave.
            child = root/'child.m'
            child.write_text("system('sleep 30 & echo $! > child.pid'); pause(30);\n")
            child_job = client.data('start_simulation', script=str(child), output_directory=str(root/'child_job'))
            deadline = time.monotonic()+5
            pid_file = root/'child_job/child.pid'
            while not pid_file.exists() and time.monotonic()<deadline:
                time.sleep(.05)
            pid = int(pid_file.read_text())
            client.data('cancel_simulation', job_id=child_job['job_id'])
            client.wait(child_job['job_id'], 'cancelled')
            time.sleep(.1)
            stat = Path(f'/proc/{pid}/stat')
            assert not stat.exists() or stat.read_text().split()[2] == 'Z', 'Solver descendant still running'
        # Closing stdin cancels an active job and preserves its final metadata.
        last = client.data('start_simulation', script=str(sleeping), output_directory=str(root/'shutdown'))
    finally:
        client.close()
    assert json.loads((root/'shutdown/job.json').read_text())['state'] == 'cancelled'
    # Missing executable is caught before creating an output directory.
    missing = Client('--octave', '/nonexistent/qrfl-octave')
    try:
        assert missing.call('start_simulation', script=str(script), output_directory=str(root/'no_executable'))['isError']
        assert not (root/'no_executable').exists()
    finally:
        missing.close()
print('MCP + real Octave: success, failure, timeout, cancellation, descendants, EOF cleanup and results passed')
