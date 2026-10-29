"""End-to-end simulator acceptance checks; requires only Python's standard library."""
import signal
import subprocess
import sys
import time

binary = sys.argv[1]

def check(args, status, *expected):
    run = subprocess.run([binary, *args], capture_output=True, text=True, timeout=5)
    output = run.stdout + run.stderr
    assert run.returncode == status, (args, run.returncode, output)
    assert all(text in output for text in expected), (args, output)
    return output

check(['--simulate'], 0, 'SIMULATION', 'expected=1.6500V', 'RESULT PASS: voltage divider')
output = check(['--simulate', '--test', 'all'], 0, 'OVERALL PASS')
assert all(f'A={a} B={b}' in output for a, b in [(0, 0), (0, 1), (1, 0), (1, 1)])
check(['--simulate', '--test', 'adc', '--samples', '3'], 0, 'raw=', 'RESULT MEASURED')
check(['--simulate', '--test', 'loopback'], 0, 'RESULT PASS: GPIO loopback')
check(['--simulate', '--fault', 'divider-low'], 1, 'RESULT FAIL: voltage divider')
# A deliberately wrong expected resistor must fail the independent circuit model.
check(['--simulate', '--rtop', '22000'], 1, 'RESULT FAIL: voltage divider')
check(['--simulate', '--test', 'logic', '--fault', 'logic-stuck-low'], 1, 'RESULT FAIL: 74HC00')
check(['--simulate', '--fault', 'adc-error'], 2, 'RESULT ERROR')
check(['--simulate', '--test', 'logic', '--fault', 'gpio-error'], 2, 'RESULT ERROR')
check(['--simulate', '--test', 'all', '--fault', 'divider-low'], 1, 'RESULT PASS: 74HC00', 'OVERALL FAIL')
for args in [['--samples', '0'], ['--samples', 'nan'], ['--channel', '8'],
             ['--channel', '-1'], ['--rtop', '0'], ['--vref', 'nan'],
             ['--test', 'unknown'], ['--a', 'GPIO24'], ['--fault', 'adc-error'],
             ['--samples'], ['--simulate', '--unknown', 'x']]:
    check(args, 2)
process = subprocess.Popen([binary, '--simulate', '--samples', '10000'],
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
time.sleep(0.05)
process.send_signal(signal.SIGINT)
out, err = process.communicate(timeout=3)
assert process.returncode == 130 and 'RESULT PASS' not in out, (out, err)
print('PASS: positive/negative circuit cases, all truth rows, I/O errors, invalid inputs, SIGINT.')
