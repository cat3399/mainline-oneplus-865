#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Compile the actual TCM parser with ASan/UBSan and exercise it without hardware.

Run from any directory: python3 tools/testing/synaptics_tcm_report_test.py
Optional replay: --config '10 08 ... 03 00' --capture /path/to/kmsg.log
Temporary build artifacts are retained under /tmp; no kernel build cache is changed.
"""

import argparse
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile


PREAMBLE = r"""
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
#define ALIGN(x, a) (((x) + (a) - 1) & ~((a) - 1))
"""

TESTS = r"""
static struct tcm_report_config cfg;
static struct tcm_report_object objects[TCM_MAX_OBJECTS];
static u8 report[TCM_MAX_REPORT_CONFIG_SIZE];
static unsigned int cursor;
static unsigned int tests;

static const u8 legacy[] = {
    0x11, 8, 0x1e, 8, 0x0f, 1, 4,
    1, 6, 4, 7, 4, 8, 12, 9, 12, 10, 8, 11, 8, 12, 8,
    13, 16, 14, 16, 3, 0
};
static const u8 alternate[] = {
    0x10, 8, 0xc6, 48,
    1, 6, 4, 7, 4, 8, 12, 9, 12, 10, 8, 11, 8, 12, 8, 3, 0
};
static const u8 counted[] = {
    0x18, 8, 1, 7, 4, 6, 4, 9, 16, 8, 16, 11, 12, 12, 12, 3, 0
};
static const u8 all_objects[] = {
    2, 7, 4, 8, 12, 9, 12, 10, 8, 4, 3, 0
};

static void load(const u8 *data, size_t size)
{
    assert(tcm_parse_report_config(&cfg, data, size) == 0);
    memset(report, 0, sizeof(report));
    cursor = 0;
}

static void put(u32 value, unsigned int bits)
{
    unsigned int i;
    assert(bits <= 32 && cursor + bits <= sizeof(report) * 8);
    for (i = 0; i < bits; i++) {
        if ((value >> i) & 1)
            report[(cursor + i) / 8] |= 1U << ((cursor + i) % 8);
    }
    cursor += bits;
}

static void contact(unsigned int slot, u32 x, u32 y)
{
    put(slot, 4); put(1, 4); put(x, 12); put(y, 12);
    put(42, 8); put(6, 8); put(9, 8);
}

static int decode(unsigned int slots)
{
    return tcm_parse_touch_report(&cfg, report, (cursor + 7) / 8, objects, slots);
}

static void test_layouts(void)
{
    const u8 footer[] = {
        0x11, 8, 1, 6, 4, 7, 4, 8, 12, 9, 12, 3, 0x05, 16, 0
    };
    const u8 wide[] = {1, 7, 8, 8, 32, 9, 32, 3, 0};

    load(legacy, sizeof(legacy));
    put(120, 8); put(0, 8); put(0, 1); cursor = ALIGN(cursor, 8);
    contact(0, 507, 64); put(0, 16); put(0, 16);
    assert(decode(10) == 0);
    assert(objects[0].x == 507 && objects[0].y == 64);
    assert(objects[0].z == 42 && objects[0].width_y == 9);
    tests++;

    load(alternate, sizeof(alternate));
    cursor = 56; contact(0, 507, 2382); contact(3, 700, 1200);
    assert(decode(10) == 0);
    assert(objects[0].x == 507 && objects[0].y == 2382);
    assert(objects[3].x == 700 && objects[3].y == 1200);
    assert(objects[1].classification == 0);
    tests++;

    load(counted, sizeof(counted));
    put(2, 8);
    put(1, 4); put(2, 4); put(50000, 16); put(12345, 16);
    put(300, 12); put(400, 12);
    put(2, 4); put(0, 4); put(60000, 16); put(54321, 16);
    put(1000, 12); put(2000, 12);
    assert(decode(10) == 0);
    assert(objects[2].x == 12345 && objects[2].y == 50000);
    assert(objects[0].classification == TCM_TOUCH_GLOVED_FINGER);
    assert(objects[0].width_x == 1000 && cfg.bits[11] == 12);
    tests++;

    load(all_objects, sizeof(all_objects));
    put(1, 4); put(100, 12); put(200, 12); put(10, 8);
    cursor = ALIGN(cursor, 8);
    put(0, 4); put(300, 12); put(400, 12); put(0, 8);
    cursor = ALIGN(cursor, 8);
    assert(decode(2) == 0);
    assert(objects[0].x == 100 && objects[1].classification == 0);
    tests++;

    load(footer, sizeof(footer));
    put(60, 8); put(5, 4); put(1, 4); put(123, 12); put(234, 12);
    put(0x1234, 16);
    assert(decode(10) == 0);
    assert(objects[5].x == 123 && objects[5].y == 234);
    tests++;
    load(footer, sizeof(footer));
    put(60, 8); put(0x1234, 16);
    assert(decode(10) == 0 && objects[5].classification == 0);
    tests++;

    load(wide, sizeof(wide));
    put(1, 8); put(UINT32_MAX, 32); put(0x87654321, 32);
    assert(decode(10) == 0);
    assert(objects[0].x == UINT32_MAX && objects[0].y == 0x87654321);
    tests++;
}

