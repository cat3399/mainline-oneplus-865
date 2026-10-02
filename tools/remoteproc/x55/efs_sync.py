#!/usr/bin/python3
# SPDX-License-Identifier: GPL-2.0-only
"""Bounded, file-only SDX55 EFS synchronization for manual kebab bring-up.

Protocol reference: Apollo tools/remoteproc/mhi_efs_sync.c, fd5210de352d.
Stock OnePlus mdm_helper launches ks -m on MHI pipe_10 after boot. This
implementation deliberately has NO raw-partition writer, image-upload path,
command-execution path, or automatic reconnect/reset-on-error policy.
"""
import argparse
import contextlib
import errno
import hashlib
import json
import os
from pathlib import Path
import select
import signal
import stat
import struct
import sys
import time
import uuid

HELLO, HELLO_RESP, RESET, RESET_RESP = 1, 2, 7, 8
MEM_DEBUG, MEM_READ, MEM_DEBUG64, MEM_READ64 = 9, 10, 16, 17
MEMORY_MODE = 2
MIB = 1024 * 1024
REGIONS = {'m9kefs1': 2 * MIB, 'm9kefs2': 2 * MIB,
           'm9kefs3': 2 * MIB, 'm9kefsc': 4096, 'oemnvbktmp': 2 * MIB}
CHUNK = 8192
PACKET_LENGTHS = {HELLO: 48, MEM_DEBUG: 16, MEM_DEBUG64: 24, RESET_RESP: 8}
STATE_ROOT = Path('/var/lib/x55-efs')
STATUS_PATH = Path('/run/x55-efs/status.json')


class ProtocolError(Exception):
    pass


class Stopped(Exception):
    pass


def log(event, **values):
    print(json.dumps({'t': round(time.monotonic(), 3), 'event': event, **values}),
          flush=True)


def frame(command, body=b''):
    return struct.pack('<II', command, 8 + len(body)) + body


def checked_range(address, length, bits):
    if address <= 0 or length <= 0 or address + length > 1 << bits:
        raise ProtocolError('invalid/overflowing target memory range')


class Transport:
    """All I/O uses absolute deadlines; a dribbling peer cannot extend them."""
    def __init__(self, fd, io_timeout=10):
        self.fd = fd
        self.io_timeout = io_timeout

    def wait(self, event, deadline):
        poller = select.poll()
        poller.register(self.fd, event)
        while True:
            left = deadline - time.monotonic()
            if left <= 0:
                raise TimeoutError('EFS exchange deadline expired')
            try:
                events = poller.poll(max(1, int(left * 1000)))
            except InterruptedError:
                continue
            for _, flags in events:
                # Drain bytes already queued before handling HUP.
                if flags & event:
                    return
                if flags & (select.POLLERR | select.POLLHUP | select.POLLNVAL):
                    raise ConnectionError('EFS channel disconnected')

    def read_exact(self, size, deadline):
        result = bytearray()
        while len(result) < size:
            self.wait(select.POLLIN, deadline)
            try:
                data = os.read(self.fd, size - len(result))
            except (BlockingIOError, InterruptedError):
                continue
            if not data:
                raise ConnectionError('EOF on EFS channel')
            result.extend(data)
        return bytes(result)

    def send(self, packet, deadline):
        while True:
            self.wait(select.POLLOUT, deadline)
            try:
                count = os.write(self.fd, packet)
            except (BlockingIOError, InterruptedError):
                continue
            # Commands fit into one MHI transfer. Never send a torn command's
            # tail as a second packet, or acknowledge it as success.
            if count != len(packet):
                raise ProtocolError('short EFS command write')
            return

    def packet(self, deadline):
        command, length = struct.unpack('<II', self.read_exact(8, deadline))
        if PACKET_LENGTHS.get(command) != length:
            raise ProtocolError(f'unexpected command/length: {command}/{length}')
        return command, self.read_exact(length - 8, deadline)

    def memory(self, address, length, bits, session_deadline):
        checked_range(address, length, bits)
        if length > CHUNK:
            raise ProtocolError('memory request exceeds bounded transfer size')
        deadline = min(session_deadline, time.monotonic() + self.io_timeout)
        if bits == 64:
            packet = frame(MEM_READ64, struct.pack('<QQ', address, length))
        else:
            packet = frame(MEM_READ, struct.pack('<II', address, length))
        self.send(packet, deadline)
        return self.read_exact(length, deadline)


