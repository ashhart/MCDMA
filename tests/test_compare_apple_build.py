"""The build comparator must reject changes to device-op callback ABI evidence."""
import contextlib
import importlib.util
import io
import pathlib
import sys
import tempfile
import unittest
from unittest import mock


ROOT = pathlib.Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location('compare_apple_build', ROOT / 'tools/compare-apple-build.py')
COMPARE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(COMPARE)


def functions(offset='#0x38', discriminator='#0x1234', count=1):
    return {
        '__ib_alloc_device': ['cmp x0, #0x400'],
        '_device_op': [f'ldr x8, [x0, {offset}]', f'mov x17, {discriminator}',
                       'blraa x8, x17'] * count,
    }


class CallbackCompatibilityTests(unittest.TestCase):
    def compare(self, old, new):
        with tempfile.TemporaryDirectory() as out:
            argv = ['compare-apple-build.py', '--old', 'old', '--new', 'new',
                    '--kext', 'fixture.kext', '--out', out]
            builds = [{'com.apple.iokit.IORDMAFamily': 'old'},
                      {'com.apple.iokit.IORDMAFamily': 'new'}]
            output = io.StringIO()
            with mock.patch.object(sys, 'argv', argv), \
                    mock.patch.object(COMPARE, 'load_build', side_effect=builds), \
                    mock.patch.object(COMPARE, 'exports', return_value=set()), \
                    mock.patch.object(COMPARE, 'xcrun', return_value=''), \
                    mock.patch.object(COMPARE, 'normalize', side_effect=[old, new]), \
                    contextlib.redirect_stdout(output):
                status = COMPARE.main()
            return status, output.getvalue()

    def test_matching_callbacks_pass(self):
        status, output = self.compare(functions(), functions())
        self.assertEqual(status, 0)
        self.assertIn('RESULT: no mechanical incompatibility found', output)

    def test_offset_and_pac_changes_fail_even_when_call_totals_match(self):
        for new in (functions(offset='#0x40'), functions(discriminator='#0x5678')):
            with self.subTest(new=new):
                status, output = self.compare(functions(), new)
                self.assertEqual(status, 1)
                self.assertIn('device-op call sites: 1 old, 1 new', output)
                self.assertIn('RESULT: PROBLEMS', output)
                self.assertIn('call site count differs at #0x38 disc #0x1234: 1 -> 0',
                              output.split('RESULT: PROBLEMS')[1])

    def test_added_removed_and_repeated_calls_fail(self):
        for old_count, new_count in ((0, 1), (1, 0), (1, 2), (2, 1)):
            with self.subTest(old_count=old_count, new_count=new_count):
                status, output = self.compare(functions(count=old_count), functions(count=new_count))
                self.assertEqual(status, 1)
                self.assertIn('RESULT: PROBLEMS', output)
                self.assertIn(f'disc #0x1234: {old_count} -> {new_count}',
                              output.split('RESULT: PROBLEMS')[1])


if __name__ == '__main__':
    unittest.main()
