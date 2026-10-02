#!/usr/bin/python3
# SPDX-License-Identifier: GPL-2.0-only
"""No modem/QRTR/NV access. AF_UNIX peers and fresh /tmp files only.
Test artifacts intentionally remain in /tmp; no filesystem cleanup/removal.
"""
import contextlib
import importlib.util
import json
import os
from pathlib import Path
import socket
import struct
import tempfile
import threading
import time
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('efs_sync', Path(__file__).parents[1] / 'efs_sync.py')
efs = importlib.util.module_from_spec(spec)
spec.loader.exec_module(efs)
BOOT = '00000000-0000-0000-0000-000000000001'


def hello(mode=0, version=2, minimum=1):
    return efs.frame(efs.HELLO, struct.pack('<10I', version, minimum, 4096, mode, *([0] * 6)))


def table_entry(bits, name=b'm9kefs1', base=0x10000, length=37):
    fmt = '<QQQ20s20s' if bits == 64 else '<III20s20s'
    return struct.pack(fmt, 1, base, length, b'synthetic', name)


def recv_exact(sock, length):
    out = b''
    while len(out) < length:
        data = sock.recv(length - len(out))
        if not data:
            raise EOFError()
        out += data
    return out


class Peer(threading.Thread):
    def __init__(self, sock, bits, table, blobs, truncate=False, reset_reply=True, dribble=0):
        super().__init__(daemon=True)
        self.sock, self.bits, self.table, self.blobs = sock, bits, table, blobs
        self.commands = []
        self.error = None
        self.truncate = truncate
        self.reset_reply = reset_reply
        self.dribble = dribble
        self.manifest_seen = None
        self.check_manifest = None

    def run(self):
        try:
            self.sock.settimeout(2)
            self.sock.sendall(hello())
            while True:
                cmd, length = struct.unpack('<II', recv_exact(self.sock, 8))
                body = recv_exact(self.sock, length - 8)
                self.commands.append(cmd)
                if cmd == efs.HELLO_RESP:
                    if struct.unpack('<10I', body)[:4] != (2, 1, 0, 2):
                        raise AssertionError('wrong HELLO response')
                    fmt, command = ('<QQ', efs.MEM_DEBUG64) if self.bits == 64 else ('<II', efs.MEM_DEBUG)
                    self.sock.sendall(efs.frame(command, struct.pack(fmt, 0x1000, len(self.table))))
                elif cmd in (efs.MEM_READ, efs.MEM_READ64):
                    address, count = struct.unpack('<QQ' if cmd == efs.MEM_READ64 else '<II', body)
                    if cmd != (efs.MEM_READ64 if self.bits == 64 else efs.MEM_READ):
                        raise AssertionError('wrong address width')
                    if address == 0x1000:
                        data = self.table[:count]
                    else:
                        base = next(b for b in self.blobs if b <= address < b + len(self.blobs[b]))
                        data = self.blobs[base][address-base:address-base+count]
                        if self.truncate:
                            self.sock.sendall(data[:1])
                            self.sock.shutdown(socket.SHUT_WR)
                            return
                    if len(data) != count:
                        raise AssertionError('out-of-range read')
                    # Exercise partial header/data reads without depending on
                    # stream socket coalescing or preserving message boundaries.
                    for i in range(0, len(data), 71):
                        if self.dribble:
                            time.sleep(self.dribble)
                        self.sock.sendall(data[i:i+71])
                elif cmd == efs.RESET:
                    if self.check_manifest:
                        self.manifest_seen = self.check_manifest()
                    self.sock.sendall(efs.frame(efs.RESET_RESP if self.reset_reply else efs.HELLO,
                                               b'' if self.reset_reply else hello()[8:]))
                    return
                else:
                    raise AssertionError(f'unexpected command {cmd}')
        except (EOFError, BrokenPipeError, ConnectionResetError):
            pass
        except BaseException as error:
            self.error = error