def decode_table(table, bits):
    fmt = '<QQQ20s20s' if bits == 64 else '<III20s20s'
    entry_size = struct.calcsize(fmt)
    if not table or len(table) % entry_size or len(table) > len(REGIONS) * entry_size:
        raise ProtocolError('invalid EFS region table size')
    regions = []
    seen = set()
    for pref, base, length, _desc, raw_name in struct.iter_unpack(fmt, table):
        # Require a terminator and an exact known stock partition basename.
        # No path components, aliases, prefixes, or modem-chosen output paths.
        if b'\0' not in raw_name:
            raise ProtocolError('unterminated EFS region name')
        try:
            name = raw_name.split(b'\0', 1)[0].decode('ascii')
        except UnicodeDecodeError as error:
            raise ProtocolError('non-ASCII EFS region name') from error
        if name not in REGIONS or name in seen:
            raise ProtocolError(f'unknown/duplicate EFS region: {name!r}')
        if not 0 < length <= REGIONS[name]:
            raise ProtocolError(f'EFS region size outside limit: {name}/{length}')
        checked_range(base, length, bits)
        if any(base < other['base'] + other['length'] and
               other['base'] < base + length for other in regions):
            raise ProtocolError('overlapping EFS regions')
        seen.add(name)
        regions.append({'name': name, 'base': base, 'length': length,
                        'save_pref': pref})
    return regions


def regular_new(directory_fd, name):
    fd = os.open(name, os.O_WRONLY | os.O_CREAT | os.O_EXCL |
                 os.O_NOFOLLOW | os.O_CLOEXEC, 0o600, dir_fd=directory_fd)
    mode = os.fstat(fd)
    if not stat.S_ISREG(mode.st_mode) or mode.st_nlink != 1:
        os.close(fd)
        raise ProtocolError('output is not a new single-link regular file')
    return fd


def write_all(fd, data):
    while data:
        try:
            count = os.write(fd, data)
        except InterruptedError:
            continue
        if count <= 0:
            raise OSError('snapshot write made no progress')
        data = data[count:]


def json_new(directory_fd, name, value):
    with contextlib.closing(os.fdopen(regular_new(directory_fd, name), 'wb')) as f:
        f.write((json.dumps(value, indent=2) + '\n').encode())
        f.flush()
        os.fsync(f.fileno())


class Snapshots:
    """Immutable snapshots only. Partial sessions are preserved, never reused."""
    def __init__(self, root, boot, byte_limit=128 * MIB, session_limit=64):
        self.fd = os.open(root, os.O_RDONLY | os.O_DIRECTORY |
                          os.O_NOFOLLOW | os.O_CLOEXEC)
        try:
            info = os.fstat(self.fd)
            if info.st_uid != os.geteuid() or info.st_mode & 0o077:
                raise ProtocolError('snapshot root must be owned by us, mode 0700')
            # The boot value is validated, not interpreted as a path.
            boot = str(uuid.UUID(boot))
            name = f'{boot}-{uuid.uuid4().hex}'
            os.mkdir(name, 0o700, dir_fd=self.fd)
            os.fsync(self.fd)
            run = os.open(name, os.O_RDONLY | os.O_DIRECTORY |
                          os.O_NOFOLLOW | os.O_CLOEXEC, dir_fd=self.fd)
        except BaseException:
            os.close(self.fd)
            raise
        os.close(self.fd)
        self.fd = run
        self.path = str(Path(root) / name)
        self.used = 0
        self.sessions = 0
        self.byte_limit = byte_limit
        self.session_limit = session_limit

    def begin(self, regions):
        total = sum(r['length'] for r in regions)
        if self.sessions >= self.session_limit or self.used + total > self.byte_limit:
            raise ProtocolError('snapshot quota exhausted; no data discarded/acknowledged')
        self.sessions += 1
        self.used += total  # reserve even if the session subsequently fails
        name = f'session-{self.sessions:04d}'
        os.mkdir(name, 0o700, dir_fd=self.fd)
        os.fsync(self.fd)
        return os.open(name, os.O_RDONLY | os.O_DIRECTORY |
                       os.O_NOFOLLOW | os.O_CLOEXEC, dir_fd=self.fd)

    def close(self):
        os.close(self.fd)


