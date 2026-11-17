"""End-to-end acceptance checks for the independent virtual-time circuit model."""
import csv
import math
from pathlib import Path
import re
import signal
import subprocess
import sys
import tempfile
import time

binary = str(Path(sys.argv[1]).resolve())
cases = 0

def check(args, status, *expected):
    global cases
    run = subprocess.run([binary, *args], capture_output=True, text=True, timeout=10)
    output = run.stdout + run.stderr
    assert run.returncode == status, (args, run.returncode, output)
    assert all(text in output for text in expected), (args, output)
    cases += 1
    return output

check(['--help'], 0, '--test divider|rc|adc|loopback|all', '--capacitance-uf')
check(['--simulate'], 0, 'SIMULATION', 'expected=1.6500V', 'RESULT PASS: voltage divider')
output = check(['--simulate', '--test', 'all'], 0, 'OVERALL PASS', 'RESULT PASS: RC')
for phase in ['charge', 'discharge']:
    tau = float(re.search(rf'RC {phase}_tau=([0-9.]+)s', output).group(1))
    assert abs(tau - 0.5) < 0.005, (phase, tau)
check(['--simulate', '--test', 'adc', '--samples', '3'], 0, 'raw=', 'RESULT MEASURED')
check(['--simulate', '--test', 'loopback'], 0, 'drive=0 matching_reads=5/5', 'drive=1 matching_reads=5/5')
check(['--simulate', '--fault', 'divider-low'], 1, 'RESULT FAIL: voltage divider')
check(['--simulate', '--rtop', '22000'], 1, 'RESULT FAIL: voltage divider')
check(['--simulate', '--vref', '3.5'], 0, 'RESULT PASS: voltage divider')
check(['--simulate', '--test', 'rc'], 0, 'RESULT PASS: RC')
check(['--simulate', '--test', 'rc', '--rc-channel', '7'], 0, 'RESULT PASS: RC')
check(['--simulate', '--test', 'rc', '--resistance', '10000'], 1, 'RESULT FAIL: RC')
check(['--simulate', '--test', 'rc', '--capacitance-uf', '20'], 1, 'RESULT FAIL: RC')
for fault in ['rc-fast', 'rc-slow', 'rc-stuck-low', 'rc-stuck-high']:
    out = check(['--simulate', '--test', 'rc', '--fault', fault], 1, 'RESULT FAIL: RC')
    assert 'RESULT PASS: RC' not in out
check(['--simulate', '--fault', 'adc-error'], 2, 'RESULT ERROR')
check(['--simulate', '--test', 'rc', '--fault', 'adc-error'], 2, 'RESULT ERROR')
check(['--simulate', '--test', 'rc', '--fault', 'gpio-error'], 2, 'RESULT ERROR')
check(['--simulate', '--test', 'loopback', '--fault', 'gpio-error'], 2, 'RESULT ERROR')
check(['--simulate', '--test', 'all', '--fault', 'divider-low'], 1, 'RESULT PASS: RC', 'OVERALL FAIL')
check(['--simulate', '--test', 'all', '--fault', 'rc-fast'], 1, 'RESULT PASS: voltage divider', 'OVERALL FAIL')
invalid = [
    ['--samples', '0'], ['--samples', 'nan'], ['--channel', '8'], ['--channel', '-1'],
    ['--rtop', '0'], ['--vref', 'nan'], ['--test', 'unknown'], ['--test', 'logic'],
    ['--fault', 'adc-error'], ['--samples'], ['--unknown', 'x'], ['--rc-channel', '8'],
    ['--test', 'all', '--rc-channel', '3'], ['--csv', 'unused.csv'],
    ['--test', 'loopback', '--drive', 'GPIO24'],
    ['--test', 'rc', '--resistance', '0'], ['--test', 'rc', '--capacitance-uf', '-10'],
    ['--test', 'rc', '--interval-ms', '100'], ['--test', 'rc', '--window', '0.5'],
    ['--test', 'rc', '--tau-tolerance', 'nan'], ['--test', 'rc', '--tau-tolerance', '1'],
    ['--test', 'rc', '--tolerance', '1'], ['--test', 'rc', '--vref', '3.0'],
    ['--test', 'rc', '--capacitance-uf', '1000'],
]
for args in invalid:
    check(['--simulate', *args], 2)
with tempfile.TemporaryDirectory() as directory:
    path = Path(directory) / 'rc.csv'
    check(['--simulate', '--test', 'rc', '--csv', str(path)], 0, 'RC CSV capture:')
    with path.open() as stream:
        rows = list(csv.DictReader(stream))
    assert set(rows[0]) == {'phase', 'time_s', 'raw', 'volts'}
    assert {r['phase'] for r in rows} == {'prepare', 'charge', 'discharge'}
    for phase in ['prepare', 'charge', 'discharge']:
        samples = [r for r in rows if r['phase'] == phase]
        times = [float(r['time_s']) for r in samples]
        assert len(samples) >= 700 and all(b > a for a, b in zip(times, times[1:]))
        assert times[-1] >= 3.5
        assert all(0 <= int(r['raw']) <= 4095 and math.isfinite(float(r['volts'])) for r in samples)
    saved = path.read_bytes()
    check(['--simulate', '--test', 'rc', '--csv', str(path)], 2, 'RC CSV open')
    assert path.read_bytes() == saved
    check(['--simulate', '--test', 'rc', '--csv', str(Path(directory) / 'missing' / 'rc.csv')],
          2, 'RC CSV open')
    for sig in [signal.SIGINT, signal.SIGTERM]:
        capture = Path(directory) / f'interrupted-{sig}.csv'
        process = subprocess.Popen([binary, '--simulate', '--test', 'rc', '--window', '100',
                                    '--interval-ms', '1', '--csv', str(capture)],
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        deadline = time.monotonic() + 5
        while (not capture.exists() or capture.stat().st_size < 4096) and process.poll() is None:
            assert time.monotonic() < deadline
            time.sleep(0.001)
        assert process.poll() is None, 'long RC capture ended before interruption'
        process.send_signal(sig)
        out, err = process.communicate(timeout=5)
        assert process.returncode == 130 and 'RESULT PASS' not in out, (out, err)
        cases += 1
print(f'PASS: {cases} scenarios; RC numeric estimates, faults, CSV integrity, invalid inputs, SIGINT/SIGTERM.')
