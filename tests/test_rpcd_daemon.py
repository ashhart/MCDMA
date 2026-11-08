"""Exercise mcdma-rpcd's failure paths offline: the real daemon sources built against a stub verbs library, with its
setup exchange driven over the loopback interface's link-local address."""
import mmap
import os
import pathlib
import shutil
import signal
import socket
import struct
import subprocess
import tempfile
import time
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
MIB = 1 << 20
SOURCES = ['rpc/rpcd_common.c', 'rpc/rpcd_verbs.c', 'rpc/rpcd_listen.c', 'rpc/rpcd_connect.c',
           'rpc/link_verbs.c', 'rpc/link_tb.c', 'rpc/link_xchg.c']
OFFER, PING, PONG, BYE, ERR = 1, 2, 3, 4, 5
CONNECT, LISTEN = 1, 2
HAVE = 1


def loopback():
    """The loopback interface when it has a link-local address (lo0 on macOS), else None."""
    for name in ('lo0', 'lo'):
        try:
            index = socket.if_nametoindex(name)
        except OSError:
            continue
        probe = socket.socket(socket.AF_INET6, socket.SOCK_DGRAM)
        try:
            probe.bind(('fe80::1', 0, 0, index))
            return name
        except OSError:
            pass
        finally:
            probe.close()
    return None


LO = loopback()


def message(kind, name, sender, to=bytes(16), role=CONNECT, flags=0, body=b''):
    return b'MCDX' + bytes([1, kind, role, flags]) + name.encode().ljust(24, b'\0') + sender + to + body


