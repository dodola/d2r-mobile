"""Offline transport tests: subprocess/ADB and device are mocked throughout."""
import json
import shlex
import subprocess
import unittest
from unittest.mock import Mock, patch

import debug_console as console


class ConsoleTests(unittest.TestCase):
    def test_envelope(self):
        self.assertEqual(console.envelope(['give', 'r30', '2', '1', 'inventory'], 'abc', 'req-1'),
                         'req-1 abc give r30 2 1 inventory\n')

    def test_reject_unsafe_or_oversize_input(self):
        for tokens in ([], ['heal\n'], ['$(id)'], ['a;b'], ['a b'], ['中文'], ['a' * 250]):
            with self.subTest(tokens=tokens), self.assertRaises(ValueError):
                console.envelope(tokens, 'abc', 'req')
        for run in ('ABC', 'x', '1' * 17, ''):
            with self.assertRaises(ValueError):
                console.envelope(['heal'], run, 'req')
        for request in ('', 'x' * 41, 'x/y'):
            with self.assertRaises(ValueError):
                console.envelope(['heal'], 'abc', request)

    def test_package_validation(self):
        for name in ('pkg;id', 'org.test\n', 'org.test bad'):
            with self.assertRaises(ValueError):
                console.Device('adb', None, name)

    @patch.object(console.subprocess, 'run')
    def test_transport_quotes_remote_script_and_uses_stdin(self, run):
        run.return_value = subprocess.CompletedProcess([], 0, '{}', '')
        device = console.Device('adb', 'test-device', 'org.d2r.binaryprobe')
        device.submit('req abc heal\n')
        args, kwargs = run.call_args
        self.assertEqual(args[0][:4], ['adb', '-s', 'test-device', 'shell'])
        remote = shlex.split(args[0][4])
        self.assertEqual(remote[:4], ['run-as', 'org.d2r.binaryprobe', 'sh', '-c'])
        self.assertIn('submit.lock', remote[4])
        self.assertIn('processing.txt', remote[4])
        self.assertEqual(kwargs['input'], 'req abc heal\n')
        self.assertNotIn('req abc heal', remote[4])
        self.assertFalse(kwargs.get('shell', False))

    @patch.object(console.subprocess, 'run')
    def test_mode_refuses_running_app(self, run):
        run.return_value = subprocess.CompletedProcess([], 0, '123\n', '')
        with self.assertRaisesRegex(RuntimeError, 'Exit the game'):
            console.Device('adb', None, 'org.d2r.binaryprobe').mode(True)
        self.assertEqual(run.call_count, 1)

    @patch.object(console.subprocess, 'run')
    def test_mode_changes_marker_only_after_stopped_check(self, run):
        run.side_effect = [subprocess.CompletedProcess([], 1, '', ''),
                           subprocess.CompletedProcess([], 0, '', '')]
        console.Device('adb', None, 'org.d2r.binaryprobe').mode(False)
        script = shlex.split(run.call_args.args[0][-1])[-1]
        self.assertIn('rm -f files/d2r-mobile/debug/enable', script)
        self.assertNotIn('/save', script)
        self.assertEqual(run.call_count, 2)

    def device(self):
        device = Mock()
        ready = dict(enabled=True, attached=True, ready=True, run='abc')
        device.status.return_value = ready
        return device, ready

    def test_not_ready_does_not_submit(self):
        device, _ = self.device()
        device.status.return_value = dict(ready=False)
        with self.assertRaisesRegex(RuntimeError, 'not ready'):
            console.send(device, ['heal'])
        device.submit.assert_not_called()

    @patch.object(console.time, 'sleep')
    @patch.object(console.uuid, 'uuid4', return_value=Mock(hex='req'))
    def test_waits_for_matching_terminal_receipt(self, uuid, sleep):
        device, ready = self.device()
        receipt = dict(request='req', state='ok')
        device.status.side_effect = [ready, dict(request='other', state='ok'),
                                     dict(request='req', state='transition'), receipt]
        self.assertEqual(console.send(device, ['scene', 'lut']), receipt)
        device.submit.assert_called_once_with('req abc scene lut\n')

    def test_timeout_never_retries_mutation(self):
        device, _ = self.device()
        with self.assertRaisesRegex(RuntimeError, 'Outcome unknown'):
            console.send(device, ['heal'], timeout=0)
        device.submit.assert_called_once()

    def test_submission_error_never_retries(self):
        device, _ = self.device()
        device.submit.side_effect = RuntimeError('busy')
        with self.assertRaisesRegex(RuntimeError, 'busy'):
            console.send(device, ['heal'])
        device.submit.assert_called_once()

    def test_lost_receipt_reports_uncertain_outcome(self):
        device, ready = self.device()
        device.status.side_effect = [ready, json.JSONDecodeError('bad receipt', '', 0)]
        with self.assertRaisesRegex(RuntimeError, 'outcome unknown'):
            console.send(device, ['heal'])
        device.submit.assert_called_once()


if __name__ == '__main__':
    unittest.main()