static void test_empty_and_invalid_frames(void)
{
    load(legacy, sizeof(legacy)); cursor = 24;
    memset(objects, 0xff, sizeof(objects));
    assert(decode(10) == 0 && objects[0].classification == 0); tests++;
    load(alternate, sizeof(alternate)); cursor = 56;
    assert(decode(10) == 0); tests++;
    load(counted, sizeof(counted)); put(0, 8);
    assert(decode(10) == 0); tests++;

    load(alternate, sizeof(alternate)); cursor = 56; contact(10, 10, 20);
    assert(decode(10) == -ERANGE); tests++;
    load(alternate, sizeof(alternate)); cursor = 56;
    contact(0, 10, 20); contact(0, 30, 40);
    assert(decode(10) == -EINVAL); tests++;
    load(alternate, sizeof(alternate)); cursor = 56; contact(0, 10, 20);
    assert(tcm_parse_touch_report(&cfg, report, cursor / 8 - 1,
                                  objects, 10) == -EMSGSIZE); tests++;
    load(alternate, sizeof(alternate)); cursor = 112;
    memset(report + 7, 0x5a, 7);
    assert(decode(10) == -ERANGE); tests++;
    load(counted, sizeof(counted)); put(11, 8);
    assert(decode(10) == -ERANGE); tests++;
    assert(tcm_parse_touch_report(&cfg, report, 1, objects, 0) == -EINVAL);
    assert(tcm_parse_touch_report(&cfg, report, 1, objects, 33) == -EINVAL);
    tests++;
}

static void test_invalid_configs(void)
{
    const u8 invalid[][16] = {
        {0}, {3, 0}, {1, 1, 0},
        {1, 7, 4, 8, 12, 3, 0},
        {1, 7, 33, 8, 12, 9, 12, 3, 0},
        {1, 7, 0, 8, 12, 9, 12, 3, 0},
        {8, 12, 0}, {1, 0x18, 8, 7, 4, 8, 12, 9, 12, 3, 0}
    };
    unsigned int i;
    for (i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        assert(tcm_parse_report_config(&cfg, invalid[i], sizeof(invalid[i])) < 0);
        tests++;
    }
    assert(tcm_parse_report_config(&cfg, legacy, 0) == -EINVAL);
    assert(tcm_parse_report_config(&cfg, legacy, sizeof(legacy) - 1) == -EINVAL);
    tests++;
}

