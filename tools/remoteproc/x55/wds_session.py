#!/usr/bin/python3
# SPDX-License-Identifier: GPL-2.0-only
"""One explicit IPv4 WDS call on a persistent QRTR socket; no link/route changes.

Wire definitions: libqmi 1.34.0, commit 3f07d6e5b4677558543b3b4484ea88ad92257e92.
No CTL CID allocation is sent: QRTR clients are separate socket endpoints (as
in libqmi's qmi-endpoint-qrtr.c). A CID from another process is not a session.
This is a bounded manual bring-up tool, not a connection manager.
"""
import argparse
import fcntl
import ipaddress
import json
import os
from pathlib import Path
import select
import signal
import socket
import stat
import struct
import subprocess
import threading
import time

ROOT = Path('/opt/x55/bin')
STATE = Path('/run/x55-data-session')
SERVICES = ['x55-mdm-helper', 'x55-tqftpserv', 'x55-pd-mapper', 'x55-efs-sync']
HEADER = struct.Struct('<BHHH')
MAX_PACKET = 65535
PCIE = 3
BIND, IP_FAMILY, START, STOP, STATUS, SETTINGS = 0xA2, 0x4D, 0x20, 0x21, 0x22, 0x2D
SETTINGS_MASK = sum(1 << n for n in (0, 3, 4, 8, 9, 13, 15))


class ProtocolError(Exception):
    pass


class QmiError(Exception):
    def __init__(self, message, code, values):
        super().__init__(f'QMI message 0x{message:04x} failed, error={code}')
        self.code = code
        self.values = values


def u32(value):
    return struct.pack('<I', value)


def tlv(kind, value):
    if not 0 <= kind <= 255 or len(value) > 65535:
        raise ProtocolError('Invalid TLV bounds')
    return struct.pack('<BH', kind, len(value)) + value


def packet(flags, tid, message, fields=()):
    body = b''.join(tlv(k, v) for k, v in fields)
    if len(body) + HEADER.size > MAX_PACKET:
        raise ProtocolError('Oversized QMI packet')
    return HEADER.pack(flags, tid, message, len(body)) + body


def decode(data):
    if not HEADER.size <= len(data) <= MAX_PACKET:
        raise ProtocolError('Invalid packet size')
    flags, tid, message, length = HEADER.unpack_from(data)
    if length != len(data) - HEADER.size or flags not in (0, 2, 4):
        raise ProtocolError('Invalid packet flags/length')
    values = {}
    offset = HEADER.size
    while offset < len(data):
        if len(data) - offset < 3:
            raise ProtocolError('Truncated TLV header')
        kind, size = struct.unpack_from('<BH', data, offset)
        offset += 3
        if size > len(data) - offset or kind in values:
            raise ProtocolError('Truncated/duplicate TLV')
        values[kind] = data[offset:offset + size]
        offset += size
    return flags, tid, message, values


def field(values, kind, size):
    data = values.get(kind)
    if data is None or len(data) != size:
        raise ProtocolError(f'Missing/invalid TLV 0x{kind:02x}')
    return data


def check_result(message, values):
    result, error = struct.unpack('<HH', field(values, 2, 4))
    if result or error:
        raise QmiError(message, error, values)


class Client:
    """A single connected datagram socket for the entire WDS lifecycle."""
    def __init__(self, sock, health=lambda: None, timeout=22):
        self.sock = sock
        self.health = health
        self.timeout = timeout
        self.tid = 0
        self.indications = 0

    def request(self, message, fields=()):
        self.health()
        self.tid = self.tid % 65535 + 1
        data = packet(0, self.tid, message, fields)
        deadline = time.monotonic() + self.timeout
        while True:
            left = deadline - time.monotonic()
            if left <= 0 or not select.select([], [self.sock], [], left)[1]:
                raise TimeoutError('QMI transmit deadline')
            try:
                if self.sock.send(data) != len(data):
                    raise ProtocolError('Short QRTR datagram write')
                break
            except BlockingIOError:
                continue
        for _ in range(1024):
            left = deadline - time.monotonic()
            if left <= 0 or not select.select([self.sock], [], [], left)[0]:
                raise TimeoutError(f'QMI 0x{message:04x} response deadline; no retry')
            try:
                data, _, msgflags, _ = self.sock.recvmsg(MAX_PACKET)
            except BlockingIOError:
                continue
            if msgflags & socket.MSG_TRUNC:
                raise ProtocolError('Truncated QRTR datagram')
            flags, tid, reply, values = decode(data)
            if flags == 4:
                self.indications += 1
                continue
            if flags != 2 or tid != self.tid or reply != message:
                raise ProtocolError('Unmatched QMI response; refusing to continue')
            check_result(message, values)
            return values
        raise ProtocolError('Excessive unsolicited messages')