def session(transport, snapshots, hello, session_seconds=60, progress=None):
    command, body = hello
    if command != HELLO:
        raise ProtocolError('expected initial HELLO')
    version, minimum, packet_limit, mode, *_ = struct.unpack('<10I', body)
    if not (1 <= minimum <= 2 <= version) or packet_limit < 48 or mode not in (0, 2):
        raise ProtocolError('unsupported HELLO version, limit or mode')
    log('hello', version=version, minimum=minimum, mode=mode)
    deadline = time.monotonic() + session_seconds
    response = frame(HELLO_RESP, struct.pack('<10I', 2, 1, 0, MEMORY_MODE, *([0] * 6)))
    transport.send(response, min(deadline, time.monotonic() + transport.io_timeout))
    command, body = transport.packet(min(deadline, time.monotonic() + transport.io_timeout))
    if command == MEM_DEBUG:
        bits = 32
        address, length = struct.unpack('<II', body)
    elif command == MEM_DEBUG64:
        bits = 64
        address, length = struct.unpack('<QQ', body)
    else:
        raise ProtocolError('expected MEMORY_DEBUG table')
    entry_size = 64 if bits == 64 else 52
    if not length or length % entry_size or length > len(REGIONS) * entry_size:
        raise ProtocolError('invalid memory table length')
    table = transport.memory(address, length, bits, deadline)
    regions = decode_table(table, bits)  # validate ALL entries before saving any
    log('table', bits=bits, regions=[{'name': r['name'], 'bytes': r['length']} for r in regions])
    if progress:
        progress('saving')
    dest = snapshots.begin(regions)
    try:
        manifest = []
        for region in regions:
            digest = hashlib.sha256()
            fd = regular_new(dest, region['name'])
            try:
                offset = 0
                while offset < region['length']:
                    count = min(CHUNK, region['length'] - offset)
                    data = transport.memory(region['base'] + offset, count, bits, deadline)
                    write_all(fd, data)
                    digest.update(data)
                    offset += count
                os.fsync(fd)
            finally:
                os.close(fd)
            manifest.append({'name': region['name'], 'bytes': offset,
                             'sha256': digest.hexdigest()})
        json_new(dest, 'manifest.json', {'regions': manifest, 'bits': bits})
        os.fsync(dest)
        # Stock EFS Sahara completes its memory-debug session with RESET.
        # This is sent ONLY on the validated EFS port, after durable files;
        # never on an error, or to a PCI/ESOC/reset control interface.
        transport.send(frame(RESET), min(deadline, time.monotonic() + transport.io_timeout))
        command, _ = transport.packet(min(deadline, time.monotonic() + transport.io_timeout))
        if command != RESET_RESP:
            raise ProtocolError('expected EFS session RESET_RESP')
        json_new(dest, 'complete.json', {'reset_response': True})
        os.fsync(dest)
    finally:
        os.close(dest)
    log('session-complete', session=snapshots.sessions, bytes=snapshots.used)
    if progress:
        progress('ready')


