"""Offline pipe-liveness tests; no SSH, RDMA, timing samples or hardware."""
import importlib.util
from pathlib import Path
import subprocess
import sys
import threading
import unittest
from unittest.mock import Mock, call, patch

spec = importlib.util.spec_from_file_location('native_cross_host', Path(__file__).resolve().parents[1]/'tools/native_cross_host.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class EndpointTests(unittest.TestCase):
    def endpoint(self, script):
        process = subprocess.Popen([sys.executable, '-u', '-c', script], stdin=subprocess.PIPE,
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        log = []
        with patch.object(module.subprocess, 'Popen', return_value=process):
            endpoint = module.Endpoint('offline-child', ['unused'], log)
        self.addCleanup(lambda: process.poll() is None and process.kill())
        return endpoint, log

    def test_full_stderr_pipe_cannot_block_protocol_reply(self):
        endpoint, log = self.endpoint("import os,sys; os.write(2,b'x'*200000+b'\\n'); print('READY'); sys.stdin.readline()")
        with patch('builtins.print'):
            self.assertEqual(endpoint.line(timeout=5), 'READY')
            self.assertEqual(endpoint.stop(), 0)
        captured = ''.join(item.get('endpoint_stderr', '') for item in log)
        self.assertEqual(captured, 'x'*200000+'\n')

    def test_capture_limit_drains_but_fails_closed(self):
        endpoint, log = self.endpoint("import os,sys; os.write(2,b'x'*2200000); print('READY'); sys.stdin.readline()")
        with patch('builtins.print'):
            self.assertEqual(endpoint.line(timeout=5), 'READY')
            self.assertEqual(endpoint.stop(), -1)
        self.assertEqual(len(endpoint.stderr_data), endpoint.STDERR_LIMIT)
        self.assertTrue(any(item.get('stderr_capture_error') == 'limit exceeded' for item in log))

    def test_split_marker_survives_chunked_capture(self):
        endpoint, log = self.endpoint("import os,sys; os.write(2,b'MCDMA_CQ_'); os.write(2,b'OBSERVER mapped=0 reason=disabled\\n'); print('READY'); sys.stdin.readline()")
        with patch('builtins.print'):
            self.assertEqual(endpoint.line(timeout=5), 'READY')
            self.assertEqual(endpoint.stop(), 0)
        self.assertFalse(module.observer_marker(log, 'offline-child', '0'))

    def test_exited_endpoint_preserves_failure_diagnostics(self):
        endpoint, log = self.endpoint("import os; os.write(2,b'firmware failure\\n')")
        with patch('builtins.print'):
            with self.assertRaisesRegex(RuntimeError, 'exited before'):
                endpoint.line(timeout=5)
            self.assertEqual(endpoint.stop(), 0)
        self.assertEqual(log[-1]['endpoint_stderr'], 'firmware failure\n')

    def mocked_endpoint(self, waits):
        endpoint = object.__new__(module.Endpoint)
        endpoint.host = 'offline-child'
        endpoint.log = []
        endpoint.pending = b''
        endpoint.stderr_data = bytearray()
        endpoint.stderr_overflow = False
        endpoint.stderr_error = None
        endpoint.stderr_stop = threading.Event()
        endpoint.stderr_thread = Mock()
        endpoint.stderr_thread.is_alive.return_value = False
        endpoint.process = Mock()
        endpoint.process.pid = 4321
        endpoint.process.wait.side_effect = waits
        return endpoint

    def test_stop_normal_exit_never_forces_kill(self):
        endpoint = self.mocked_endpoint([0])
        self.assertEqual(endpoint.stop(), 0)
        endpoint.process.wait.assert_called_once_with(timeout=15)
        endpoint.process.terminate.assert_not_called()
        endpoint.process.kill.assert_not_called()

    def test_stop_timeout_uses_sigterm_then_exits_nonzero(self):
        endpoint = self.mocked_endpoint([subprocess.TimeoutExpired('ssh', 15), 143])
        self.assertEqual(endpoint.stop(), -1)
        self.assertEqual(endpoint.process.wait.call_args_list, [call(timeout=15), call(timeout=5)])
        endpoint.process.terminate.assert_called_once_with()
        endpoint.process.kill.assert_not_called()

    def test_stop_surviving_child_reports_pid_and_never_forces_kill(self):
        endpoint = self.mocked_endpoint([
            subprocess.TimeoutExpired('ssh', 15), subprocess.TimeoutExpired('ssh', 5)])
        with self.assertRaisesRegex(module.EndpointCleanupError, 'pid 4321 survived.*SIGTERM'):
            endpoint.stop()
        self.assertEqual(endpoint.process.wait.call_args_list, [call(timeout=15), call(timeout=5)])
        endpoint.process.terminate.assert_called_once_with()
        endpoint.process.kill.assert_not_called()


if __name__ == '__main__':
    unittest.main()
