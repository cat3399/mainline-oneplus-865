// SPDX-License-Identifier: BSD-3-Clause
/* Exercise the actual tqftpserv reader over AF_UNIX, without a QRTR peer,
 * modem, firmware files, or persistent writes. Define TQFTPSERV_SOURCE to the
 * patched upstream tqftpserv.c; compile with function/data section GC.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>
#include <libqrtr.h>

static const struct sockaddr_qrtr peer = {
	.sq_family = AF_QIPCRTR, .sq_node = 1, .sq_port = 12345,
};

static ssize_t test_recvfrom(int fd, void *buf, size_t len, int flags,
			    struct sockaddr *addr, socklen_t *addrlen)
{
	ssize_t n = recv(fd, buf, len, flags);

	if (n >= 0 && addr && addrlen && *addrlen >= sizeof(peer)) {
		memcpy(addr, &peer, sizeof(peer));
		*addrlen = sizeof(peer);
	}
	return n;
}

static int request_sock = -1;
static int request_file = -1;

static int test_qrtr_open(unsigned int port)
{
	(void)port;
	return dup(request_sock);
}

static int test_connect(int fd, const struct sockaddr *addr, socklen_t len)
{
	(void)fd;
	if (len != sizeof(peer) || addr->sa_family != AF_QIPCRTR)
		abort();
	return 0; /* The AF_UNIX socketpair is already connected. */
}

static int test_translate_open(const char *path, int flags)
{
	(void)path;
	(void)flags;
	return dup(request_file); /* Only synthetic memfd data is accessible. */
}

#ifndef TQFTPSERV_SOURCE
#error Define TQFTPSERV_SOURCE to the tqftpserv.c being tested
#endif
#define recvfrom test_recvfrom
#define qrtr_open test_qrtr_open
#define connect test_connect
#define translate_open test_translate_open
#define main tqftpserv_unused_main
#include TQFTPSERV_SOURCE
#undef main
#undef translate_open
#undef connect
#undef qrtr_open
#undef recvfrom

struct fixture {
	struct tftp_client c;
	int remote;
};

static unsigned char pattern(size_t pos)
{
	return (pos * 17 + 3) % 251;
}

static void prepare(struct fixture *f, size_t file_size, size_t bs,
		    size_t window, size_t read_size, off_t seek)
{
	unsigned char buf[4096];
	int sv[2];
	size_t pos = 0, i, n;

	memset(f, 0, sizeof(*f));
	if (socketpair(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK, 0, sv))
		abort();
	f->c.sock = sv[0];
	f->remote = sv[1];
	request_sock = f->c.sock;
	f->c.fd = memfd_create("x55-tftp-synthetic", MFD_CLOEXEC);
	if (f->c.fd < 0)
		abort();
	request_file = f->c.fd;
	while (pos < file_size) {
		n = file_size - pos;
		if (n > sizeof(buf))
			n = sizeof(buf);
		for (i = 0; i < n; i++)
			buf[i] = pattern(pos + i);
		if (write(f->c.fd, buf, n) != (ssize_t)n)
			abort();
		pos += n;
	}
	f->c.sq = peer;
	f->c.blksize = bs;
	f->c.wsize = window;
	f->c.rsize = read_size;
	f->c.seek = seek;
	f->c.blk_buf = calloc(1, bs + 4);
	if (!f->c.blk_buf)
		abort();
}

static void cleanup(struct fixture *f)
{
	free(f->c.blk_buf);
	close(f->c.fd);
	close(f->c.sock);
	close(f->remote);
}

static int ack_for(struct fixture *f, struct tftp_client *client, unsigned int block)
{
	unsigned char a[4] = {0, OP_ACK, block >> 8, block};

	if (send(f->remote, a, sizeof(a), 0) != sizeof(a))
		abort();
	return handle_reader(client);
}

static int ack(struct fixture *f, unsigned int block)
{
	return ack_for(f, &f->c, block);
}

static int packet(struct fixture *f, unsigned int block, size_t len, size_t offset)
{
	unsigned char buf[65536];
	ssize_t n = recv(f->remote, buf, sizeof(buf), MSG_DONTWAIT);
	size_t i;

	if (n != (ssize_t)(len + 4) || buf[0] || buf[1] != OP_DATA ||
	    buf[2] != ((block >> 8) & 255) || buf[3] != (block & 255))
		return 0;
	for (i = 0; i < len; i++)
		if (buf[i + 4] != pattern(offset + i))
			return 0;
	return 1;
}

