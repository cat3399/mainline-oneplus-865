.. SPDX-License-Identifier: GPL-2.0-only

Dual-Hall three-position selector
================================

The ``dual-hall-slider`` input driver consumes two independent magnetic IIO
channels mounted at opposite ends of a three-position selector. It reports
mechanical positions, not audio modes or key presses. Chip initialization,
addresses, supplies and board wiring are not part of the input driver.

This is local bring-up support. The sensor variant, thresholds, calibration,
resume behaviour and physical wiring must be validated on the device before
calling the hardware adaptation complete.

Userspace interface
-------------------

The input device is named ``Dual Hall Slider``. On the kebab board it reports
``EV_ABS / ABS_MISC``, followed by ``SYN_REPORT``:

* ``0``: upper detent (near the channel named ``end-0``).
* ``1``: middle detent.
* ``2``: lower detent (near the channel named ``end-2``).
* ``-1``: initial state before a valid, debounced position has been obtained.

There are no ``KEY_VOLUME*`` events or silent/vibrate/ring semantics. Userspace
can associate the three values with any action. It can obtain the most recently
reported value using ``EVIOCGABS(ABS_MISC)`` and listen for subsequent changes.
That value is a debounced cache, not an instantaneous hardware read. On reopen,
allow sampling/debounce to complete before assuming the old cache is current.
After ``SYN_DROPPED``, ignore events until the next ``SYN_REPORT`` and reread the
absolute state with ``EVIOCGABS``.

Find the evdev node by its name, rather than a hard-coded event number::

    grep -l '^Dual Hall Slider$' /sys/class/input/event*/device/name
    evtest /dev/input/eventN

For plain position numbers, the dependency-free example listener reads evdev
packets and resynchronizes after ``SYN_DROPPED``::

    gcc -std=c11 -O2 -Wall -Wextra -Werror \
        tools/input/hall-slider-monitor.c -o /tmp/hall-slider-monitor
    /tmp/hall-slider-monitor /dev/input/eventN

It prints only ``0``, ``1`` or ``2``, without assigning an action or grabbing
the device. The initial output, when present, is the last reported cache.

The kernel driver assigns no permissions or application policy. Existing evdev
permissions apply. A userspace daemon should keep the device open while it is
interested in changes.

Sampling and limitations
------------------------

The first implementation uses the standard input polling framework. The board
requests a 100 ms poll interval and 100 ms debounce. Polling starts when the
first input consumer opens the device and stops when the last consumer closes
it. The IIO sensors enter runtime standby after their reads become idle; this
is chip standby, not removal of the board supply.

This implementation does not use the physical Hall threshold IRQs, does not
provide selector-triggered wake from system sleep, and does not capture every
intermediate detent during rapid movement. After resume, an open input device
samples again and requires a fresh debounce period. Transport errors,
saturation or rejected magnetic samples retain the last reported position
rather than fabricating a new input event.

Without calibration, the bootstrap rule compares the two field magnitudes,
using the board's threshold and hysteresis. With calibration it compares the
signed field difference to the three reference detents and rejects excessive
differential/common-mode departures. This is not a guarantee against every
possible magnetic disturbance and is not the entire vendor interference FSM.

IIO and calibration
-------------------

``mxm1120`` handles one firmware-described MXM1120 or IST8801 sensor per probe.
It checks that client's chip ID before reset/configuration. It neither scans
addresses nor selects an OEM BOM. Each device exposes:

* ``in_magn_z_raw``: signed counts in 10-bit mode, including full-scale values.
* ``in_magn_z_raw_available``: ``[-512 1 511]``.
* ``in_magn_sampling_frequency``: 20 Hz (read-only).

No unverified magnetic scale or factory gain conversion is advertised. These
are raw-count interfaces, not calibrated physical-unit measurements. The input
consumer expects comparable, signed, zero-centred raw readings from both ends.

The selector's platform sysfs directory provides ``position``, ``sample_valid``,
``hall_raw`` and ``calibration``. Their format is documented in
``Documentation/ABI/testing/sysfs-driver-dual-hall-slider``.

For example, with the IST8801 pair::

    /sys/bus/platform/devices/hall-slider/hall_raw
    /sys/bus/platform/devices/hall-slider/calibration

With the MXM1120 pair, use ``hall-slider-mxm1120`` in place of ``hall-slider``.
Use the platform directory corresponding to the bound input device, not an
unpopulated candidate's directory.

Hold each detent still and collect several ``hall_raw`` samples. Use stable
representative values for each channel, preferably their medians. Apply six
signed decimal counts in this order::

    position0_end0 position0_end2 position1_end0 position1_end2 position2_end0 position2_end2

Write that single line to ``calibration`` as root. Do not use example numbers
as hardware calibration. Invalid counts, indistinguishable detents or an
unordered field difference are rejected. Writing ``reset`` restores bootstrap
classification. Calibration is volatile; userspace may save/restore it for
this physical device. No Android persist/NV partition is accessed.

Firmware description (local bring-up)
------------------------------------

A sensor node describes only that sensor:

* ``compatible``: ``magnachip,mxm1120`` or ``isentek,ist8801``.
* ``reg``: the firmware-described I2C address.
* ``#io-channel-cells = <0>``: the single magnetic channel.
* ``vdd-supply``: required; no dummy board supply is substituted.
* ``vio-supply``: optional, only when its physical connection is verified.
* ``label``, ``pinctrl-*`` and ``interrupts-extended``: board descriptions.
  The IRQ is described faithfully but is not consumed by this polling version.

A selector node has ``compatible = "dual-hall-slider"`` and:

* Two ``io-channels``, named ``end-0`` and ``end-2`` by ``io-channel-names``.
* ``linux,axis``: a non-multitouch EV_ABS axis (the board uses ``ABS_MISC``).
* ``position-threshold``: positive bootstrap count threshold.
* ``hysteresis``: count hysteresis, strictly less than the threshold.
* ``interference-threshold``: positive calibrated common-mode count tolerance.
* ``poll-interval``: 50--1000 ms, default 100 ms.
* ``debounce-interval``: 0--5000 ms, default 100 ms.

These properties and calibration interfaces are local prototypes, not a claim
that a device-tree binding has been reviewed or accepted upstream.

Kebab bring-up and existing installation flow
--------------------------------------------

Board wiring belongs directly in the existing ``sm8250-oneplus-kebab.dts``;
``sm8250-oneplus-kebab-x55.dts`` includes it. No separate Hall DTS/DTBO variant
is required.

The existing OEM DTS includes both stock Hall BOMs. Each has its own pair of
IIO providers and its own instance of the same generic selector driver:

* IST8801: end-0 at ``0x19`` and end-2 at ``0x18``; expected chip ID ``0x81``.
* MXM1120: end-0 at ``0x0d`` and end-2 at ``0x0c``; expected chip ID ``0x9c``.

Each input instance references only its own two channels. This keeps the
missing BOM's supplier dependencies from blocking the present BOM. The chip
driver verifies the firmware-described device ID before reset/configuration;
no OEM names or candidate address tables are added to the driver. With the
stock mutually exclusive BOMs, only the populated, matching pair can expose a
``Dual Hall Slider`` input device. Failed probes/deferred supplier links for
the unpopulated pair are expected, not an extra usable input device.

Both BOMs share upper GPIO157 and lower GPIO138. Their pinctrl states belong to
the selector instances, which bind after their IIO pair is ready, rather than
the candidate sensor devices. Thus nonexistent candidates do not acquire the
shared GPIOs before chip-ID checking. The temporary identification-only VDD
always-on vote is removed; bound sensor drivers hold their supply references.

This physical device was checked by read-only transactions: both ``0x18`` and
``0x19`` returned ``0x81`` from ID register ``0x09``. That verifies IST8801 on
this specimen; it is not a claim of a physical MXM1120 test. Before driver
initialization both were in standby (OPF ``0x00``), so their zero field bytes
were not valid position measurements.

Downstream ``pm8150a_l10`` names RPMh ``ldoc10`` (``vreg_l10c_3p0``), not
``ldoa10``. The ID reads verified operation with this supply enabled at 3.0 V.
VIO wiring remains unverified and is not guessed.

The two drivers are enabled as modules in ``kebab_defconfig``. Preserve the
current X55 fragment/release and always reuse ``out-kebab/``. Do not create a
new build directory or run clean/mrproper.

Installation and image generation use the existing ``install-kebab.sh``;
there is no additional build/install wrapper. Use the ordinary OEM DTB name,
``sm8250-oneplus-kebab-x55`` for the current X55 configuration. When matching
artifacts have already been built, use the script's existing ``--skip-build``
option to preserve the cached X55 configuration. The script's default
``kebab_defconfig`` selects ``-kebab``, not ``-kebab-x55-r5``. If rebuilding
through the existing script from a deliberately prepared cached configuration,
its ``DEFCONFIG=olddefconfig`` setting retains that configuration. Do not rely
on a default invocation to preserve X55.

Normal installation updates installed modules, /boot files and initramfs links.
The existing ``--stage-only`` mode refuses the currently running release; do
not bypass that check or bump the version merely to make staging work. Direct
boot-partition writes require the script's explicit ``--write-slot`` option
and user authorization. Do not change slots or reboot automatically.

Validation
----------

The host test uses the same classifier, debounce and decode helpers as the
kernel drivers. Build the test executable under /tmp, not a new workspace::

    gcc -std=c11 -O2 -Wall -Wextra -Werror \
        -fsanitize=undefined -fno-sanitize-recover=all \
        tools/input/dual-hall-slider-test.c -o /tmp/dual-hall-slider-test
    /tmp/dual-hall-slider-test

It covers both magnet polarities, invalid calibration, hysteresis, magnetic
rejection, debounce/jiffies wraparound, signed arithmetic, every MXM/IST encoded
value and all 1,048,576 pairs of 10-bit raw values. These synthetic tests do not
verify sensor identity, wiring, physical position accuracy or power management.

After an explicitly authorized test boot, validate the bound sensor IDs, all
three detents, cold boot at each position, rapid movement, calibration,
reopening evdev, and suspend/resume. IRQ-driven operation and wake support are
separate follow-up work after the basic hardware path has been confirmed.
