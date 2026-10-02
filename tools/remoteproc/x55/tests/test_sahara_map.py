#!/usr/bin/python3
# SPDX-License-Identifier: GPL-2.0-only
"""Compile the real Sahara mapping function with tiny host stubs, no modem I/O."""
from pathlib import Path
import subprocess
import tempfile
import unittest


class Tests(unittest.TestCase):
    def test_mapping_and_board_scope(self):
        repo = Path(__file__).resolve().parents[4]
        text = (repo / 'drivers/bus/mhi/host/sahara.c').read_text()
        start = text.index('static const char *sahara_image_to_fw(')
        end = text.index('\nstatic void sahara_send_reset', start)
        body = text[start:end]
        work = Path(tempfile.mkdtemp(prefix='x55-sahara-test-', dir='/tmp'))
        fixture = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
typedef uint32_t u32;
struct controller { const char *name; const char *amss_image; };
struct mhi_device { struct controller *mhi_cntrl; };
struct mhi_sahara_dev { struct mhi_device *mhi_dev; };
static bool kebab;
static bool of_machine_is_compatible(const char *name) {
    return kebab && !strcmp(name, "oneplus,kebab");
}
'''
        checks = r'''
int main(void) {
    struct controller ctrl = { "qcom-sdx55m-fusion", "amss-test" };
    struct mhi_device dev = { &ctrl };
    struct mhi_sahara_dev sdev = { &dev };
    assert(!strcmp(sahara_image_to_fw(&sdev,20), "qcom/sdx55m/efs3.bin"));
    kebab = true;
    assert(!strcmp(sahara_image_to_fw(&sdev,20), "qcom/sdx55m/oemnvbktmp.bin"));
    ctrl.name = "standalone";
    assert(!strcmp(sahara_image_to_fw(&sdev,20), "qcom/sdx55m/efs3.bin"));
    ctrl.name = NULL;
    assert(!strcmp(sahara_image_to_fw(&sdev,20), "qcom/sdx55m/efs3.bin"));
    assert(!strcmp(sahara_image_to_fw(&sdev,37), "amss-test"));
    assert(!strcmp(sahara_image_to_fw(&sdev,38), "qcom/sdx55m/xbl_cfg.elf"));
    assert(!strcmp(sahara_image_to_fw(&sdev,8), "qcom/sdx55m/qdsp6sw.mbn"));
    assert(!strcmp(sahara_image_to_fw(&sdev,34), "qcom/sdx55m/mdmddr.bin"));
    assert(!strcmp(sahara_image_to_fw(&sdev,16), "qcom/sdx55m/efs1.bin"));
    assert(!strcmp(sahara_image_to_fw(&sdev,17), "qcom/sdx55m/efs2.bin"));
    assert(sahara_image_to_fw(&sdev,0) == NULL);
    assert(sahara_image_to_fw(&sdev,35) == NULL);
    assert(sahara_image_to_fw(&sdev,0xffffffff) == NULL);
    return 0;
}
'''
        source = work / 'mapping.c'
        source.write_text(fixture + body + checks)
        subprocess.run(['cc', '-std=c11', '-O1', '-Wall', '-Wextra', '-Werror',
                        str(source), '-o', str(work / 'test')], check=True, timeout=20)
        subprocess.run([str(work / 'test')], check=True, timeout=5)


if __name__ == '__main__':
    unittest.main(verbosity=2)