def offer_body(req=4 * MIB, rep=4 * MIB, mode=1):
    keys = [99 + i for i in range((req + rep) // (4 * MIB))]
    gid = socket.inet_pton(socket.AF_INET6, 'fe80::1')
    return (struct.pack('<BBHIIII', 1, mode, 0, 5, 0, 6, 0) + gid + struct.pack('<QQ', req, rep) +
            struct.pack('<QQQI', 12345, req + rep, 4 * MIB, len(keys)) + b''.join(struct.pack('<I', k) for k in keys))


class Peer:
    """A connect end as the listen daemon sees one: offers, confirmations, pings and goodbyes from one session."""

    def __init__(self, port, name, hops=255, source='fe80::1'):
        self.port, self.name, self.session = port, name, os.urandom(16)
        self.index = socket.if_nametoindex(LO)
        self.sock = socket.socket(socket.AF_INET6, socket.SOCK_DGRAM)
        self.sock.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_UNICAST_HOPS, hops)
        self.sock.bind((source, 0, 0, self.index if source.startswith('fe80') else 0))
        self.sock.settimeout(0.5)
        self.listen_session = bytes(16)

    def send(self, kind, flags=0, body=b'', to=None):
        packet = message(kind, self.name, self.session, self.listen_session if to is None else to, flags=flags, body=body)
        self.sock.sendto(packet, ('fe80::1', self.port, 0, self.index))

    def receive(self, timeout=2.0):
        """The next datagram addressed to this session as (kind, sender session, text), or None."""
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            try:
                data = self.sock.recv(2048)
            except socket.timeout:
                continue
            if data[:4] == b'MCDX' and data[48:64] == self.session:
                return data[5], data[32:48], data[64:].split(b'\0', 1)[0].decode(errors='replace')
        return None

    def connect(self):
        """Offer, take the listen end's answer, and confirm: the daemon is armed afterwards."""
        self.send(OFFER, body=offer_body())
        answer = self.receive()
        if not answer or answer[0] != OFFER:
            raise AssertionError(f'no answer to an offer: {answer}')
        self.listen_session = answer[1]
        self.send(OFFER, flags=HAVE, body=offer_body())
        return answer

    def close(self):
        self.sock.close()


@unittest.skipUnless(shutil.which('cc'), 'C compiler required')
@unittest.skipUnless(LO, 'the loopback interface has no link-local address')
class DaemonTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.work = tempfile.mkdtemp(prefix='rpcd-', dir='/tmp')
        flags = ['cc', '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-DMCDMA_LINK_TEST_INTERFACES',
                 f'-DMCDMA_RPC_BOX_DIR="{cls.work}"', f'-DMCDMA_RPC_LOCK_DIR="{cls.work}"',
                 '-DMCDMA_RPC_HANDSHAKE_S=1', '-DMCDMA_RPC_LIVENESS_S=1']
        stub = ['tests/rpcd_stub_verbs.c']
        cls.daemon = os.path.join(cls.work, 'rpcd-stub')
        cls.harness = os.path.join(cls.work, 'connect-harness')
        subprocess.run([*flags, 'rpc/mcdma-rpcd.c', *SOURCES, *stub, '-o', cls.daemon], cwd=ROOT, check=True)
        subprocess.run([*flags, 'tests/test_rpcd_connect.c', *SOURCES, *stub, '-o', cls.harness], cwd=ROOT,
                       check=True)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.work, ignore_errors=True)

    def connect_scenario(self, mode):
        done = subprocess.run([self.harness, mode], capture_output=True, text=True, timeout=30)
        self.assertEqual(done.returncode, 0, done.stderr[-2000:])

    def test_a_request_staged_the_moment_the_link_is_up_is_served(self):
        self.connect_scenario('race')

    def test_shutdown_completes_when_the_listen_end_never_answers(self):
        self.connect_scenario('hang')

    def test_an_oversized_pull_reply_drops_the_link_instead_of_crashing(self):
        self.connect_scenario('pullcrash')

    def test_sigterm_during_a_call_stops_the_daemon_the_orderly_way(self):
        self.connect_scenario('sigterm')

    def test_a_goodbye_from_the_listen_end_drops_the_link_at_once(self):
        self.connect_scenario('bye')

    def test_a_listen_end_that_stops_answering_is_declared_down(self):
        self.connect_scenario('silent')

    # Listen end: the daemon under test, driven over its exchange port and socket.

    def start(self, name, hosts=None):
        port = free_port()
        sock = os.path.join(self.work, name + '.sock')
        env = dict(os.environ, MCDMA_RPCD_SOCKET=sock, STUB_NOCOPY='1')
        if hosts is not None:
            env['MCDMA_RPCD_HOSTS'] = hosts
        daemon = subprocess.Popen([self.daemon, 'listen', name, 'stub0', '0', '4096', f'{LO}:{port}'],
                                  stderr=subprocess.PIPE, env=env, text=True)
        self.addCleanup(stop, daemon, sock)
        deadline = time.monotonic() + 5
        while command(sock, 'STATUS') is None:
            self.assertLess(time.monotonic(), deadline, 'the listen daemon never answered')
            time.sleep(0.05)
        return daemon, port, sock, os.path.join(self.work, 'mcdma-rpc.' + name)

    def peer(self, port, name, **kwargs):
        peer = Peer(port, name, **kwargs)
        self.addCleanup(peer.close)
        return peer

    def test_a_second_daemon_for_the_same_link_leaves_the_live_one_alone(self):
        live, port, sock, box = self.start('dup')
        with open(box, 'r+b') as stream, mmap.mmap(stream.fileno(), 8 * MIB) as mailbox:
            mailbox[4096:4102] = b'MARKER'
            env = dict(os.environ, MCDMA_RPCD_SOCKET=os.path.join(self.work, 'dup2.sock'), STUB_NOCOPY='1')
            second = subprocess.run([self.daemon, 'listen', 'dup', 'stub0', '0', '4096', f'{LO}:{free_port()}'],
                                    capture_output=True, text=True, env=env, timeout=10)
            self.assertEqual(second.returncode, 2)
            self.assertIn('another mcdma-rpcd serves link dup', second.stderr)
            self.assertEqual(bytes(mailbox[4096:4102]), b'MARKER')
        self.assertIsNone(live.poll())
        self.assertIn('PEER dup down', command(sock, 'STATUS'))

    def test_status_names_the_peer_host_only_when_a_label_is_set(self):
        _, _, sock, _ = self.start('plain')
        self.assertNotIn('host=', command(sock, 'STATUS'))
        _, port, sock, _ = self.start('named', hosts='other=x,named=spark-a.example')
        self.assertIn('PEER named down', command(sock, 'STATUS'))
        self.assertIn(' link=roce host=spark-a.example via=', command(sock, 'STATUS'))
        self.peer(port, 'named').connect()
        deadline = time.monotonic() + 2
        while 'PEER named up' not in command(sock, 'STATUS'):
            self.assertLess(time.monotonic(), deadline, 'the connect end never armed the link')
            time.sleep(0.05)
        self.assertIn(' host=spark-a.example via=', command(sock, 'STATUS'))

    def test_a_malformed_host_list_refuses_to_start(self):
        env = dict(os.environ, MCDMA_RPCD_SOCKET=os.path.join(self.work, 'badhosts.sock'), STUB_NOCOPY='1',
                   MCDMA_RPCD_HOSTS='badhosts=spark a')
        refused = subprocess.run([self.daemon, 'listen', 'badhosts', 'stub0', '0', '4096', f'{LO}:{free_port()}'],
                                 capture_output=True, text=True, env=env, timeout=10)
        self.assertEqual(refused.returncode, 2)
        self.assertIn('MCDMA_RPCD_HOSTS must be NAME=HOST[,NAME=HOST]', refused.stderr)

    def test_silent_socket_clients_cannot_hold_shutdown_off(self):
        daemon, _, sock, _ = self.start('quiet')
        idle = []
        for _ in range(4):
            client = socket.socket(socket.AF_UNIX)
            client.connect(sock)
            idle.append(client)
        try:
            deadline = time.monotonic() + 10
            while command(sock, 'SHUTDOWN') != 'BYE':
                self.assertLess(time.monotonic(), deadline, 'SHUTDOWN stayed locked out')
                time.sleep(0.2)
            self.assertEqual(daemon.wait(timeout=10), 0)
        finally:
            for client in idle:
                client.close()

    def test_a_session_that_never_confirms_gives_way(self):
        _, port, sock, _ = self.start('idle')
        squatter = self.peer(port, 'idle')
        squatter.send(OFFER, body=offer_body())
        self.assertEqual(squatter.receive()[0], OFFER)
        time.sleep(1.5)
        self.assertIn('PEER idle down', command(sock, 'STATUS'))
        mac = self.peer(port, 'idle')
        mac.connect()
        deadline = time.monotonic() + 2
        while 'PEER idle up' not in command(sock, 'STATUS'):
            self.assertLess(time.monotonic(), deadline, 'the real connect end never armed the link')
            mac.send(PING)
            time.sleep(0.05)

    def test_a_refused_new_session_disarms_the_link(self):
        daemon, port, sock, box = self.start('rehello')
        with register(sock) as service:
            mac = self.peer(port, 'rehello')
            mac.connect()
            restarted = self.peer(port, 'rehello')
            restarted.send(OFFER, body=offer_body(rep=8 * MIB))
            refusal = restarted.receive()
            self.assertEqual((refusal[0], refusal[2]), (ERR, 'mailbox sizes differ'))
            service.settimeout(5)
            self.assertEqual(service.recv(16), b'BYE\n')
            with open(box, 'r+b') as stream, mmap.mmap(stream.fileno(), 8 * MIB) as mailbox:
                struct.pack_into('<Q', mailbox, 4 * MIB + 128, (1 << 32) | 16)
                time.sleep(0.5)
            self.assertIsNone(daemon.poll(), 'the daemon died writing a reply without a queue pair')

    def test_a_goodbye_ends_the_service_registration(self):
        _, port, sock, _ = self.start('loss')
        with register(sock) as service:
            mac = self.peer(port, 'loss')
            mac.connect()
            time.sleep(0.2)
            mac.send(BYE)
            service.settimeout(5)
            self.assertEqual(service.recv(16), b'BYE\n')

    def test_a_silent_connect_end_loses_the_link(self):
        _, port, sock, _ = self.start('mute')
        with register(sock) as service:
            mac = self.peer(port, 'mute')
            mac.connect()
            service.settimeout(5)
            started = time.monotonic()
            self.assertEqual(service.recv(16), b'BYE\n')
            self.assertLess(time.monotonic() - started, 4)

    def test_offers_from_off_the_link_are_ignored(self):
        _, port, _, _ = self.start('fence')
        routed = self.peer(port, 'fence', hops=64)
        routed.send(OFFER, body=offer_body())
        self.assertIsNone(routed.receive(0.6), 'an offer with hop limit 64 was answered')
        foreign = self.peer(port, 'other')
        foreign.send(OFFER, body=offer_body())
        self.assertIsNone(foreign.receive(0.6), "another link's offer was answered")
        self.assertEqual(self.peer(port, 'fence').connect()[0], OFFER)

    def test_a_ping_or_confirmation_from_an_unknown_session_hears_goodbye(self):
        _, port, sock, _ = self.start('stale')
        stale = self.peer(port, 'stale')
        stale.send(PING, to=os.urandom(16))
        self.assertEqual(stale.receive()[0], BYE)
        stale.send(OFFER, flags=HAVE, body=offer_body(), to=os.urandom(16))
        self.assertEqual(stale.receive()[0], BYE)
        self.assertIn('PEER stale down', command(sock, 'STATUS'))

    def test_sigterm_stops_cleanly_and_sighup_is_ignored(self):
        daemon, _, sock, box = self.start('signals')
        daemon.send_signal(signal.SIGHUP)
        time.sleep(0.3)
        self.assertIsNone(daemon.poll(), 'SIGHUP from a closed terminal must not stop the daemon')
        daemon.send_signal(signal.SIGTERM)
        self.assertEqual(daemon.wait(timeout=10), 0)
        self.assertFalse(os.path.exists(sock))
        self.assertFalse(os.path.exists(box))


def free_port():
    with socket.socket(socket.AF_INET6, socket.SOCK_DGRAM) as probe:
        probe.bind(('::', 0))
        return probe.getsockname()[1]


def command(sock, text):
    try:
        with socket.socket(socket.AF_UNIX) as client:
            client.settimeout(2)
            client.connect(sock)
            client.sendall(text.encode() + b'\n')
            answer = b''
            while chunk := client.recv(4096):
                answer += chunk
            return answer.decode().strip()
    except OSError:
        return None


class register:
    """A service registered with the listen daemon, as an application would."""

    def __init__(self, sock):
        self.client = socket.socket(socket.AF_UNIX)
        self.client.connect(sock)
        self.client.sendall(b'MODE poll\n')
        self.client.settimeout(5)
        if self.client.recv(3) != b'OK\n':
            raise AssertionError('the service was not registered')

    def __enter__(self):
        return self.client

    def __exit__(self, *_):
        self.client.close()


def stop(daemon, sock):
    if daemon.poll() is None:
        command(sock, 'SHUTDOWN')
        try:
            daemon.wait(timeout=10)
        except subprocess.TimeoutExpired:
            daemon.kill()
            daemon.wait()
    daemon.stderr.close()


if __name__ == '__main__':
    unittest.main()