static void test_randomized(void)
{
    u8 program[TCM_MAX_REPORT_CONFIG_SIZE];
    unsigned int iteration, i, size, width, x, y;
    srand(1);
    for (iteration = 0; iteration < 20000; iteration++) {
        size = 1 + rand() % sizeof(program);
        for (i = 0; i < size; i++) program[i] = rand();
        if (tcm_parse_report_config(&cfg, program, size) == 0) {
            memset(report, 0x5a, sizeof(report));
            (void)tcm_parse_touch_report(&cfg, report, rand() % sizeof(report),
                                         objects, 10);
        }
    }
    for (iteration = 0; iteration < 2000; iteration++) {
        width = 1 + rand() % 16;
        const u8 variable[] = {1, 6, 4, 7, 4, 9, width, 8, width, 3, 0};
        load(variable, sizeof(variable));
        x = rand() & ((1U << width) - 1);
        y = rand() & ((1U << width) - 1);
        put(3, 4); put(1, 4); put(y, width); put(x, width);
        assert(decode(10) == 0 && objects[3].x == x && objects[3].y == y);
    }
    tests++;
}
"""


def captured_packets(path):
    packets = []
    current = None
    for line in Path(path).read_text().splitlines():
        match = re.search(r"report: ([0-9a-f]{8}): ((?:[0-9a-f]{2} ?)+)", line)
        if not match:
            continue
        offset = int(match[1], 16)
        data = bytes.fromhex(match[2])
        if offset == 0:
            if current is not None:
                packets.append(current)
            current = bytearray(data)
        elif current is not None and offset == len(current):
            current.extend(data)
    if current is not None:
        packets.append(current)
    for packet in packets:
        if len(packet) >= 4 and packet[:2] == b"\xa5\x11":
            length = int.from_bytes(packet[2:4], "little")
            if len(packet) >= length + 4:
                yield packet[4:4 + length]


def replay_source(config, capture):
    code = ["static void test_replay(void) { unsigned int good = 0, bad = 0; int ret;"]
    code.append("const u8 descriptor[] = {" + ",".join(map(str, config)) + "};")
    code.append("load(descriptor, sizeof(descriptor));")
    for i, payload in enumerate(captured_packets(capture)):
        code.append("{ const u8 data[] = {" + ",".join(map(str, payload)) + "};")
        code.append("ret = tcm_parse_touch_report(&cfg, data, sizeof(data), objects, 10);")
        code.append("if (ret) bad++; else good++;")
        code.append("if (!ret && objects[0].classification == 1) printf(\"frame %u: %u,%u\\n\", "
                    + str(i) + ", objects[0].x, objects[0].y); }")
    code.append('printf("replay: %u accepted, %u rejected\\n", good, bad); }')
    return "\n".join(code)


def main():
    args = argparse.ArgumentParser(description=__doc__)
    args.add_argument("--config", help="Controller report descriptor, hexadecimal bytes")
    args.add_argument("--capture", type=Path, help="Previously captured raw TCM kernel log")
    options = args.parse_args()
    if bool(options.config) != bool(options.capture):
        args.error("--config and --capture must be supplied together")
    root = Path(__file__).resolve().parents[2]
    header = root / "drivers/input/touchscreen/synaptics_tcm_report.h"
    parser = "\n".join(line for line in header.read_text().splitlines()
                       if not line.startswith("#include <linux/"))
    replay = replay_source(bytes.fromhex(options.config), options.capture) if options.config else ""
    ending = """
int main(void) {
    test_layouts(); test_empty_and_invalid_frames(); test_invalid_configs(); test_randomized();
    printf("PASS: %u test groups, 22000 randomized cases (ASan/UBSan)\\n", tests);
""" + ("    test_replay();\n" if replay else "") + "    return 0;\n}\n"
    work = Path(tempfile.mkdtemp(prefix="synaptics-tcm-test-", dir="/tmp"))
    source = work / "test.c"
    executable = work / "test"
    source.write_text(PREAMBLE + parser + TESTS + replay + ending)
    compiler = shlex.split(os.environ.get("CC", "cc"))
    subprocess.run(compiler + ["-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                              "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                              str(source), "-o", str(executable)], check=True)
    subprocess.run([str(executable)], check=True)
    print(f"Test artifacts retained: {work}")


if __name__ == "__main__":
    main()