def ipv4(values, kind):
    return ipaddress.IPv4Address(struct.unpack('<I', field(values, kind, 4))[0])


def usable(address):
    return not (address.is_unspecified or address.is_loopback or
                address.is_multicast or address.is_link_local or
                int(address) == 0xffffffff)


def settings(values):
    addr = ipv4(values, 0x1E)
    mask = ipv4(values, 0x21)
    gateway = ipv4(values, 0x20)
    try:
        prefix = ipaddress.IPv4Network('0.0.0.0/' + str(mask)).prefixlen
    except ValueError as e:
        raise ProtocolError('Non-contiguous IPv4 mask') from e
    mtu = struct.unpack('<I', field(values, 0x29, 4))[0]
    if field(values, 0x2B, 1) != b'\x04' or not usable(addr) or not usable(gateway):
        raise ProtocolError('Invalid IPv4 bearer addresses/family')
    if not 576 <= mtu <= 16380 or not 1 <= prefix <= 32:
        raise ProtocolError('Invalid MTU/prefix')
    dns = [str(ipv4(values, k)) for k in (0x15, 0x16)
           if k in values and usable(ipv4(values, k))]
    if not dns:
        raise ProtocolError('No usable modem-provided DNS')
    return dict(address=str(addr), prefix=prefix, gateway=str(gateway),
                dns=dns, mtu=min(mtu, 1500))


class Session:
    def __init__(self, client):
        self.client = client
        self.handle = None
        self.start_sent = False

    def status(self):
        return field(self.client.request(STATUS), 1, 1)[0]

    def start(self, endpoint, mux, profile):
        if self.start_sent or self.handle is not None:
            raise ProtocolError('A second Start Network is forbidden')
        if not 1 <= mux <= 254 or not 1 <= profile <= 255 or endpoint != 4:
            raise ProtocolError('Unsupported endpoint/mux/profile')
        if self.status() != 1:
            raise ProtocolError('WDS client is not initially disconnected')
        self.client.request(BIND, [(0x10, struct.pack('<II', PCIE, endpoint)),
                                   (0x11, bytes([mux])), (0x13, u32(1))])
        self.client.request(IP_FAMILY, [(1, b'\x04')])
        self.start_sent = True
        values = self.client.request(START, [(0x31, bytes([profile])), (0x19, b'\x04')])
        self.handle = struct.unpack('<I', field(values, 1, 4))[0]
        if self.handle in (0, 0xffffffff):
            self.handle = None
            raise ProtocolError('Invalid packet handle; never send wildcard Stop')
        if self.status() != 2:
            raise ProtocolError('Bearer did not become connected')
        values = self.client.request(SETTINGS, [(0x10, u32(SETTINGS_MASK))])
        if 0x1F in values and field(values, 0x1F, 2) != bytes([0, profile]):
            raise ProtocolError('Unexpected active profile')
        return settings(values)

    def stop(self):
        if self.handle is None:
            raise ProtocolError('No owned handle to stop')
        self.client.request(STOP, [(1, u32(self.handle))])
        self.handle = None


def health():
    procs = [p for p in Path('/sys/class/remoteproc').glob('remoteproc*')
             if (p / 'name').read_text().strip().startswith('esoc')]
    if len(procs) != 1 or (procs[0] / 'state').read_text().strip() != 'running':
        raise ProtocolError('ESOC is not running')
    if (procs[0] / 'recovery').read_text().strip() != 'disabled':
        raise ProtocolError('Automatic recovery is not disabled')
    subprocess.run(['systemctl', 'is-active', '--quiet', *SERVICES], check=True, timeout=5)
    if json.loads(Path('/run/x55-efs/status.json').read_text()).get('state') not in ('ready', 'saving'):
        raise ProtocolError('EFS is not healthy')
    subprocess.run([str(ROOT / 'mdm_helper_native'), '--check-alive'],
                   check=True, timeout=3, stdout=subprocess.DEVNULL)


def discover():
    env = dict(os.environ, LD_LIBRARY_PATH='/opt/x55/lib')
    text = subprocess.check_output([str(ROOT / 'qrtr-lookup')], env=env, text=True, timeout=5)
    services = {1: set(), 2: set()}
    for line in text.splitlines():
        v = line.split()
        if len(v) >= 5 and all(x.isdecimal() for x in v[:5]):
            service, _, _, node, port = map(int, v[:5])
            if service in services:
                services[service].add((node, port))
    if any(len(v) != 1 for v in services.values()):
        raise ProtocolError('Missing/ambiguous WDS or DMS service')
    wds, dms = next(iter(services[1])), next(iter(services[2]))
    if wds[0] != dms[0]:
        raise ProtocolError('WDS/DMS are on different modem nodes')
    return wds