static int no_packet(struct fixture *f)
{
	unsigned char b[65536];

	return recv(f->remote, b, sizeof(b), MSG_DONTWAIT) < 0 && errno == EAGAIN;
}

#define CHECK(expr) do { if (!(expr)) { \
	fprintf(stderr, "%s:%d failed: %s\n", name, __LINE__, #expr); \
	ok = 0; goto out; } } while (0)

static int run(unsigned int test)
{
	struct fixture f;
	struct tftp_client *owned = NULL;
	struct sockaddr_qrtr sq = peer;
	unsigned char buf[256];
	ssize_t n;
	const char *name = NULL;
	int ok = 1;

	switch (test) {
	case 0:
		name = "unlimited multi-block read";
		prepare(&f, 1200, 512, 1, 0, 0);
		CHECK(ack(&f, 0) == 1 && packet(&f, 1, 512, 0));
		CHECK(ack(&f, 1) == 1 && packet(&f, 2, 512, 512));
		CHECK(ack(&f, 2) == 1 && packet(&f, 3, 176, 1024));
		CHECK(ack(&f, 3) == 0 && no_packet(&f));
		break;
	case 1:
		name = "unlimited window stops at short EOF";
		prepare(&f, 700, 512, 4, 0, 0);
		CHECK(ack(&f, 0) == 1);
		CHECK(packet(&f, 1, 512, 0) && packet(&f, 2, 188, 512));
		CHECK(no_packet(&f) && ack(&f, 2) == 0);
		break;
	case 2:
		name = "exact rsize has empty terminal block, no range leak";
		prepare(&f, 2048, 512, 8, 1024, 0);
		CHECK(ack(&f, 0) == 1);
		CHECK(packet(&f, 1, 512, 0) && packet(&f, 2, 512, 512));
		CHECK(packet(&f, 3, 0, 1024) && no_packet(&f));
		CHECK(ack(&f, 3) == 0);
		break;
	case 3:
		name = "partial rsize with nonzero seek";
		prepare(&f, 2048, 512, 4, 700, 37);
		CHECK(ack(&f, 0) == 1);
		CHECK(packet(&f, 1, 512, 37) && packet(&f, 2, 188, 549));
		CHECK(no_packet(&f) && ack(&f, 2) == 0);
		break;
	case 4:
		name = "EOF before requested rsize";
		prepare(&f, 600, 512, 4, 800, 0);
		CHECK(ack(&f, 0) == 1);
		CHECK(packet(&f, 1, 512, 0) && packet(&f, 2, 88, 512));
		CHECK(no_packet(&f) && ack(&f, 2) == 0);
		break;
	case 5:
		name = "empty file";
		prepare(&f, 0, 512, 8, 0, 0);
		CHECK(ack(&f, 0) == 1 && packet(&f, 1, 0, 0));
		CHECK(no_packet(&f) && ack(&f, 1) == 0);
		break;
	case 6:
		name = "exact file size has one empty EOF block";
		prepare(&f, 1024, 512, 8, 0, 0);
		CHECK(ack(&f, 0) == 1);
		CHECK(packet(&f, 1, 512, 0) && packet(&f, 2, 512, 512));
		CHECK(packet(&f, 3, 0, 1024) && no_packet(&f));
		CHECK(ack(&f, 3) == 0);
		break;
	case 7:
		name = "partial window ACK retransmits remaining data";
		prepare(&f, 2048, 512, 8, 1024, 0);
		CHECK(ack(&f, 0) == 1);
		CHECK(packet(&f, 1, 512, 0) && packet(&f, 2, 512, 512));
		CHECK(packet(&f, 3, 0, 1024) && no_packet(&f));
		CHECK(ack(&f, 1) == 1);
		CHECK(packet(&f, 2, 512, 512) && packet(&f, 3, 0, 1024));
		CHECK(no_packet(&f) && ack(&f, 3) == 0);
		break;
	case 8:
		name = "16-bit block rollover retains file offset";
		prepare(&f, 65537 * 8 + 3, 8, 4, 0, 0);
		f.c.block = 65534;
		CHECK(ack(&f, 65534) == 1);
		CHECK(packet(&f, 65535, 8, 65534 * 8));
		CHECK(packet(&f, 65536, 8, 65535 * 8));
		CHECK(packet(&f, 65537, 8, 65536 * 8));
		CHECK(packet(&f, 65538, 3, 65537 * 8));
		CHECK(no_packet(&f) && ack(&f, 2) == 0);
		break;
	case 9: {
		static const char request[] = "\0\1fixture\0octet\0";

		name = "RRQ without options sends complete file";
		prepare(&f, 700, 512, 1, 0, 0);
		handle_rrq(request, sizeof(request) - 1, &sq);
		CHECK(!list_empty(&readers));
		owned = list_entry(readers.next, struct tftp_client, node);
		CHECK(packet(&f, 1, 512, 0));
		CHECK(ack_for(&f, owned, 1) == 1 && packet(&f, 2, 188, 512));
		CHECK(ack_for(&f, owned, 2) == 0 && no_packet(&f));
		break;
	}
	case 10: {
		static const char request[] = "\0\1fixture\0octet\0tsize\0" "0\0";
		static const char option[] = "tsize\0" "0";

		name = "empty file stat preserves tsize zero in OACK";
		prepare(&f, 0, 512, 1, 0, 0);
		handle_rrq(request, sizeof(request) - 1, &sq);
		CHECK(!list_empty(&readers));
		owned = list_entry(readers.next, struct tftp_client, node);
		n = recv(f.remote, buf, sizeof(buf), MSG_DONTWAIT);
		CHECK(n >= 2 && !buf[0] && buf[1] == OP_OACK);
		CHECK(memmem(buf, n, option, sizeof(option)) != NULL);
		break;
	}
	case 11: {
		static const char request[] = "\0\2fixture\0octet\0";
		const unsigned char data[] = {0, OP_DATA, 0, 1, 'a', 'b', 'c'};

		name = "WRQ without options sends ACK zero then accepts data";
		prepare(&f, 0, 512, 1, 0, 0);
		handle_wrq(request, sizeof(request) - 1, &sq);
		CHECK(!list_empty(&writers));
		owned = list_entry(writers.next, struct tftp_client, node);
		n = recv(f.remote, buf, sizeof(buf), MSG_DONTWAIT);
		CHECK(n == 4 && !buf[0] && buf[1] == OP_ACK && !buf[2] && !buf[3]);
		CHECK(send(f.remote, data, sizeof(data), 0) == sizeof(data));
		CHECK(handle_writer(owned) == 0);
		n = recv(f.remote, buf, sizeof(buf), MSG_DONTWAIT);
		CHECK(n == 4 && !buf[0] && buf[1] == OP_ACK && !buf[2] && buf[3] == 1);
		CHECK(pread(f.c.fd, buf, 3, 0) == 3 && !memcmp(buf, "abc", 3));
		break;
	}
	case 12:
		name = "reject ACK for data that was never sent";
		prepare(&f, 700, 512, 1, 0, 0);
		CHECK(ack(&f, 1) < 0 && no_packet(&f));
		break;
	case 13:
		name = "reject truncated reader packet";
		prepare(&f, 700, 512, 1, 0, 0);
		buf[0] = 0;
		CHECK(send(f.remote, buf, 1, 0) == 1);
		CHECK(handle_reader(&f.c) < 0 && no_packet(&f));
		break;
	case 14:
		name = "full-size reader error has safe terminator";
		prepare(&f, 700, 512, 1, 0, 0);
		memset(buf, 'X', 128);
		buf[0] = buf[2] = 0;
		buf[1] = OP_ERROR;
		buf[3] = ERROR_END_OF_TRANSFER;
		CHECK(send(f.remote, buf, 128, 0) == 128);
		CHECK(handle_reader(&f.c) < 0 && no_packet(&f));
		break;
	default:
		abort();
	}
out:
	printf("%s: %s\n", ok ? "PASS" : "FAIL", name);
	if (owned)
		client_close_and_free(owned);
	cleanup(&f);
	return ok;
}

int main(void)
{
	unsigned int i, passed = 0;

	for (i = 0; i < 15; i++)
		passed += run(i);
	printf("%u/15 protocol tests passed (AF_UNIX + synthetic memfd only)\n", passed);
	return passed == 15 ? 0 : 1;
}
