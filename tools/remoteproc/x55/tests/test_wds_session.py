#!/usr/bin/python3
# SPDX-License-Identifier: GPL-2.0-only
"""Synthetic QMI datagrams only; no modem, netlink, firmware or raw NV access."""
import importlib.util
import ipaddress
import json
from pathlib import Path
import socket
import struct
import sys
import tempfile
import threading
import unittest

spec = importlib.util.spec_from_file_location('wds_session', Path(__file__).parents[1] / 'wds_session.py')
wds = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = wds
spec.loader.exec_module(wds)


def ipv4(value):
    return wds.u32(int(ipaddress.IPv4Address(value)))


def config():
    return {0x1E: ipv4('198.18.0.2'), 0x20: ipv4('198.18.0.1'),
            0x21: ipv4('255.255.255.252'), 0x15: ipv4('192.0.2.53'),
            0x29: wds.u32(1500), 0x2B: b'\x04', 0x1F: b'\x00\x03'}


class Tests(unittest.TestCase):
    def sockets(self):
        a, b = socket.socketpair(socket.AF_UNIX, socket.SOCK_DGRAM)
        a.setblocking(False)
        b.settimeout(1)
        self.addCleanup(a.close)
        self.addCleanup(b.close)
        return a, b

    def test_codec(self):
        p = wds.packet(2, 65535, 0xA2, [(2, bytes(4)), (0x1E, ipv4('198.18.0.2'))])
        flags, tid, msg, data = wds.decode(p)
        self.assertEqual((flags, tid, msg), (2, 65535, 0xA2))
        self.assertEqual(str(wds.ipv4(data, 0x1E)), '198.18.0.2')
        wds.check_result(msg, data)

    def test_bad_packets(self):
        good = wds.packet(2, 1, 0x20, [(2, bytes(4))])
        for p in (b'', good[:-1], good + b'x', bytes([6]) + good[1:],
                  wds.HEADER.pack(2, 1, 0x20, 2) + b'\x02\x00',
                  wds.packet(2, 1, 0x20, [(2, bytes(4)), (2, bytes(4))])):
            with self.subTest(packet=p), self.assertRaises(wds.ProtocolError):
                wds.decode(p)

    def test_oversized_encode(self):
        with self.assertRaises(wds.ProtocolError):
            wds.packet(0, 1, 1, [(1, bytes(65530))])

    def test_settings_endian_and_limits(self):
        self.assertEqual(wds.settings(config()), dict(address='198.18.0.2', prefix=30,
                         gateway='198.18.0.1', dns=['192.0.2.53'], mtu=1500))
        cases = ((0x1E, ipv4('127.0.0.1')), (0x20, ipv4('224.0.0.1')),
                 (0x21, ipv4('255.0.255.0')), (0x29, wds.u32(16000)),
                 (0x29, wds.u32(575)), (0x2B, b'\x06'), (0x15, ipv4('0.0.0.0')))
        for k, v in cases:
            d = config(); d[k] = v
            if k == 0x29 and v == wds.u32(16000):
                self.assertEqual(wds.settings(d)['mtu'], 1500)
            else:
                with self.subTest(key=k, value=v), self.assertRaises(wds.ProtocolError):
                    wds.settings(d)
        d = config(); d.pop(0x1E)
        with self.assertRaises(wds.ProtocolError):
            wds.settings(d)

    def test_persistent_lifecycle_and_wire_requests(self):
        a, b = self.sockets()
        messages, errors = [], []
        def peer():
            connected = False
            try:
                for _ in range(7):
                    flag, tid, msg, values = wds.decode(b.recv(65535))
                    self.assertEqual(flag, 0)
                    messages.append(msg)
                    reply = [(2, bytes(4))]
                    if msg == wds.STATUS:
                        reply.append((1, bytes([2 if connected else 1])))
                    elif msg == wds.BIND:
                        self.assertEqual(values[0x10], struct.pack('<II', 3, 4))
                        self.assertEqual(values[0x11], b'\x01')
                        self.assertEqual(values[0x13], wds.u32(1))
                    elif msg == wds.IP_FAMILY:
                        self.assertEqual(values, {1: b'\x04'})
                    elif msg == wds.START:
                        self.assertEqual(values, {0x31: b'\x03', 0x19: b'\x04'})
                        reply.append((1, wds.u32(0x12345678)))
                        connected = True
                    elif msg == wds.SETTINGS:
                        self.assertEqual(values[0x10], wds.u32(wds.SETTINGS_MASK))
                        reply.extend(config().items())
                    elif msg == wds.STOP:
                        self.assertEqual(values, {1: wds.u32(0x12345678)})
                        connected = False
                    else:
                        self.fail('Unexpected message')
                    # Unsolicited status is not mistaken for a transaction reply.
                    b.send(wds.packet(4, 0, wds.STATUS, [(1, b'\x02\x00')]))
                    b.send(wds.packet(2, tid, msg, reply))
            except Exception as e:
                errors.append(e)
        t = threading.Thread(target=peer)
        t.start()
        health_checks = []
        client = wds.Client(a, lambda: health_checks.append(1), timeout=0.5)
        session = wds.Session(client)
        result = session.start(4, 1, 3)
        self.assertEqual(result['address'], '198.18.0.2')
        with self.assertRaises(wds.ProtocolError):
            session.start(4, 1, 3)
        session.stop()
        with self.assertRaises(wds.ProtocolError):
            session.stop()
        t.join(2)
        self.assertFalse(t.is_alive())
        self.assertEqual(errors, [])
        self.assertEqual(messages, [wds.STATUS, wds.BIND, wds.IP_FAMILY,
                                   wds.START, wds.STATUS, wds.SETTINGS, wds.STOP])
        self.assertEqual(client.indications, 7)
        self.assertEqual(len(health_checks), 7)

    def test_timeout_does_not_retry(self):
        a, b = self.sockets()
        with self.assertRaises(TimeoutError):
            wds.Client(a, timeout=0.02).request(wds.STATUS)
        self.assertEqual(wds.decode(b.recv(65535))[2], wds.STATUS)
        b.setblocking(False)
        with self.assertRaises(BlockingIOError):
            b.recv(65535)

    def test_wrong_transaction_rejected(self):
        a, b = self.sockets()
        b.send(wds.packet(2, 99, wds.STATUS, [(2, bytes(4))]))
        with self.assertRaises(wds.ProtocolError):
            wds.Client(a, timeout=0.1).request(wds.STATUS)

    def test_error_response(self):
        a, b = self.sockets()
        b.send(wds.packet(2, 1, wds.START, [(2, struct.pack('<HH', 1, 14))]))
        with self.assertRaises(wds.QmiError) as ctx:
            wds.Client(a, timeout=0.1).request(wds.START)
        self.assertEqual(ctx.exception.code, 14)

    def test_unhealthy_prevents_sending(self):
        a, b = self.sockets()
        def unhealthy():
            raise wds.ProtocolError('hardware unhealthy')
        with self.assertRaises(wds.ProtocolError):
            wds.Client(a, unhealthy).request(wds.STATUS)
        b.setblocking(False)
        with self.assertRaises(BlockingIOError):
            b.recv(65535)

    def test_private_exclusive_state(self):
        root = Path(tempfile.mkdtemp(prefix='x55-wds-test-', dir='/tmp')) / 'state'
        s = wds.StateFile(root)
        try:
            s.update('connected', config={'address': '198.18.0.2'})
            self.assertEqual(json.loads((root/'status.json').read_text())['state'], 'connected')
            self.assertEqual((root/'status.json').stat().st_mode & 0o777, 0o600)
            with self.assertRaises(FileExistsError):
                wds.StateFile(root)
        finally:
            s.close()


if __name__ == '__main__':
    unittest.main(verbosity=2)