class StateFile:
    def __init__(self, root):
        root.mkdir(mode=0o700)  # Exclusive run; preserve previous failed attempts.
        self.fd = os.open(root / 'status.json', os.O_WRONLY | os.O_CREAT | os.O_EXCL |
                          os.O_NOFOLLOW | os.O_CLOEXEC, 0o600)
        if not stat.S_ISREG(os.fstat(self.fd).st_mode):
            raise ProtocolError('Status output is not regular')

    def update(self, state, **values):
        data = (json.dumps(dict(state=state, t=time.monotonic(), **values)) + '\n').encode()
        os.lseek(self.fd, 0, os.SEEK_SET)
        os.ftruncate(self.fd, 0)
        while data:
            n = os.write(self.fd, data)
            if n <= 0:
                raise OSError('Short status write')
            data = data[n:]
        os.fsync(self.fd)

    def close(self):
        os.close(self.fd)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('action', choices=('status', 'start'))
    ap.add_argument('--profile', type=int)
    ap.add_argument('--mux', type=int, default=1)
    ap.add_argument('--endpoint', type=int, default=4)
    ap.add_argument('--hold-seconds', type=int, default=900)
    args = ap.parse_args()
    if os.geteuid() or '-kebab-x55-' not in os.uname().release:
        ap.error('Root and the X55 test kernel are required')
    if args.action == 'start' and (args.profile is None or not 1 <= args.hold_seconds <= 900):
        ap.error('start needs a reviewed profile and a 1..900 second lifetime')
    os.umask(0o077)
    lock = os.open('/run/lock/x55-wds-session.lock', os.O_CREAT | os.O_RDWR | os.O_CLOEXEC, 0o600)
    sock = None
    state = None
    try:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        health()
        peer = discover()
        sock = socket.socket(socket.AF_QIPCRTR, socket.SOCK_DGRAM)
        sock.bind(sock.getsockname())  # Use the kernel's real local node, not node 0.
        sock.connect(peer)
        sock.setblocking(False)
        session = Session(Client(sock, health=health))
        if args.action == 'status':
            print(json.dumps(dict(peer=peer, connection_status=session.status())))
            return 0
        state = StateFile(STATE)
        state.update('starting', peer=peer, local=sock.getsockname())
        stop_event = threading.Event()
        for sig in (signal.SIGTERM, signal.SIGINT):
            signal.signal(sig, lambda *_: stop_event.set())
        try:
            config = session.start(args.endpoint, args.mux, args.profile)
            state.update('connected', config=config, peer=peer, local=sock.getsockname(),
                         packet_handle=session.handle, mux=args.mux, profile=args.profile)
            print(json.dumps(dict(event='connected', config=config)), flush=True)
            end = time.monotonic() + args.hold_seconds
            while time.monotonic() < end and not stop_event.wait(min(15, end - time.monotonic())):
                if session.status() not in (2, 3):
                    raise ProtocolError('Data session lost; no reconnect')
            session.stop()
            state.update('stopped')
            print('{"event":"stopped"}', flush=True)
            return 0
        except Exception as e:
            state.update('failed', reason=str(e), packet_handle=session.handle,
                         start_sent=session.start_sent)
            print(json.dumps(dict(event='failed', reason=str(e))), flush=True)
            if isinstance(e, QmiError):
                # Error reasons only, no APN credentials or arbitrary payload dump.
                for key, fmt in ((0x10, '<H'), (0x11, '<Hh')):
                    if key in e.values and len(e.values[key]) == struct.calcsize(fmt):
                        print(json.dumps(dict(error_tlv=key, value=struct.unpack(fmt, e.values[key]))), flush=True)
            if session.handle is not None:
                # Reference hardware can crash if Stop is sent after a DL wedge.
                # Keep the same socket parked for explicit operator review instead
                # of blindly stopping/retrying/resetting an unhealthy connection.
                # A termination request may already have initiated the failed
                # Stop. Require a *new* explicit operator signal to release the
                # parked socket; services should use SendSIGKILL=no as well.
                stop_event.clear()
                print('{"event":"quarantined","note":"socket retained; operator review required"}', flush=True)
                state.update('quarantined', reason=str(e), packet_handle=session.handle)
                while not stop_event.wait(60):
                    pass
            return 1
    finally:
        if state is not None:
            state.close()
        if sock is not None:
            sock.close()
        os.close(lock)


if __name__ == '__main__':
    raise SystemExit(main())
