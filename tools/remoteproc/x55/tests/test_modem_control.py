#!/usr/bin/python3
# SPDX-License-Identifier: GPL-2.0-only
"""Isolated lifecycle tests: fake sysfs, commands and QMI. Nothing on the modem.
All files stay in fresh /tmp directories; no deletion or system service calls.
"""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest

SCRIPT = Path(__file__).parents[1] / 'modem.sh'


class Tests(unittest.TestCase):
    def setUp(self):
        self.root = Path(tempfile.mkdtemp(prefix='x55-control-test-', dir='/tmp'))
        self.rproc = self.root / 'sys/remoteproc6'
        self.rproc.mkdir(parents=True)
        (self.rproc / 'name').write_text('esoc0\n')
        (self.rproc / 'state').write_text('offline\n')
        (self.rproc / 'recovery').write_text('enabled\n')
        cdsp = self.root / 'sys/remoteproc0'
        cdsp.mkdir()
        (cdsp / 'name').write_text('cdsp\n')
        (cdsp / 'state').write_text('running\n')
        (self.root / 'bin').mkdir()
        (self.root / 'mode').write_text('shutting-down\n')
        (self.root / 'efs.json').write_text('{"state":"ready"}')
        for name in ('tqftpserv', 'pd-mapper', 'efs_sync.py'):
            self.executable(name, '#!/bin/bash\nexit 0\n')
        self.executable('mdm_helper_native', '''#!/bin/bash
if [[ $1 == --help ]]; then echo --check-alive; exit; fi
[[ $1 == --check-alive ]] || exit 99
[[ ${FAKE_ALIVE:-1} == 1 ]]
''')
        self.executable('qrtr-lookup', '''#!/bin/bash
printf 'Service Version Instance Node Port\n2 1 0 42 67 DMS\n'
[[ ${AMBIGUOUS:-0} != 1 ]] || printf '2 1 0 99 67 DMS\n'
''')
        self.executable('qmicli-x55', '''#!/bin/bash
printf 'qmi %s\n' "$*" >> "$FAKE_ROOT/trace"
for arg; do
 case "$arg" in
 --dms-get-operating-mode)
  read -r mode < "$FAKE_ROOT/mode"
  printf "Mode: '%s'\\nHW restricted: '%s'\\n" "$mode" "${RESTRICTED:-no}"
  ;;
 --dms-set-operating-mode=online) printf 'online\\n' > "$FAKE_ROOT/mode" ;;
 esac
done
''')

    def executable(self, name, text):
        p = self.root / 'bin' / name
        p.write_text(text)
        p.chmod(0o700)

    def run_control(self, action, **extra):
        env = dict(os.environ, FAKE_ROOT=str(self.root), **extra)
        shell = '''
source "$1"
ROOT=$FAKE_ROOT
RPROC_CLASS=$FAKE_ROOT/sys
EFS_STATUS=$FAKE_ROOT/efs.json
ATTEMPT=$FAKE_ROOT/attempt
LOCK=$FAKE_ROOT/control.lock
require_kernel() { return 0; }
check_firmware() { return 0; }
check_loader() { [[ ${STALE_DISK:-0} != 1 ]]; }
check_loaded_loader() { [[ ${STALE_LOADED:-0} != 1 ]]; }
modprobe() { printf 'module %s\\n' "$*" >> "$FAKE_ROOT/trace"; }
sleep() { :; }
systemd-run() { printf 'submit %s\\n' "$*" >> "$FAKE_ROOT/trace"; }
systemctl() {
 printf 'systemctl %s\\n' "$*" >> "$FAKE_ROOT/trace"
 case "$1" in
 show) echo loaded ;;
 is-active)
  [[ "$*" != *x55-rproc-start.service* ]] || return 1
  [[ -z ${FAILED_UNIT:-} || "$*" != *"$FAILED_UNIT"* ]]
  ;;
 start) : ;;
 stop) printf 'state-before-services-stop=%s\\n' "$(<"$RPROC/state")" >> "$FAKE_ROOT/trace" ;;
 *) return 99 ;;
 esac
}
main "$2"
'''
        return subprocess.run(['bash', '-c', shell, 'test', str(SCRIPT), action],
                              env=env, capture_output=True, text=True, timeout=5)

    def trace(self):
        p = self.root / 'trace'
        return p.read_text() if p.exists() else ''

    def running(self):
        (self.rproc / 'state').write_text('running\n')

    def test_start_is_one_attempt_and_discovers_esoc(self):
        # An EFS status file already present also prevents a repeat attempt.
        # Move it aside, never delete it, to model a fresh boot.
        (self.root / 'efs.json').rename(self.root / 'efs-unused.json')
        r = self.run_control('start')
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual((self.rproc / 'recovery').read_text(), 'disabled\n')
        self.assertEqual((self.root / 'sys/remoteproc0/state').read_text(), 'running\n')
        self.assertIn('start x55-runtime.target', self.trace())
        self.assertIn('submit --unit=x55-rproc-start', self.trace())
        r = self.run_control('start')
        self.assertNotEqual(r.returncode, 0)
        self.assertEqual(self.trace().count('submit '), 1)

    def test_stale_loader_blocks_start(self):
        (self.root / 'efs.json').rename(self.root / 'efs-unused.json')
        for flag in ('STALE_DISK', 'STALE_LOADED'):
            with self.subTest(flag=flag):
                r = self.run_control('start', **{flag: '1'})
                self.assertNotEqual(r.returncode, 0)
                self.assertNotIn('submit', self.trace())
                self.assertFalse((self.root / 'attempt').exists())

    def test_start_rejects_existing_efs_attempt(self):
        r = self.run_control('start')
        self.assertNotEqual(r.returncode, 0)
        self.assertNotIn('submit', self.trace())

    def test_failed_service_prevents_hardware_start(self):
        (self.root / 'efs.json').rename(self.root / 'efs-unused.json')
        r = self.run_control('start', FAILED_UNIT='x55-efs-sync.service')
        self.assertNotEqual(r.returncode, 0)
        self.assertNotIn('submit', self.trace())
        self.assertTrue((self.root / 'attempt').exists())

    def test_online_is_explicit_and_not_repeated(self):
        self.running()
        for _ in range(2):
            r = self.run_control('online')
            self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(self.trace().count('--dms-set-operating-mode=online'), 1)
        self.assertIn('-d qrtr://42', self.trace())

    def test_hardware_restriction_not_overridden(self):
        self.running()
        r = self.run_control('online', RESTRICTED='yes')
        self.assertNotEqual(r.returncode, 0)
        self.assertNotIn('--dms-set-operating-mode', self.trace())

    def test_errfatal_prevents_qmi(self):
        self.running()
        r = self.run_control('probe', FAKE_ALIVE='0')
        self.assertNotEqual(r.returncode, 0)
        self.assertNotIn('qmi ', self.trace())

    def test_failed_efs_prevents_qmi(self):
        self.running()
        (self.root / 'efs.json').write_text('{"state":"failed"}')
        r = self.run_control('probe')
        self.assertNotEqual(r.returncode, 0)
        self.assertNotIn('qmi ', self.trace())

    def test_ambiguous_modem_node_rejected(self):
        self.running()
        r = self.run_control('probe', AMBIGUOUS='1')
        self.assertNotEqual(r.returncode, 0)
        self.assertNotIn('qmi ', self.trace())

    def test_stop_keeps_services_until_shutdown(self):
        self.running()
        r = self.run_control('stop')
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn('state-before-services-stop=stop', self.trace())
        self.assertIn('x55-efs-sync.service', self.trace())
        self.assertEqual((self.root / 'sys/remoteproc0/state').read_text(), 'running\n')


if __name__ == '__main__':
    unittest.main(verbosity=2)