def discover_port(sys_class=Path('/sys/class/wwan'), dev_root=Path('/dev')):
    matches = []
    for port in sys_class.glob('*'):
        try:
            if (port / 'type').read_text().strip() != 'EFS':
                continue
            path = port.resolve(strict=True)
            pci = next((p for p in path.parents
                        if (p / 'vendor').is_file() and (p / 'device').is_file()), None)
            if pci is None or (pci / 'vendor').read_text().strip() != '0x17cb' or \
                    (pci / 'device').read_text().strip() != '0x0306':
                continue
            if not (pci / 'of_node/esoc-0').exists():
                continue  # not a Fusion endpoint
            major, minor = map(int, (port / 'dev').read_text().split(':'))
            matches.append((dev_root / port.name, os.makedev(major, minor)))
        except FileNotFoundError:
            continue  # device removal during enumeration
    if len(matches) > 1:
        raise ProtocolError('ambiguous EFS ports; refusing to choose')
    return matches[0] if matches else None


def open_port(candidate):
    path, device = candidate
    # Check before open: opening a wrong WWAN port can itself start a channel.
    before = path.lstat()
    if not stat.S_ISCHR(before.st_mode) or before.st_rdev != device:
        raise ProtocolError('EFS node is not the expected character device')
    fd = os.open(path, os.O_RDWR | os.O_NONBLOCK | os.O_NOFOLLOW | os.O_CLOEXEC)
    after = os.fstat(fd)
    if not stat.S_ISCHR(after.st_mode) or after.st_rdev != device or \
            (before.st_dev, before.st_ino) != (after.st_dev, after.st_ino):
        os.close(fd)
        raise ProtocolError('EFS device changed during open')
    return fd


class Status:
    def __init__(self, path):
        # Existing status means this boot already attempted EFS service.
        self.fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL |
                          os.O_NOFOLLOW | os.O_CLOEXEC, 0o600)

    def update(self, state, **extra):
        data = (json.dumps({'state': state, 't': time.monotonic(), **extra}) + '\n').encode()
        os.lseek(self.fd, 0, os.SEEK_SET)
        write_all(self.fd, data)
        os.ftruncate(self.fd, len(data))
        os.fsync(self.fd)

    def close(self):
        os.close(self.fd)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--wait-seconds', type=int, default=120)
    args = parser.parse_args()
    if not 1 <= args.wait_seconds <= 120:
        parser.error('wait must be between 1 and 120 seconds')
    if os.geteuid() != 0:
        parser.error('root is required for the validated EFS port')

    def stop(_signum, _stack):
        raise Stopped()

    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)
    with contextlib.ExitStack() as cleanup:
        status = Status(STATUS_PATH)
        cleanup.callback(status.close)
        try:
            status.update('waiting')
            until = time.monotonic() + args.wait_seconds
            candidate = None
            while time.monotonic() < until:
                candidate = discover_port()
                if candidate:
                    break
                time.sleep(0.1)
            if not candidate:
                raise TimeoutError('EFS port did not appear')
            fd = open_port(candidate)
            cleanup.callback(os.close, fd)
            boot = Path('/proc/sys/kernel/random/boot_id').read_text().strip()
            snapshots = Snapshots(STATE_ROOT, boot)
            cleanup.callback(snapshots.close)
            transport = Transport(fd)
            status.update('ready', port=str(candidate[0]), sessions=0)
            log('port-open', port=str(candidate[0]), snapshots=snapshots.path)
            while True:
                # Idle is normal. Once the first byte arrives, even the HELLO
                # must finish within a deadline. No reconnect after EOF/error.
                poller = select.poll()
                poller.register(fd, select.POLLIN)
                if not poller.poll(1000):
                    continue
                hello = transport.packet(time.monotonic() + transport.io_timeout)
                session(transport, snapshots, hello,
                        progress=lambda state: status.update(state, port=str(candidate[0]),
                                                             sessions=snapshots.sessions))
        except Stopped:
            status.update('stopped')
            return 0
        except (OSError, ProtocolError, ValueError) as error:
            log('failed', reason=str(error))
            status.update('failed', reason=str(error))
            return 1


if __name__ == '__main__':
    sys.exit(main())