class Tests(unittest.TestCase):
    def setUp(self):
        self.root = Path(tempfile.mkdtemp(prefix='x55-efs-test-', dir='/tmp'))
        self.store = efs.Snapshots(self.root, BOOT)
        self.addCleanup(self.store.close)

    def link(self, bits=32, table=None, blobs=None, **kwargs):
        a, b = socket.socketpair()
        a.setblocking(False)
        peer = Peer(b, bits, table if table is not None else table_entry(bits),
                    blobs if blobs is not None else {0x10000: bytes(range(37))}, **kwargs)
        peer.check_manifest = lambda: bool(list(Path(self.store.path).glob('session-*/manifest.json')))
        peer.start()

        def close_peer():
            with contextlib.suppress(OSError):
                a.shutdown(socket.SHUT_RDWR)
            peer.join(2.5)
            a.close()
            b.close()
            self.assertFalse(peer.is_alive(), 'synthetic peer did not exit')

        self.addCleanup(close_peer)
        transport = efs.Transport(a.fileno(), io_timeout=0.5)
        packet = transport.packet(time.monotonic() + 1)
        return transport, peer, packet

    def complete(self, bits):
        data = bytes(range(256)) * 33 + b'end'
        table = table_entry(bits, length=len(data)) + table_entry(bits, b'm9kefsc', 0x20000, 5)
        t, peer, h = self.link(bits, table, {0x10000: data, 0x20000: b'abcde'})
        efs.session(t, self.store, h)
        peer.join(2)
        self.assertFalse(peer.is_alive())
        self.assertIsNone(peer.error)
        self.assertTrue(peer.manifest_seen)
        dest = Path(self.store.path) / 'session-0001'
        self.assertEqual((dest / 'm9kefs1').read_bytes(), data)
        self.assertEqual((dest / 'm9kefsc').read_bytes(), b'abcde')
        self.assertEqual((dest / 'm9kefs1').stat().st_mode & 0o777, 0o600)
        self.assertTrue((dest / 'complete.json').is_file())
        self.assertEqual(peer.commands.count(efs.RESET), 1)
        self.assertEqual(len(json.loads((dest / 'manifest.json').read_text())['regions']), 2)

    def test_session32(self):
        self.complete(32)

    def test_session64(self):
        self.complete(64)

    def test_oneplus_oem_region(self):
        t, peer, h = self.link(64, table_entry(64, b'oemnvbktmp'), {0x10000: bytes(range(37))})
        efs.session(t, self.store, h)
        peer.join(2)
        self.assertIsNone(peer.error)
        self.assertEqual((Path(self.store.path) / 'session-0001/oemnvbktmp').read_bytes(), bytes(range(37)))

    def test_session_deadline_not_extended_by_slow_peer(self):
        t, peer, h = self.link(table=table_entry(32, length=512),
                               blobs={0x10000: bytes(512)}, dribble=0.01)
        t.io_timeout = 1
        start = time.monotonic()
        with self.assertRaises(TimeoutError):
            efs.session(t, self.store, h, session_seconds=0.03)
        self.assertLess(time.monotonic() - start, 0.5)
        self.assertNotIn(efs.RESET, peer.commands)

    def test_port_discovery_requires_fusion_efs(self):
        pci = self.root / 'pci/0002:01:00.0'
        port = pci / 'mhi0/wwan/wwan8/wwan8efs0'
        port.mkdir(parents=True)
        (pci / 'vendor').write_text('0x17cb\n')
        (pci / 'device').write_text('0x0306\n')
        (pci / 'of_node').mkdir()
        (port / 'type').write_text('EFS\n')
        (port / 'dev').write_text('241:7\n')
        cls = self.root / 'class'
        cls.mkdir()
        (cls / port.name).symlink_to(port)
        self.assertIsNone(efs.discover_port(cls, self.root / 'dev'))
        (pci / 'of_node/esoc-0').write_bytes(b'\0\0\0\1')
        self.assertEqual(efs.discover_port(cls, self.root / 'dev'),
                         (self.root / 'dev/wwan8efs0', os.makedev(241, 7)))
        (port / 'type').write_text('QMI\n')
        self.assertIsNone(efs.discover_port(cls, self.root / 'dev'))
        (port / 'type').write_text('EFS\n')
        (cls / 'wwan9efs0').symlink_to(port)
        with self.assertRaises(efs.ProtocolError):
            efs.discover_port(cls, self.root / 'dev')

    def test_names(self):
        for name in (b'../m9kefs1', b'/dev/sda', b'mdm1m9kefs1', b'manifest.json',
                     b'unknown', b'', b'X' * 20, b'\xff'):
            with self.subTest(name=name), self.assertRaises(efs.ProtocolError):
                efs.decode_table(table_entry(32, name), 32)

    def test_region_bounds(self):
        for base, size in ((0, 10), (0xfffffff0, 32), (1, 0), (1, 2*efs.MIB+1)):
            with self.subTest(base=base, size=size), self.assertRaises(efs.ProtocolError):
                efs.decode_table(table_entry(32, base=base, length=size), 32)
        with self.assertRaises(efs.ProtocolError):
            efs.decode_table(table_entry(64, base=(1 << 64) - 2, length=3), 64)
        with self.assertRaises(efs.ProtocolError):
            efs.decode_table(table_entry(32, b'm9kefsc', length=4097), 32)

    def test_table_bounds(self):
        for data in (b'', b'x', table_entry(64) * (len(efs.REGIONS) + 1)):
            with self.assertRaises(efs.ProtocolError):
                efs.decode_table(data, 64)

    def test_duplicate_or_overlap(self):
        for table in (table_entry(32) * 2,
                      table_entry(32) + table_entry(32, b'm9kefs2')):
            with self.assertRaises(efs.ProtocolError):
                efs.decode_table(table, 32)

    def test_validate_whole_table_before_data(self):
        t, peer, h = self.link(table=table_entry(32) + table_entry(32, b'../unsafe', 0x20000))
        with self.assertRaises(efs.ProtocolError):
            efs.session(t, self.store, h)
        self.assertEqual(peer.commands, [efs.HELLO_RESP, efs.MEM_READ])
        self.assertEqual(list(Path(self.store.path).iterdir()), [])

    def test_unsupported_hello(self):
        for h in (hello(mode=3), hello(version=1), hello(minimum=3)):
            with self.assertRaises(efs.ProtocolError):
                efs.session(None, self.store, (efs.HELLO, h[8:]))

    def test_truncated_data_never_reset(self):
        t, peer, h = self.link(truncate=True)
        with self.assertRaises(ConnectionError):
            efs.session(t, self.store, h)
        self.assertNotIn(efs.RESET, peer.commands)
        self.assertFalse(list(Path(self.store.path).glob('*/manifest.json')))
        self.assertTrue(list(Path(self.store.path).glob('*/m9kefs1')))

    def test_wrong_reset_response_not_complete(self):
        t, peer, h = self.link(reset_reply=False)
        with self.assertRaises(efs.ProtocolError):
            efs.session(t, self.store, h)
        self.assertTrue(list(Path(self.store.path).glob('*/manifest.json')))
        self.assertFalse(list(Path(self.store.path).glob('*/complete.json')))

    def test_disk_failure_never_reset(self):
        t, peer, h = self.link()
        with patch.object(efs, 'write_all', side_effect=OSError('disk full')):
            with self.assertRaises(OSError):
                efs.session(t, self.store, h)
        self.assertNotIn(efs.RESET, peer.commands)

    def test_fsync_failure_never_reset(self):
        t, peer, h = self.link()
        with patch.object(efs.os, 'fsync', side_effect=OSError('fsync failure')):
            with self.assertRaises(OSError):
                efs.session(t, self.store, h)
        self.assertNotIn(efs.RESET, peer.commands)

    def test_no_output_overwrite_or_symlink(self):
        fd = self.store.begin([{'length': 1}])
        self.addCleanup(os.close, fd)
        path = Path(self.store.path) / 'session-0001'
        original = path / 'original'
        original.write_bytes(b'unchanged')
        (path / 'm9kefs1').symlink_to(original)
        with self.assertRaises(FileExistsError):
            efs.regular_new(fd, 'm9kefs1')
        self.assertEqual(original.read_bytes(), b'unchanged')
        with self.assertRaises(FileExistsError):
            efs.regular_new(fd, 'original')

    def test_root_symlink_or_permissions(self):
        target = self.root / 'link'
        target.symlink_to(Path(self.store.path))
        with self.assertRaises(OSError):
            efs.Snapshots(target, BOOT)
        other = self.root / 'public'
        other.mkdir(mode=0o755)
        # mkdir's mode is masked by the caller's umask (including private
        # 0077 test runs); explicitly construct the unsafe-permission case.
        other.chmod(0o755)
        with self.assertRaises(efs.ProtocolError):
            efs.Snapshots(other, BOOT)

    def test_quotas(self):
        self.store.byte_limit = 2
        with self.assertRaises(efs.ProtocolError):
            self.store.begin([{'length': 3}])
        self.store.session_limit = 0
        with self.assertRaises(efs.ProtocolError):
            self.store.begin([{'length': 1}])

    def test_timeout_and_packet_size(self):
        a, b = socket.socketpair()
        with a, b:
            a.setblocking(False)
            t = efs.Transport(a.fileno(), io_timeout=0.05)
            start = time.monotonic()
            with self.assertRaises(TimeoutError):
                t.read_exact(8, start + 0.05)
            self.assertLess(time.monotonic() - start, 0.5)
            b.sendall(struct.pack('<II', efs.HELLO, 0xffffffff))
            with self.assertRaises(efs.ProtocolError):
                t.packet(time.monotonic() + 0.5)

    def test_short_write_is_not_retried(self):
        a, b = socket.socketpair()
        with a, b:
            t = efs.Transport(a.fileno())
            with patch.object(efs.os, 'write', return_value=3) as writer:
                with self.assertRaises(efs.ProtocolError):
                    t.send(efs.frame(efs.RESET), time.monotonic() + 1)
                writer.assert_called_once()

    def test_wrong_device_types_rejected_before_open(self):
        regular = self.root / 'port'
        regular.write_bytes(b'not a device')
        for p in (regular, self.root / 'symlink'):
            if p != regular:
                p.symlink_to('/dev/null')
            with self.assertRaises(efs.ProtocolError):
                efs.open_port((p, os.makedev(1, 3)))
        self.assertEqual(regular.read_bytes(), b'not a device')

    def test_status_is_exclusive(self):
        path = self.root / 'status.json'
        s = efs.Status(path)
        self.addCleanup(s.close)
        s.update('ready')
        with self.assertRaises(FileExistsError):
            efs.Status(path)
        s.update('failed', reason='synthetic')
        self.assertEqual(json.loads(path.read_text())['state'], 'failed')


if __name__ == '__main__':
    unittest.main(verbosity=2)
