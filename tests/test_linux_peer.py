"""Compile the real peer against offline verbs stubs and verify Linux role gates."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(shutil.which('cc'), 'C compiler required')
class LinuxPeerTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.work = tempfile.TemporaryDirectory(prefix='mcdma-linux-peer-')
        cls.addClassCleanup(cls.work.cleanup)
        work = Path(cls.work.name)
        # Reuse resource-lifetime stubs, with configurable hardware observations.
        stub = (ROOT / 'tests/rpcd_stub_verbs.c').read_text()
        # The Linux feature is independent of the separate GPU/RPC change.
        # Provide the same fault seams when the baseline fixture is older.
        stub = stub.replace('#include <infiniband/verbs.h>', '#include <infiniband/verbs.h>\n#include <errno.h>', 1)
        if 'getenv("STUB_FAIL_DESTROY_QP")' not in stub:
            stub = stub.replace('q->destroyed = 1;',
                                'if (getenv("STUB_FAIL_DESTROY_QP")) return EBUSY;\n    q->destroyed = 1;')
        if 'getenv("STUB_FAIL_DESTROY_CQ")' not in stub:
            stub = stub.replace('c->destroyed = 1;',
                                'if (getenv("STUB_FAIL_DESTROY_CQ")) return EBUSY;\n    c->destroyed = 1;')
        old_deregister = 'int ibv_dereg_mr(struct ibv_mr *m) { free(m); return 0; }'
        if old_deregister in stub:
            stub = stub.replace(old_deregister,
                                'int ibv_dereg_mr(struct ibv_mr *m) { if (getenv("STUB_FAIL_DEREG_MR")) return EBUSY; peer_test_payload_registered=0; free(m); return 0; }')
        stub = stub.replace('static struct ibv_device g_dev[2];', '''
static void *peer_test_payload;
static int peer_test_payload_registered;
static struct ibv_device g_dev[2];
''')
        stub = stub.replace('m->context = pd->context;', 'peer_test_payload=addr; peer_test_payload_registered=1; m->context = pd->context;')
        stub = stub.replace('((struct spd *)m->pd)->mrs--;', 'peer_test_payload_registered=0; ((struct spd *)m->pd)->mrs--;')
        stub = stub.replace('p->active_mtu = IBV_MTU_4096;', '''
    p->active_mtu = getenv("STUB_SMALL_MTU") ? IBV_MTU_1024 : IBV_MTU_4096;
    p->max_mtu = p->active_mtu;
    p->state = getenv("STUB_PORT_DOWN") ? IBV_PORT_DOWN : IBV_PORT_ACTIVE;
    p->link_layer = getenv("STUB_INFINIBAND") ? IBV_LINK_LAYER_INFINIBAND : IBV_LINK_LAYER_ETHERNET;
    fprintf(stderr, "STUB_PORT observed=%u\\n", (unsigned)port);
''')
        stub += '''
int ibv_query_device(struct ibv_context *ctx, struct ibv_device_attr *attr) {
    (void)ctx;
    memset(attr, 0, sizeof(*attr));
    attr->vendor_id = getenv("STUB_OTHER_VENDOR") ? 0x1234 : getenv("STUB_LEGACY_PCI_VENDOR") ? 0x15b3 : 0x02c9;
    attr->vendor_part_id = getenv("STUB_NATIVE_CX5") ? 0x1019 : 0x1021;
    return 0;
}
#undef free
extern void free(void *memory);
void mcdma_test_free(void *memory) {
    if (memory && memory==peer_test_payload) {
        if (peer_test_payload_registered) abort();
        fputs("STUB_PAYLOAD_RELEASE registration=ended\\n", stderr);
        peer_test_payload=NULL;
    }
    free(memory);
}
'''
        stub_path = work / 'verbs-stub.c'
        stub_path.write_text(stub)
        cls.linux = work / 'linux-peer'
        flags = ['cc', '-std=c11', '-O1', '-Wall', '-Wextra', '-Werror', '-Dfree=mcdma_test_free']
        if sys.platform == 'darwin':
            flags.append('-D_DARWIN_C_SOURCE=1')
        # On macOS this compiles the Linux-only application branch against the
        # installed verbs declarations; no Linux runtime or device is emulated.
        compiled = subprocess.run([*flags, '-U__APPLE__', '-D__linux__', str(ROOT / 'peer/verbs_peer.c'),
                                   str(stub_path), '-o', str(cls.linux)], capture_output=True, text=True)
        if compiled.returncode:
            raise RuntimeError(compiled.stderr[-6000:])
        cls.native = work / 'native-peer'
        compiled = subprocess.run([*flags, str(ROOT / 'peer/verbs_peer.c'), str(stub_path), '-o', str(cls.native)],
                                  capture_output=True, text=True)
        if compiled.returncode:
            raise RuntimeError(compiled.stderr[-6000:])

    def run_peer(self, args, extra_env=None, native=False):
        env = {key: value for key, value in os.environ.items() if not key.startswith(('STUB_', 'MCDMA_'))}
        env.update(extra_env or {})
        return subprocess.run([str(self.native if native else self.linux), *args], input='',
                              capture_output=True, text=True, timeout=5, env=env)

    def test_version_requires_no_device_or_configuration(self):
        done = self.run_peer(['--version'], {'MCDMA_RDMA_PORT': 'invalid', 'STUB_OTHER_VENDOR': '1'})
        self.assertEqual(done.returncode, 0)
        self.assertEqual(done.stdout, 'MCDMA_VERBS_PEER abi=1 platform=linux stock_initiator=1 stock_responder=1\n')
        self.assertEqual(done.stderr, '')

    def test_native_platform_version_preserves_the_macos_boundary(self):
        done = self.run_peer(['--version'], native=True)
        self.assertEqual(done.returncode, 0)
        expected = 'macos stock_initiator=0' if sys.platform == 'darwin' else 'linux stock_initiator=1 stock_responder=1'
        self.assertEqual(done.stdout, 'MCDMA_VERBS_PEER abi=1 platform=' + expected + '\n')
        if sys.platform == 'darwin':
            rejected = self.run_peer(['stub0', '0', 'stock-initiator'], native=True)
            self.assertEqual(rejected.returncode, 2)
            self.assertIn('Usage:', rejected.stderr)
            self.assertNotIn('ENDPOINT', rejected.stdout)

    def test_stock_linux_accepts_cx7_but_original_native_guard_still_rejects_it(self):
        done = self.run_peer(['stub0', '0', 'stock-initiator'])
        self.assertIn('ENDPOINT ', done.stdout)
        self.assertIn('LINUX_PEER_CONFIG backend=stock-libibverbs vendor_id=0x2c9 rdma_port=1 role=stock-initiator', done.stderr)
        rejected = self.run_peer(['stub0', '0', 'initiator'])
        self.assertEqual(rejected.returncode, 2)
        self.assertIn('Native peer must be a supported ConnectX Ethernet device', rejected.stderr)
        self.assertNotIn('ENDPOINT ', rejected.stdout)
        native_oui = self.run_peer(['stub0', '0', 'initiator'], {'STUB_NATIVE_CX5': '1'})
        self.assertEqual(native_oui.returncode, 2)
        self.assertNotIn('ENDPOINT ', native_oui.stdout)
        accepted = self.run_peer(['stub0', '0', 'initiator'], {'STUB_NATIVE_CX5': '1', 'STUB_LEGACY_PCI_VENDOR': '1'})
        self.assertIn('ENDPOINT ', accepted.stdout)

    def test_stock_linux_accepts_the_legacy_pci_vendor_value_and_reports_it(self):
        done = self.run_peer(['stub0', '0', 'stock-initiator'], {'STUB_LEGACY_PCI_VENDOR': '1'})
        self.assertIn('ENDPOINT ', done.stdout)
        self.assertIn('vendor_id=0x15b3 rdma_port=1 role=stock-initiator', done.stderr)

    def test_soft_opt_in_admits_other_vendor_and_labels_the_evidence_line(self):
        done = self.run_peer(['stub0', '0', 'stock-initiator'],
                             {'STUB_OTHER_VENDOR': '1', 'MCDMA_SOFT_TRANSPORT': '1'})
        self.assertIn('ENDPOINT ', done.stdout)
        self.assertIn('soft_transport=1 vendor_id=0x1234 rdma_port=1 role=stock-initiator', done.stderr)

    def test_soft_opt_in_alone_does_not_change_the_default_hardware_gate(self):
        done = self.run_peer(['stub0', '0', 'stock-responder'], {'STUB_OTHER_VENDOR': '1'})
        self.assertEqual(done.returncode, 2)
        self.assertIn('MCDMA_SOFT_TRANSPORT=1', done.stderr)
        self.assertNotIn('ENDPOINT ', done.stdout)

    def test_stock_responder_checks_hardware_without_changing_the_original_native_guard(self):
        accepted = self.run_peer(['stub0', '0', 'stock-responder'])
        self.assertIn('ENDPOINT ', accepted.stdout)
        self.assertIn('vendor_id=0x2c9 rdma_port=1 role=stock-responder', accepted.stderr)
        for fault in ['STUB_OTHER_VENDOR', 'STUB_PORT_DOWN', 'STUB_INFINIBAND']:
            with self.subTest(fault=fault):
                rejected = self.run_peer(['stub0', '0', 'stock-responder'], {fault: '1'})
                self.assertEqual(rejected.returncode, 2)
                self.assertNotIn('ENDPOINT ', rejected.stdout)
        original = self.run_peer(['stub0', '0', 'responder'])
        self.assertEqual(original.returncode, 2)
        self.assertIn('Native peer must be a supported ConnectX Ethernet device', original.stderr)
        self.assertNotIn('ENDPOINT ', original.stdout)
        if sys.platform == 'darwin':
            unavailable = self.run_peer(['stub0', '0', 'stock-responder'], native=True)
            self.assertEqual(unavailable.returncode, 2)
            self.assertIn('Usage:', unavailable.stderr)
            self.assertNotIn('ENDPOINT ', unavailable.stdout)

    def test_stock_role_refuses_other_vendor_inactive_port_and_non_ethernet(self):
        for fault in ['STUB_OTHER_VENDOR', 'STUB_PORT_DOWN', 'STUB_INFINIBAND']:
            with self.subTest(fault=fault):
                done = self.run_peer(['stub0', '0', 'stock-initiator'], {fault: '1'})
                self.assertEqual(done.returncode, 2)
                self.assertNotIn('ENDPOINT ', done.stdout)

    def test_mtu_and_requested_port_are_checked_before_registration(self):
        rejected = self.run_peer(['stub0', '0', 'stock-initiator'],
                                 {'STUB_SMALL_MTU': '1', 'MCDMA_PATH_MTU': '4096'})
        self.assertEqual(rejected.returncode, 2)
        self.assertIn('Requested path MTU exceeds', rejected.stderr)
        self.assertNotIn('ENDPOINT ', rejected.stdout)
        done = self.run_peer(['stub0', '0', 'stock-initiator'], {'MCDMA_RDMA_PORT': '2'})
        self.assertIn('STUB_PORT observed=2', done.stderr)
        self.assertIn('rdma_port=2 role=stock-initiator', done.stderr)
        for port in ['0', '256', '-1', '2x', '+2', ' 2']:
            with self.subTest(port=port):
                invalid = self.run_peer(['stub0', '0', 'stock-initiator'], {'MCDMA_RDMA_PORT': port})
                self.assertEqual(invalid.returncode, 2)
                self.assertNotIn('STUB_PORT observed=', invalid.stderr)

    def test_bad_gid_index_is_refused_without_creating_registered_memory(self):
        for gid in ['-1', '256', '9x', '+9', ' 9']:
            with self.subTest(gid=gid):
                done = self.run_peer(['stub0', gid, 'stock-initiator'])
                self.assertEqual(done.returncode, 2)
                self.assertIn('GID index must be', done.stderr)
                self.assertNotIn('ENDPOINT ', done.stdout)

    def test_cleanup_failure_never_frees_registered_payload_or_reports_success(self):
        positive = self.run_peer(['stub0', '0', 'resources'])
        self.assertEqual(positive.returncode, 0, positive.stderr)
        self.assertIn('cleanup=0 transfer_test=0', positive.stdout)
        self.assertIn('STUB_PAYLOAD_RELEASE registration=ended', positive.stderr)
        for fault in ['STUB_FAIL_DESTROY_QP', 'STUB_FAIL_DESTROY_CQ', 'STUB_FAIL_DEREG_MR']:
            with self.subTest(fault=fault):
                failed = self.run_peer(['stub0', '0', 'resources'], {fault: '1'})
                self.assertEqual(failed.returncode, 2, failed.stderr)
                self.assertNotIn('cleanup=0', failed.stdout)
                self.assertNotIn('STUB_PAYLOAD_RELEASE', failed.stderr)
                self.assertNotIn('STUB-VIOLATION', failed.stderr)


if __name__ == '__main__':
    unittest.main()
