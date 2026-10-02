# X55 offline regression tests

These tests use only synthetic inputs, AF_UNIX sockets and fresh `/tmp`
directories. They do not contact a modem or write raw partitions. Temporary
artifacts remain under `/tmp` for inspection; the tests do not remove files.

From the kernel repository:

```sh
PYTHONDONTWRITEBYTECODE=1 python3 -X dev -W error -m unittest discover \
  -s tools/remoteproc/x55/tests -p 'test_*.py' -v
```

Currently: 22 EFS tests, 10 lifecycle tests, 10 persistent-WDS tests and one
compiled test of the real Sahara mapping function (43 total). The mapping
test invokes the native C compiler on a small userspace fixture; it does not rebuild the kernel.
The EFS tests include the OnePlus OEM region, 32/64-bit sessions, bounded
transfers and absolute deadlines, persistence-before-completion, malformed
packets/tables, device/path restrictions, storage errors and discovery. The
controller tests stub all hardware/service/QMI operations and cover lifecycle
ordering, version checks, liveness, ambiguity and fail-closed behavior.
The WDS tests use synthetic QMI datagrams over AF_UNIX, validating message/TLV
bounds, transaction matching, no timeout retries, one-client bind/start/settings/
stop sequencing, IPv4 byte order, settings limits and private status creation.
No WDS regression test sends data to a physical modem or an Internet host.

## tqftpserv protocol tests

Use upstream `linux-msm/tqftpserv` at
`c2559a26098f6d2b36a946ef0b6ad02223264c3e`, applying
`tqftpserv-sdx55.patch` and then `tqftpserv-read-fixes.patch` from the parent
directory. These are userspace sources, not another kernel build directory.

From the kernel repository, with the patched tqftpserv source in `$TQ_SRC`
(an absolute path) and libqrtr headers installed under `/opt/x55/include`:

```sh
cc -O1 -g -Wall -Wextra -Werror \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -ffunction-sections -fdata-sections -I/opt/x55/include \
  -DTQFTPSERV_SOURCE="\"$TQ_SRC/tqftpserv.c\"" \
  tools/remoteproc/x55/tests/tqftpserv-reader-test.c \
  -Wl,--gc-sections -o /tmp/x55-tqftp-reader-test
ASAN_OPTIONS=detect_leaks=1 /tmp/x55-tqftp-reader-test
```

The test includes the actual server implementation. It substitutes AF_UNIX
socketpairs for QRTR and anonymous synthetic memfds for file translation;
it never publishes a service, contacts a modem, reads firmware or writes NV.
No libqrtr linking is needed because unused daemon code is discarded.

Coverage: unlimited/windowed reads, `rsize` boundaries, short/empty/exact EOF,
nonzero seek, partial-window ACKs, 16-bit block rollover, RRQ without options,
zero `tsize` OACK, WRQ without options, invalid ACKs and reader packet bounds.

On this device, the original implementation passed 2 of the initial 9 reader
cases. The repaired implementation passes all 15 expanded protocol tests
with AddressSanitizer, UndefinedBehaviorSanitizer and leak detection enabled.
This proves protocol regressions/fixes, **not** the cause or resolution of the
modem's approximately 100-second runtime failure.
