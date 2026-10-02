// SPDX-License-Identifier: GPL-2.0-only
/* Print only reported detent numbers; assign no action or exclusive grab. */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static void emit_position(int position, int *last)
{
	if (position >= 0 && position <= 2 && position != *last) {
		printf("%d\n", position);
		*last = position;
	}
}

static int query_position(int fd, unsigned int axis, int *last)
{
	struct input_absinfo info;

	if (ioctl(fd, EVIOCGABS(axis), &info) < 0) {
		perror("EVIOCGABS");
		return -1;
	}
	if (info.minimum != -1 || info.maximum != 2) {
		fprintf(stderr, "The requested axis is not a three-position selector\n");
		return -1;
	}
	emit_position(info.value, last);
	return 0;
}

int main(int argc, char **argv)
{
	unsigned int axis = ABS_MISC;
	struct input_event event;
	char name[256] = { 0 }, *end;
	int fd, last = -1, pending = -1;
	bool dropped = false;
	unsigned long parsed;
	ssize_t bytes;

	if (argc == 2 && (!strcmp(argv[1], "--help") || !strcmp(argv[1], "-h"))) {
		printf("Usage: %s /dev/input/eventN [axis-code, default 0x28]\n", argv[0]);
		return EXIT_SUCCESS;
	}
	if (argc < 2 || argc > 3) {
		fprintf(stderr, "Usage: %s /dev/input/eventN [axis-code]\n", argv[0]);
		return EXIT_FAILURE;
	}
	if (argc == 3) {
		errno = 0;
		parsed = strtoul(argv[2], &end, 0);
		if (errno || end == argv[2] || *end || parsed >= ABS_MT_SLOT) {
			fprintf(stderr, "Invalid non-multitouch axis code\n");
			return EXIT_FAILURE;
		}
		axis = parsed;
	}
	setvbuf(stdout, NULL, _IOLBF, 0);
	fd = open(argv[1], O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		perror(argv[1]);
		return EXIT_FAILURE;
	}
	if (ioctl(fd, EVIOCGNAME(sizeof(name)), name) < 0) {
		perror("EVIOCGNAME");
		goto out;
	}
	if (strcmp(name, "Dual Hall Slider")) {
		fprintf(stderr, "Not a Dual Hall Slider input device: %s\n", name);
		goto out;
	}
	/* This is the last reported state, possibly unknown/stale on first open. */
	if (query_position(fd, axis, &last))
		goto out;
	for (;;) {
		bytes = read(fd, &event, sizeof(event));
		if (bytes < 0 && errno == EINTR)
			continue;
		if (bytes != (ssize_t)sizeof(event)) {
			if (bytes < 0)
				perror("read input event");
			else
				fprintf(stderr, "Input device ended or returned a short event\n");
			break;
		}
		if (event.type == EV_SYN && event.code == SYN_DROPPED) {
			dropped = true;
			pending = -1;
			continue;
		}
		if (dropped) {
			if (event.type == EV_SYN && event.code == SYN_REPORT) {
				if (query_position(fd, axis, &last))
					break;
				dropped = false;
			}
			continue;
		}
		if (event.type == EV_ABS && event.code == axis)
			pending = event.value;
		if (event.type == EV_SYN && event.code == SYN_REPORT) {
			emit_position(pending, &last);
			pending = -1;
		}
	}
 out:
	close(fd);
	return EXIT_FAILURE;
}
