.. _lilygo-tdeck-max:

=================
LilyGo T-Deck Max
=================

.. tags:: chip:esp32, chip:esp32s3, arch:xtensa, vendor:lilygo

The `LilyGo T-Deck Max <https://github.com/Xinyuan-LilyGO/T-Deck-MAX>`_ is a
handheld communicator built around an ESP32-S3 (dual Xtensa LX7 @ 240 MHz,
16 MB quad flash, 8 MB quad PSRAM).  It combines a 3.1" e-paper display, a
QWERTY keyboard, LoRa, 4G and GNSS.  Nearly every peripheral is power-gated by
an XL9555 I/O expander, so the expander is brought up first and every rail is
exposed as a GPIO device.

Features
========

* ESP32-S3 (QFN56, rev v0.2), 16 MB quad flash (3.3 V), 8 MB quad PSRAM
* 3.1" 240x320 GDEQ031T10 e-paper (UC8253), front-light on GPIO41
* TCA8418 keyboard controller, CST3530 touch controller
* SX1262 LoRa radio (868/915 MHz) with internal/external antenna switch
* A7682E 4G LTE Cat 1 modem, MIA-M10Q GNSS receiver
* BHI260AP IMU, DRV2605 haptic driver, ES8311 audio codec
* SY6970 charger, BQ27220 fuel gauge
* microSD slot (SPI), XL9555 I/O expander
* USB Type-C (native USB-Serial-JTAG)

Serial Console
==============

The NSH console runs over the **USB-Serial-JTAG** peripheral and enumerates on
the host as ``/dev/ttyACM0`` (Linux, USB ID ``303a:1001``).  No USB-to-UART
bridge is needed.  The two hardware UARTs are free for the GNSS receiver
(``/dev/ttyS0``, UART1, 38400 baud) and the modem (``/dev/ttyS1``, UART2,
115200 baud).

Buttons
=======

There are two side buttons.  ``RST`` (S1) resets the chip.  ``BOOT`` (S2)
pulls GPIO0 low through a 10 kΩ pull-up; to enter the ROM download mode by
hand, hold ``BOOT``, press and release ``RST``, then release ``BOOT``.
``esptool`` normally does this automatically over USB-Serial-JTAG.

With ``LILYGO_TDECK_MAX_POWERKEY`` (on in ``full``) ``BOOT`` is also the
power key: ``/dev/kbd2`` is a one-key keyboard that reports
``KEYCODE_POWER`` as a special key, press and release
(``src/esp32s3_powerkey.c``).  The interrupt is taken on the low level
(light sleep wakes on it), masked while a worker debounces the press and
polls every 30 ms until the release.  LilyGo's own firmware uses the same
button as its "user key" to wake from hibernation.  The pnut-os shell locks
and unlocks with it.

Building and Flashing
=====================

The toolchain is ``xtensa-esp32s3-elf-gcc`` from the ESP-IDF tools installer;
``esptool`` is required for flashing (see :doc:`the ESP32-S3 platform page
</platforms/xtensa/esp32s3/index>`)::

    $ ./tools/configure.sh lilygo-tdeck-max:nsh
    $ make -j$(nproc)
    $ make flash ESPTOOL_PORT=/dev/ttyACM0 ESPTOOL_BINDIR=./

.. warning::

   Flashing replaces the factory bootloader, partition table and application.
   Read the whole flash first if the factory firmware is to be restored later::

       $ esptool -p /dev/ttyACM0 read-flash 0 0x1000000 factory-16MB.bin
       $ esptool -p /dev/ttyACM0 write-flash 0 factory-16MB.bin   # to restore

.. note::

   The board's ``scripts/Make.defs`` builds with ``-std=gnu17``.  The GCC 15
   toolchain shipped with recent ESP-IDF releases defaults to C23, which
   removed ``ATOMIC_VAR_INIT`` and breaks the Espressif HAL.

Power rails
===========

The XL9555 (I2C0, address ``0x20``, interrupt line not wired) is driven with
the PCA9555 driver.  At boot every line is driven to a defined level and
registered as ``/dev/<name>``; use ``gpio -o 0|1 /dev/<name>``:

================== ======= ==================================================
Device             XL9555  Function (boot state)
================== ======= ==================================================
``modem_pwr``      P00     A7682E supply (off)
``lora_en``        P01     SX1262 supply (off, see ``LILYGO_TDECK_MAX_BOOT_LORA_POWER``)
``gps_en``         P02     MIA-M10Q supply (off, see ``LILYGO_TDECK_MAX_BOOT_GPS_POWER``)
``imu_en``         P03     BHI260AP 1.8 V rail (on)
``lora_ant``       P04     high = internal antenna, low = external (internal)
``motor_en``       P05     DRV2605 supply (on)
``amp_en``         P06     audio power amplifier (off)
``touch_rst``      P07     touch reset, active low (released)
``modem_pwrkey``   P10     asserts the modem PWRKEY (idle)
``key_rst``        P11     keyboard reset, active low (released)
``audio_sel``      P12     high = modem audio, low = ES8311 (ES8311)
================== ======= ==================================================

To power the modem, switch on ``modem_pwr``, wait a moment, then pulse
``modem_pwrkey`` high for about 100 ms; the ``modem`` command does this
and waits for it to answer (see Modem).  The vendor notes that the battery
must be connected to use the A7682E.

Flash layout
============

The 16 MB flash is divided as follows (``full`` configuration):

===================== ======= ================================================
Range                 Size    Use
===================== ======= ================================================
0x000000 - 0x3FFFFF   4 MB    Firmware (it boots from address 0, ~1.2 MB)
0x400000 - 0x7FFFFF   4 MB    Reserved for a second firmware slot (updates)
0x800000 - 0xEFFFFF   7 MB    Not allocated
0xF00000 - 0xFFFFFF   1 MB    ``/data``: littlefs for settings, keys, contacts
===================== ======= ================================================

``/data`` (``ESP32S3_SPIFLASH_LITTLEFS``, ``ESP32S3_STORAGE_MTD_OFFSET`` and
``_SIZE``) is formatted on first use and survives reflashing, since
``make flash`` only writes the firmware.  It sits in the last megabyte so
that it stays put whatever boot scheme the firmware slots end up using.

Pin map
=======

Names are from the ESP32-S3 point of view.  The vendor macros for the modem UART
are named from the modem's side, so its TX and RX appear swapped there.

======================= =========================================
Function                GPIO
======================= =========================================
I2C0 SDA / SCL          13 / 14
SPI2 SCK / MOSI / MISO  36 / 33 / 47
Chip selects            e-paper 34, microSD 48, SX1262 3
e-paper                 DC 35, BUSY 37, RST 9, front-light 41
SX1262                  NRESET 4, DIO1 5, BUSY 6
GNSS UART1              TX 16, RX 2, PPS 1
Modem UART2             TX 10, RX 11, RI 7, DTR 8
Touch INT, keyboard INT 12, 15 (keyboard backlight 42)
IMU INT                 21
ES8311 I2S              MCLK 38, BCLK 39, WS 18, DOUT 40, DIN 17
======================= =========================================

The e-paper, the SD card and the SX1262 share SPI2.  Their chip selects are
parked high early in boot and are driven by the board (user-defined chip
select), so ``SPIDEV_MMCSD(0)``, ``SPIDEV_DISPLAY(0)`` and ``SPIDEV_LPWAN(0)``
select the SD card, the e-paper and the radio.  With the ``spi`` tool this is
``-t 1``, ``-t 4`` and ``-t 20`` respectively (``-n 0``).

Configurations
==============

nsh
---

NSH on the USB-Serial-JTAG console with 8 MB PSRAM added to the heap, I2C0
with the ``i2c`` tool, the XL9555 rails as GPIO devices (``gpio`` example),
SPI2 with the ``spi`` tool and ``/dev/spi2``, a microSD slot on SPI2
(``/dev/mmcsd0``), and UART1/UART2 for the GNSS receiver and the modem.

Two driver options matter on this board:

* ``CONFIG_ESP32S3_USBSERIAL_RXBUFSIZE=1024``: input to the USB console is
  dropped when it arrives faster than NSH consumes it (each echoed character
  is a separate USB packet, about 1 KB/s), so pasted lines were truncated.
* ``CONFIG_ESP32S3_I2C_SCL_TIMEOUT_US=20000``: the default I2C clock-stretch
  timeout is about 10 bus cycles (25 us).  The BQ27220 fuel gauge stretches
  the clock for longer, so with the default it never appears on the bus (the
  driver reports ``Transfer error 256``, a timeout, not a NACK).

Debugging
=========

The USB Serial/JTAG unit is also a JTAG probe.  It needs a udev rule that
gives the user access to USB ID ``303a:1001`` (for example ``MODE="0660",
GROUP="plugdev", TAG+="uaccess"``).  Then::

    $ openocd -c 'set ESP_RTOS hwthread' -f board/esp32s3-builtin.cfg \
        -c 'esp32s3.cpu0 configure -event gdb-detach {resume}' \
        -c 'esp32s3.cpu1 configure -event gdb-detach {resume}' \
        -c 'init; reset halt; esp appimage_offset 0x0'
    $ xtensa-esp32s3-elf-gdb -ex 'target extended-remote :3333' \
        -ex 'monitor reset halt' -ex 'thbreak esp32s3_bringup' nuttx

The ``gdb-detach`` handlers matter: without them OpenOCD leaves both cores
halted when the debugger detaches and the board looks dead until it is reset.
Use hardware breakpoints (``thbreak``/``hbreak``); the code runs from flash.

wifi
----

``nsh`` plus the 2.4 GHz Wi-Fi station: the ``wapi`` tool, DHCP renewal,
``ping`` and ``iperf``.  Associate with::

    nsh> wapi mode wlan0 WAPI_MODE_MANAGED
    nsh> wapi psk wlan0 <passphrase> WPA_ALG_CCMP WPA_VER_2
    nsh> wapi essid wlan0 <ssid> WAPI_ESSID_ON
    nsh> renew wlan0

``wapi show wlan0`` reports the association: an ESSID flag of
``WAPI_ESSID_ON`` and a real AP address mean it joined, while
``WAPI_ESSID_OFF`` with ``ff:ff:ff:ff:ff:ff`` means it did not.

blewifi
-------

``wifi`` plus Bluetooth LE and the Wi-Fi/BT coexistence arbiter (which
``ESPRESSIF_WIFI_BT_COEXIST`` turns on by default once both radios are
selected).  The ``btsak`` tool is registered under the name ``bt``::

    nsh> ifup bnep0
    nsh> bt bnep0 info
    nsh> bt bnep0 scan start
    nsh> bt bnep0 scan get
    nsh> bt bnep0 advertise start

Both configurations also include the telnet client, which is handy for
checking that TCP works end to end.  It takes a numeric address only::

    nsh> nslookup example.com
    nsh> telnet 104.20.23.154 80

.. note::

   The ESP32-S3 radio is **2.4 GHz only**.  5 GHz access points are invisible
   to it, including iPhone Personal Hotspots unless "Maximize Compatibility"
   is enabled.

full
----

Everything the board can currently do at once: the e-paper panel, both
backlights, the keyboard, Wi-Fi and Bluetooth LE, the pnut-os system layer
and its user interface on the panel, with the ``fb``, ``pwm``, ``kbd`` and
``nxterm`` examples.
This is the configuration to use on the device itself; ``nsh`` stays as the
minimal one to fall back to when something needs to be bisected.

At boot ``full`` also mounts the microSD card at ``/mnt/sd`` (when there is
one), mounts the ``/data`` settings partition (see Flash layout), and keeps
the system log in a 4 KB RAM buffer as well as on the console, so that
``dmesg`` shows messages from before anyone attached.  The RAM log lives at
``/dev/kmsg``: ``CONFIG_SYSLOG_DEVPATH`` defaulted to ``/dev/ttyS1``, the
modem's UART, where the RAM log could not register and ``dmesg`` then waited
on the modem forever.

When the out-of-tree pnut-os system layer is built in (linked into the apps
tree as ``apps/external``), ``init.rc`` starts its daemons before the shell
and restarts one that exits: ``cfgd`` (settings), ``sysd`` (battery, clock,
backlights), ``modemd`` (the modem), ``msgd`` (messages) and ``meshd`` (the
LoRa mesh).  They own the hardware they drive; ``pnut`` is their
command-line client.  After them it starts ``shell``, the user interface on
the panel (see User interface below).

The panel is a GoodDisplay GDEQ031T10, a 3.1 inch 240x320 monochrome panel
driven by a UC8253 controller, on the SPI2 bus it shares with the microSD
slot and the SX1262.  The driver is ``drivers/lcd/uc8253.c``.

Adding the radios does not slow the panel down: a full refresh still
measures 1.00 s with Wi-Fi and BLE up.  It does change the memory model,
see below.

In this configuration the radios are **off at boot**.  Neither is
initialised until something asks for it, which keeps internal heap use at
about 30 KB rather than 108 KB, and keeps the Wi-Fi radio from starting: the
network stack brings ``wlan0`` up, and with it the radio, as soon as the
interface exists.  Each radio is offered as a device that behaves like the
XL9555 power rails::

    nsh> gpio -o 1 /dev/wifi_en
    nsh> ifup wlan0
    nsh> gpio -o 1 /dev/ble_en
    nsh> ifup bnep0

Either order works and asking twice is harmless.  Bringing a radio up is a
one way trip, because the drivers have no way to unregister their network
devices: writing 0 once a radio is up fails with ``ENOTSUP``.  ``ifdown``
and ``ifup`` still stop and start the Wi-Fi radio.  ``wifi`` and
``blewifi`` bring their radios up at boot instead, through
``LILYGO_TDECK_MAX_BOOT_WIFI`` and ``LILYGO_TDECK_MAX_BOOT_BLE``.

.. warning::

   Bringing radios up at boot needs ``BOARD_INITTHREAD_STACKSIZE`` of at
   least 4096, and the build refuses anything smaller.  The board
   initialisation thread runs the radio drivers' setup, which is deep.  With
   2048 it overflowed and trampled the thread-local block at the bottom of
   its stack; the damage only surfaced when the thread exited, as a panic in
   ``pthread_mutex_inconsistent``, and because syslog goes to a buffer the
   board simply reset in a loop with nothing on the console.  The thread
   exits once boot is done, so the larger stack is not a lasting cost.

The panel appears both as ``/dev/fb0`` (through the LCD framebuffer front
end) and as ``/dev/lcd0``::

    nsh> fb -p smpte

Refreshing e-paper takes about a second and wears the panel.  ``putrun``
and ``putarea`` only update a shadow framebuffer; what pushes it to the
panel depends on two options, both enabled in this configuration:

* ``LCD_UC8253_ASYNC`` moves refreshes to a kernel thread (``uc8253``), so
  a caller never waits out the second.  Updates that arrive while a refresh
  runs are merged into the next one.
* ``LCD_UC8253_AUTOREFRESH`` lets drawing itself wake that thread.  NX
  needs it, because it never asks the driver for a refresh.

With both disabled the panel is written and refreshed only when ``redraw``
is called, which through ``/dev/fb0`` is an ``FBIO_UPDATE`` ioctl, and the
caller waits for it.

A few properties worth knowing:

* Pixels are packed **most significant bit first** and a **set bit is
  white**, which is the controller's own layout.  ``LCD_UC8253`` selects
  ``LCD_PACKEDMSFIRST``; enable ``NX_PACKEDMSFIRST`` as well if you drive
  the panel through NX.
* The panel is presented in its **native portrait orientation**, 240x320.
  The driver does not rotate.
* The **first** refresh after a reset is taken as a clear: the panel is
  driven to white and the frame that triggered it is dropped.  This is
  deliberate.  ``up_fbinitialize()`` flushes the framebuffer as soon as it
  registers, and that buffer has just been allocated with ``kmm_zalloc``,
  so it is all zeroes, which on this panel is every pixel black.  Without
  this the board would paint its screen black on every boot.  With
  ``LCD_UC8253_ASYNC`` the clear runs in the refresh thread and boot does
  not wait for it; synchronously it costs about 1.2 s.
* ``LCD_UC8253_FASTUPDATE`` (on by default) forces the waveform the
  controller would pick at a high temperature, which shortens a full
  refresh from about 3 s to about 1 s.  Turn it off if the panel has to
  work in the cold.
* A full refresh measures 1.00 s on this board, against the panel's
  documented 1.015 s for the fast waveform.  That timing is the easiest way
  to tell the BUSY line is really being polled: a driver falling back on
  fixed delays would take the full 8 s timeout instead.
* ``LCD_UC8253_PARTIAL`` (on by default) refreshes only what changed.  The
  driver keeps a copy of what the glass shows, and refreshes the rectangle
  where the new drawing differs from it; if nothing differs there is no
  refresh at all.  Measured at about 0.80 s against 1.00 s for a full one,
  but the real gain is that the rest of the screen is left alone: a full
  refresh inverts the entire panel on its way to the new image, which is
  very visible.
* Partial refreshes accumulate ghosting, so ``LCD_UC8253_FULL_EVERY``
  (16 by default) forces a full one periodically.  Set it to 0 to leave
  that entirely to the application.
* An application can steer this at run time, through ``/dev/fb0``:
  ``UC8253IOC_FULLREFRESH`` makes the next refresh a full one (the shell
  does it on unlock) and ``UC8253IOC_SETFULLEVERY`` changes the interval
  (Settings > Display > Full refresh).  Both are in
  ``include/nuttx/lcd/uc8253.h``.
* The controller does **not** keep its own "previous frame".  A partial
  refresh only moves pixels whose previous and current values differ, and
  the UC8253 leaves the previous buffer as it was after a refresh.  Every
  partial refresh therefore writes both, the previous one from the
  driver's copy of the glass.  The driver also soft resets the controller
  after every refresh, as the vendor's code does ("needed, reason
  unknown").  Before these two changes, erased pixels never turned white
  and larger updates often did not appear; both cleared up together.

Backlights
==========

Both backlights are on LEDC PWM and appear as ``/dev/pwm0``: channel 1 is
the e-paper frontlight (GPIO41) and channel 2 is the keyboard backlight
(GPIO42).  Both are off after boot::

    nsh> pwm -c 1 -c 2 -d 100 -d 100 -t 4    # both on, full, for 4 s
    nsh> pwm -c 1 -d 30 -t 10                # frontlight dim

``PWM_NCHANNELS`` must be at least 2.  With it left at 1 the LEDC driver
collapses the timer to a single channel and the keyboard backlight is never
driven.

Keyboard
========

The thumb keyboard is a 4x10 matrix scanned by a TCA8418 at I2C address
0x34, with its interrupt on GPIO15 and its reset on the XL9555.  The driver
is ``drivers/input/tca8418.c`` and it registers ``/dev/kbd0``::

    nsh> kbd 6

(The ``keyboard`` example builds under the program name ``kbd``.)  Each key
produces a press and a release event carrying its character.

The driver is interrupt driven rather than polled: the handler masks the
line, a work queue job drains the controller's event FIFO over I2C, and the
line is unmasked afterwards.  A key pressed between the last FIFO read and
the acknowledge would otherwise be left waiting with the interrupt line
already released and no edge coming, so the worker re-checks the event
counter and reschedules itself rather than lose it.

Two details of this board are easy to get wrong:

* **The matrix columns are wired in reverse.**  Column 0 of the keymap is
  the rightmost column of the matrix, which ``colreverse`` in the board
  configuration accounts for.  Without it every letter comes out mirrored
  across the keyboard.
* **The key the vendor's code calls "UP" is the shift key**, the one with
  the arrow printed on it.  There are two of them, at either end of the
  bottom row.

The keyboard is a BlackBerry Q20's, and the layers follow its legends.
Holding shift gives capitals; holding ``Alt`` (left, next to Z) gives the
characters printed on the keys, digits and punctuation (the driver calls
this held layer its symbol layer, ``TCA8418_SYM``); ``Alt`` + shift toggles
caps lock (the driver's ``TCA8418_ALT``).  ``Alt`` wins when both are held.
The ``Sym`` key (right of space) is not a modifier: it sends
``KEYCODE_FIND``.  Modifiers are handled inside the driver and are never
reported; a modifier's release always follows the base layer, so a shift
let go while ``Alt`` is held is still let go.
Space is reported as its ASCII value.  Enter and backspace are reported as
special keycodes (``KEYBOARD_SPECPRESS`` with ``KEYCODE_ENTER`` and
``KEYCODE_BACKDEL``), so that each reader applies its own convention; the
terminal turns them into a newline and DEL.

Where a layer has nothing at a position, the base layer is used instead, so
the modifiers and the digit key keep working in every layer.

With ``Alt`` held, enter, backspace and space become keys of their own:
``KEYCODE_MENU``, ``KEYCODE_CANCEL`` and ``KEYCODE_PAGEUP`` (shift + space
is ``KEYCODE_PAGEUP`` too).  The keyboard has no arrow or function keys,
and these give a user interface its options, back and previous-page keys
(the pnut-os shell uses them so).  Ordinary typing is unaffected: enter,
backspace and space without ``Alt`` are what they always were.

.. note::

   Only the key scanner may raise events.  The pins outside the matrix are
   left with their GPI event mode and interrupts disabled, because they are
   usually unconnected: a floating pin allowed into the event FIFO produces
   phantom keys and a stream of interrupts with no key behind them.  Some
   vendor libraries enable all of them, which is safe only on a board where
   every pin is wired.

.. note::

   ``lcd_framebuffer.c`` discards the return value of ``redraw``, so a
   refresh that fails or times out does **not** surface as an
   ``FBIO_UPDATE`` error.  Do not read a successful ioctl as proof that the
   panel updated; time it instead.

Battery
=======

The 1400 mAh cell is measured by a BQ27220 fuel gauge (I2C 0x55) and
charged from USB by an SY6970 charger (I2C 0x6a).  The drivers are
``drivers/power/battery/bq27220.c`` and ``drivers/power/battery/sy6970.c``,
and the ``full`` configuration registers them as ``/dev/batt0`` and
``/dev/charger0``::

    nsh> batterydump -t 1 /dev/batt0
    mask:0, state:3, online:1, vol:4205 mV, capacity:100%, current:0 mA, temperature:25.2 C
    nsh> batterydump -t 0 /dev/charger0
    mask:0, state:3, online:1, health:1, vol:4288, protocol:0

The gauge reports millivolts, percent, milliamps and tenths of a degree
Celsius.  For the gauge ``online`` means a battery is present; for the
charger it means input power is good.

Both chips are powered by the cell rather than by the ESP32-S3, so they
keep their settings across resets of the board but lose them if the
battery is disconnected.  The board therefore applies them at every boot:

* ``LILYGO_TDECK_MAX_BATTERY_CAPACITY`` (1400 mAh) is the capacity the gauge
  uses to turn charge into a percentage.  It is written only when the gauge
  holds a different value, which needs the gauge unsealed and a
  configuration update, and adds about 2 s to that one boot.
* ``LILYGO_TDECK_MAX_CHARGE_VOLTAGE`` (4288 mV) and
  ``LILYGO_TDECK_MAX_CHARGE_CURRENT`` (1024 mA) are the values the vendor's
  firmware uses.  4288 mV is above the 4.20 V a standard lithium polymer
  cell is rated for; the rating of the fitted cell is not documented.

The charger driver disables the charger's I2C watchdog.  Once the host has
written to the charger, an expired watchdog would otherwise return every
setting to its default.  The charger's ADC is left off, as the vendor does,
so its own voltage readings are stale; the gauge measures the battery.

Vibration motor
===============

The vibration motor is an ERM driven by a DRV2605L at I2C address 0x5a.
The chip's enable is the XL9555's ``motor_en`` line, which is on after
boot.  The driver is ``drivers/input/drv2605.c``, and the ``full``
configuration registers it as the force feedback device ``/dev/input_ff0``,
with the ``haptic`` example to play it::

    nsh> haptic                  # 200 ms at full strength
    nsh> haptic buzz 500 40      # 500 ms at 40 %
    nsh> haptic effect 1 7 10    # strong click, soft bump, double click

The driver runs the motor in open loop with the chip's ERM library 1, as the
vendor's firmware does, and puts the chip in standby between effects.
``FF_CONSTANT`` and ``FF_RUMBLE`` effects run the motor at a level for the
effect's length; ``FF_PERIODIC`` effects with the ``FF_CUSTOM`` waveform
play up to eight of the chip's 123 built-in effects, whose numbers go in
``custom_data``.  ``FF_GAIN`` scales the level.

.. note::

   Effects uploaded to a force feedback device outlive the file they were
   uploaded through, and only that file may erase them.  A program that
   exits without ``EVIOCRMFF`` leaves its slot in use for good.  ``haptic``
   waits for its effect to end, using the driver's ``DRV2605IOC_BUSY``
   ioctl, and erases it.

Touch
=====

The panel's touch controller is a Hynitron CST3530 at I2C address 0x1a, with
its interrupt on GPIO12 and its reset on the XL9555.  The vendor's pin map
calls it a CST328, but it answers in the protocol Hynitron uses for the
CST3530 and CST66xx family.  The driver is ``drivers/input/cst3530.c``, and
the ``full`` configuration registers it as the touchscreen ``/dev/input0``,
with the ``touchscreen`` example (``tc``) to print samples.

The protocol follows Hynitron's own driver: 32-bit register addresses,
commands written as addresses with no data, and checksummed reports read
from 0xD0070000 and acknowledged by writing 0xD00002AB.  The driver only
registers if the controller's info block carries the family's signature.
The controller is put in deep sleep at boot and only woken, by a reset,
while the device is open.  The vendor maps its coordinates
straight onto the 240x320 panel, so no swapping or mirroring is configured.
The controller's three touch keys are registered as the keyboard device
``/dev/kbd1``, reporting the special keys F1 to F3 from left to right while
it or ``/dev/input0`` is open.

Touches are reported: ``tc`` prints them on the device.

IMU
===

The BHI260AP smart sensor hub is registered with the ``bhi260ap`` driver
(``SENSORS_BHI260AP``) as the uORB topics ``sensor_accel0`` and
``sensor_gyro0``::

    nsh> uorb_listener -n 5 -t 10 sensor_accel

The chip runs a firmware image from its program RAM: the driver uploads
Bosch's standard BHI260AP image, which the board carries, when a sensor is
first activated (about 3.5 s over I2C at 400 kHz) and leaves it running.
The accelerometer (m/s², 8 g range) and gyroscope (rad/s, 2000 deg/s) are
the firmware's passthrough sensors, 50 Hz by default.  Its host interrupt
on GPIO21 is level triggered and wakes the chip from light sleep.

Audio
=====

The ES8311 codec on I2S0 is registered with the ``es8311`` driver
(``AUDIO_ES8311``) as ``/dev/audio/pcm0`` for playback, through the PCM
decoder so that WAV files play, and ``/dev/audio/pcm_in0`` for recording
from the microphone, an analogue electret on the codec's MIC1 input, with
30 dB of analog and 36 dB of digital gain (``ES8311_MIC_GAIN``).
``nxplayer`` and ``nxrecorder`` drive them, for example with raw 16-bit mono
files at 16 kHz on the microSD card::

    nsh> nxrecorder
    nxrecorder> device /dev/audio/pcm_in0
    nxrecorder> recordraw /mnt/sd/rec.raw 1 16 16000
    nxrecorder> stop
    nxrecorder> q
    nsh> nxplayer
    nxplayer> device /dev/audio/pcm0
    nxplayer> volume 50
    nxplayer> playraw /mnt/sd/rec.raw 1 16 16000
    nxplayer> q

The speaker amplifier (``amp_en``) is on while the playback device is
reserved, which also keeps the chip awake; recording holds the chip out of
light sleep itself, because I2S stops in light sleep.  Between streams the
codec's analog side is powered down, so an idle codec costs nothing
measurable.  The vendor's pin table and macros name the two data lines the
other way round from the schematic: the ESP32-S3 sends on GPIO40 (the
codec's DSDIN) and receives on GPIO17 (its ASDOUT).

Writes to the microSD card put low-frequency noise on the microphone (at
the rate nxrecorder writes buffers, a few hertz, and its harmonics).  The
configuration raises the ADC's high-pass filter (``ES8311_ADC_HPF=4``),
which takes it down by about 30 dB below 20 Hz while costing speech about
3 dB at 200-500 Hz.  The vibration motor is picked up too.

Record to the microSD card, not to ``/tmp``.  Every transfer needs a DMA
buffer in internal RAM, taken from the kernel heap (about 160 KB free in
``full``), and ``/tmp`` is a tmpfs whose files come from the same heap
(``FS_HEAPSIZE`` is 0): a 64 KB file there leaves about 90 KB, and a
recording into ``/tmp`` runs out of it after a second or two.  When I2S
cannot get its buffer the transfer fails and the audio buffer goes back
empty, so the recording just stops growing and ``stop`` still works.  (The
ES8311 driver used to lose such a buffer while still counting it in
flight, and ``stop`` then waited for it forever.)

Getting there took fixes in the ESP32-S3 I2S driver (receive clocks in
full-duplex master mode, mono slot selection, a receiver that keeps
running across buffers instead of restarting for each, the end-of-buffer
count in mono, DMA buffers in internal RAM, a hang on buffers longer than
one DMA descriptor, error paths that kept a buffer reference or unlocked a
mutex they did not hold) and in the ES8311 driver (capabilities,
configuration, the channel count, empty final buffers, failed transfers,
powering down, and options for the microphone gain and the high-pass
filter).

LoRa
====

The SX1262 is registered as ``/dev/lora0`` with the SX126x driver
(``LPWAN_SX126X``), and the ``lora`` example sends and receives packets with
the frequency, spreading factor, bandwidth, coding rate, power, sync word and
preamble given on the command line::

    nsh> lora tx hello
    nsh> lora -t 30 rx

The module is the 868 MHz variant, with a TCXO powered from DIO3 at 2.4 V and
its RF switch on DIO2; the board allows 863-870 MHz and -9 to +22 dBm, with
14 dBm by default.  The radio is reset when the device is opened and put to
sleep when it is closed.  DIO1 (GPIO5) is level triggered and a light sleep
wake-up source, so transmission and reception work while the chip sleeps.

The driver waits for the chip's BUSY line before every command, clears the
chip's IRQ status in its worker, returns received packets with their RSSI and
SNR, and takes a receive timeout (``SX126XIOC_RXTIMEOUTSET``) and a LoRa sync
word (``SX126XIOC_SYNCWORDSET``).  Its setup enables the TCXO, recalibrates,
calibrates the image rejection for the band in use and applies the
datasheet's known-limitation fixes.

Transmission is verified: the measured send times follow the calculated time
on air at SF7, SF9 and SF12.  Reception is verified with a MeshCore node (a
LilyGo T-Watch S3 at 869.618 MHz, 62.5 kHz, SF8): its Public channel message
arrived with a good CRC, at -34 dBm and 13 dB SNR, and decrypted to the text
sent; a message built in MeshCore's format and sent from ``/dev/lora0``
showed up on the watch.

The antenna switch (``lora_ant``, XL9555 P04) selects the internal antenna
when high, the default, and the external connector on the top edge when low.
It sits between the module and both antennas, so it applies to transmission
and reception alike.  Measured with the same watch 2-3 m away, alternating
the switch between its packets (median RSSI of 6 packets each):

================================ ============ ============
Setting                          Antenna on   Antenna off
================================ ============ ============
``lora_ant`` low (external)      -29 dBm      -60 dBm
``lora_ant`` high (internal)     -43 dBm      -28 dBm
================================ ============ ============

Taking the external antenna off costs its setting 31 dB, which identifies
it.  An attached external antenna that is not selected detunes the internal
one (15 dB worse), so select it whenever it is fitted::

    nsh> gpio -o 0 /dev/lora_ant

GNSS
====

The u-blox MIA-M10Q on UART1 is registered with NuttX's GNSS upper half
(``SENSORS_GNSS``), which parses its NMEA into the uORB topics
``sensor_gnss`` and ``sensor_gnss_satellite`` and passes the sentences
through ``/dev/ttyGNSS0``::

    nsh> gps

``gps`` (the ``gnss`` example) waits for a fix, reports the satellites in
view while it waits and prints the position in lines that fit the on-device
terminal; ``-s`` sets the system clock from the fix.

The receiver is powered while any of these is in use: the first user switches
the ``gps_en`` rail on and starts a thread that feeds the UART to the upper
half, and five seconds after the last user has gone the thread stops and the
rail goes off.  Writing to ``/dev/ttyGNSS0`` sends commands to the receiver.
The build downloads the minmea library, which needs ``unzip``.

NMEA arriving and being parsed into satellite messages is verified, on USB
and on battery; a position fix has not been tried with a view of the sky.

Modem
=====

The SIMCom A7682E (4G LTE Cat 1) is on UART2 at 115200 baud as
``/dev/ttyS1``.  In ``full`` the pnut-os daemon ``modemd`` owns it (see the
full configuration); without it, the ``modem`` example
(``EXAMPLES_MODEM``) drives it::

    nsh> modem on            # supply, PWRKEY, wait for the first answer
    nsh> modem info          # identity, SIM, signal, network
    nsh> modem at +CSQ       # any command, the AT prefix is optional
    nsh> modem sms send +441234567890 hello
    nsh> modem sms list      # unread, or "list all"
    nsh> modem term          # type at it yourself; Ctrl-] leaves
    nsh> modem off

``modem on`` switches the ``modem_pwr`` rail on, presses PWRKEY (high on
the XL9555 pulls the modem's PWRKEY low) for 100 ms and then sends ``AT``
once a second until the modem answers, which takes about 7.2 s; it also
turns the command echo off and verbose errors on.  The supply alone does
not start the modem, and its UART is up about 8 s after the key press.

``modem off`` shuts the modem down with ``AT+CPOF``, or with a 3 s PWRKEY
press if it does not answer, waits for it to stop answering and only then
cuts its rail, which takes about 4.2 s in all.

.. warning::

   Do not cut ``modem_pwr`` while the modem is running: SIMCom warns that
   it can damage the modem's flash.  Use ``modem off``.

The level shifter between the modem and the ESP32-S3 (a 4-bit RS0104 for
RX, TX, RI and DTR) is powered from the modem's own 1.8 V output, so while
the modem is off the ESP32-S3 sees nothing on UART2 at all.  The SIM and
the microSD card share one combined holder (WL-SIM3IN2) and can both be
fitted: the SIM contacts go only to the modem, the card to the ESP32-S3's
SPI bus.  The modem has its own microphone; its speaker output is shared with the codec's through
``audio_sel``.

With a SIM (Orange Polska): the SIM reports ready and the modem
registers on LTE at -51 dBm, and the network's time sets the device clock
(``AT+CTZU=1``, ``AT+CCLK?``).  The modem refuses caller ID (``AT+CLIP=1``)
until the SIM has loaded, which it announces with ``PB DONE``.

SMS work both ways.  This modem refuses ``AT+CMGS`` (plain ``ERROR``) with
the UCS2 character set and the GSM 7-bit data coding together; a GSM text
has to be sent with ``AT+CSCS="IRA"``, and the number given plain rather
than as UCS-2 hex.  A sender that is a name (``Orange info``) is reported
as GSM 7-bit packed into semi-octets, written as the characters ``0`` to
``?`` (``?4978==7>2382=>63?;1``).

Calls do not work with that SIM, in either direction: a call to the device
goes to voicemail, and one from it stays at dialling.  The module
(A7682E: LTE-FDD B1/B3/B5/B7/B8/B20 and GSM 900/1800, firmware
``A011B15A7682M7``) is not registered for voice over LTE (``+CIREG: 1,0``,
``+CAVIMS: 0``; the IMS context, cid 8, is deactivated by the modem at
start), so a call must fall back to GSM.  Set to GSM only
(``AT+CNMP=13``) it found no GSM network in several minutes, then stopped
answering AT commands; the mode survives power cycles, so it took a
restart that sets ``AT+CNMP=2`` first thing to recover.

Verified without a SIM card: the modem answers, identifies itself
(``A7682E``, firmware ``A7682M7_V1.11.1``, its IMEI), reports the strongest
cell it can hear (``+CSQ`` at -67 dBm) and says that no SIM is inserted;
the terminal, and powering it down and up again, work.  Not yet verified:
a data connection (PPP is in NuttX as ``NETUTILS_PPPD`` with
``NETUTILS_CHAT``, and is not configured here yet).

.. warning::

   The Espressif HAL releases memory from ``heap_caps_malloc()`` with plain
   ``free()``.  NuttX's ``heap_caps_malloc()`` takes kernel heap memory, so
   with a separate user heap (``MM_KERNEL_HEAP``, which Wi-Fi with PSRAM
   forces) ``free()`` handed it to the user heap and corrupted both; every
   interrupt teardown, such as closing a UART, did it.  ``hal.mk`` now
   force-includes ``esp_hal_free.h`` into the HAL's components, which sends
   ``free()`` back to the heap the block came from.

Power
=====

With the terminal idle and the radios off or asleep, the board draws 3 mA on
battery, about 19 days from a full cell, and 28 mA on USB power, where it
stays awake.  It drew 98 mA before these changes:

* The ``full`` configuration puts the chip into light sleep from the idle
  loop (``ESP32S3_AUTO_SLEEP``) whenever nothing is due for 20 ms or more.
  It wakes on the next timer, a key, or a touch while ``/dev/input0`` is
  open; pins, memory and CPU state are kept, and NuttX's clock stays close:
  it gained 1.83 s over 3 h 51 min asleep (+132 ppm).  Time asleep is kept
  by the internal RC oscillator, as the board has no 32 kHz crystal.
  The chip is held awake while USB power is present (the charger's power
  good status, checked every 2 s), while a backlight is on, while the GPS,
  modem or audio amplifier rail is on, and once Wi-Fi or BLE has been
  brought up.  Without USB power the USB Serial/JTAG port is disconnected.
  The I2C driver, and the SPI driver's DMA transfers, also hold it awake
  for each transfer: the controller stops in light sleep and its interrupt
  does not wake the chip, so each I2C message used to wait for the next
  timer, up to its 500 ms timeout.  With the CPU powered down during sleep
  as well (``ESP32S3_AUTO_SLEEP_CPU_PD``), the idle chip wakes only for the
  USB power check every 2 s.

* The scheduler no longer leaves the round-robin timeslice timer running
  after a round-robin task (every task, by default) goes back to waiting,
  which woke the CPU once for nothing after each task wake-up.  With the
  I2C change this took the sleeping board from 8.2 mA to 3.0 mA.

* The ``full`` configuration scales the CPU frequency
  (``ESP32S3_DFS``): 240 MHz whenever anything runs, 80 MHz while the CPU is
  idle.  The idle loop lowers the frequency before it waits for an
  interrupt, and every interrupt raises it again.  Idling at 80 MHz rather
  than 240 MHz saves 14 mA.
* The ``full`` configuration uses the tickless scheduler
  (``ESP32S3_TICKLESS``, with ``USEC_PER_TICK`` kept at 10000 so the 32-bit
  tick counter does not wrap within days).  It saves no measurable power by
  itself, but it is what lets the CPU stay asleep for longer than a tick.
  The ESP32-S3 tickless driver could lose its alarm: set to "counter +
  0" for a timer already due, the target was behind the counter by the
  time the comparator loaded it and never fired, and with it no timer of
  the system ever did again (every ``usleep`` and timed wait hung, while
  interrupts still worked).  The alarm is now kept at least 2 us ahead
  and checked after loading, its interrupt is cleared before it is armed
  rather than after, and a light sleep that moves the counter past it
  sets it again (``esp32s3_tickless_resync()``).
* The start code now loads the chip's calibrated core voltages from eFuse
  (``esp_rtc_init()``, as the ESP32 and RISC-V Espressif ports already did).
  Without them the HAL applies conservative defaults, about 40 mV higher on
  this board's chip, and frequency scaling cost 3 mA instead of saving it.
* Chips whose power rails are off are no longer fed through their pins.  A
  line held high into an unpowered chip powers it through its input
  protection.  The GPS UART's transmit line fed the unpowered GPS about
  25 mA; the ``gps_en`` device now holds it low while the rail is off and
  hands it back to the UART when the rail is switched on.
* The SX1262 was fed about 15 mA the same way, through its SPI chip select.
  Its rail is now switched on at boot and the radio put into its sleep mode
  instead (``LILYGO_TDECK_MAX_BOOT_LORA_POWER``, on by default); its first
  SPI access wakes it.

.. warning::

   Do not hold the SX1262's chip select low while its rail is off.  The
   unpowered chip then loads the SPI clock and data lines it shares with the
   e-paper and the microSD card, and the e-paper stops receiving commands:
   the display freezes while its driver sees no error.  Switching the LoRa
   rail off through ``lora_en`` still works, with the chip select high, but
   costs about 15 mA more than leaving the radio powered and asleep.

The draw can be measured with USB connected: setting ``EN_HIZ`` (bit 7 of
the SY6970's register 0x00) disconnects USB power, so the board runs from
the battery and the fuel gauge reports its current, while USB data keeps
working.  It must be cleared again afterwards; the charger driver clears it
at boot.  With light sleep the USB console goes away as soon as USB power
does, so the measurement has to run on the device, from a script that sets
``EN_HIZ``, logs ``batterydump`` output to a file and clears it again.

Frequency scaling uses the HAL's power management, which the build enables
for the HAL (``CONFIG_PM_ENABLE``) only with ``ESP32S3_DFS``; NuttX's own
power management (``CONFIG_PM``) stays off.  The idle frequency cannot go
below 80 MHz: below that the APB clock drops too, and peripheral timing
with it.  ``up_udelay()`` counts CPU cycles through the ROM, so busy-wait
delays stay right at either frequency.

.. note::

   After BLE has been brought up, a reset that does not switch the
   controller off first leaves the radio's RF section powered, about 13 mA,
   until the board is power cycled.  ``reboot`` does switch it off: the BLE
   adapter registers a shutdown handler, which ``board_reset()`` runs.  A USB
   reset (esptool), ``reboot 1`` or a crash do not; bringing BLE up and
   running ``reboot``, or starting and stopping Wi-Fi once (``ifup wlan0``
   then ``ifdown wlan0``), switches it off.  A JTAG session similarly leaves
   the chip drawing about 5 mA more until the next reset.

.. warning::

   The Espressif HAL objects do not depend on the configuration, so after
   changing an ``ESP32S3_*`` option such as the CPU frequency, run
   ``make clean``.  Otherwise the old value stays compiled in without any
   warning.

User interface
==============

In ``full`` the panel shows pnut-os's ``shell``, a phone user interface
built on LVGL 9: home screen, messages (SMS and the LoRa mesh), phone,
settings.  Its source and its documentation are in the pnut-os repository;
what matters for the board:

* The framebuffer and LVGL agree on the pixel format.  ``/dev/fb0`` is
  1 bpp, most significant bit first, a set bit white; LVGL's
  ``LV_COLOR_FORMAT_I1`` is the same, so the display driver copies rows
  (after the 8-byte palette LVGL puts in front) and calls ``FBIO_UPDATE``
  once per frame.  LVGL already widens 1-bit areas to whole bytes.  The
  shell reads the size and format from the framebuffer, so the same code
  runs on colour screens too (``sim:pnut``).
* ``LCD_UC8253_ASYNC`` makes ``FBIO_UPDATE`` return at once and folds what
  is drawn during a refresh into the next one, so typing fast costs a
  refresh per burst; the shell also redraws a text field only every 600 ms
  while typing (Settings > Display > Typing echo).
* The shell's refresh modes (Settings > Display) map to the driver: Quality
  asks for a full refresh every time, Balanced leaves partial refreshes
  with a full one every N, Fast never forces one.  A full one also runs on
  unlock.
* The keyboard follows the design's map: W/S move the focus, A/D turn the
  page, Q/E are Back and Enter; Alt+Enter, Alt+Backspace and Shift+Space
  are Options, Back and the previous page; Sym is Find.
* The glass keys under the panel (``/dev/kbd1``, see Touch) are Home,
  Messages and Phone (Home on Home locks); touch arrives on ``/dev/input0``
  in panel coordinates, and the whole softkey bar takes taps.
* The shell locks itself after ``ui.lock_after`` seconds without input
  (60 by default) with the design's sleep screen.  The power key
  (``/dev/kbd2``) locks from any screen and is the only thing that unlocks
  (or any keyboard key, with ``ui.unlock`` set to ``key``); touch and the
  glass keys never do.
* The touch panel reports the whole glass: taps at the bottom edge came in
  at y = 317-319 and at the right at x = 235-238, so the softkey bar needs
  no margin.
* Themes (colours, fonts, metrics) are files in ``/data/themes`` or on the
  SD card, and fonts in them load through LVGL's POSIX drive, so the
  ``full`` configuration has ``LV_USE_FS_POSIX`` on letter A.  After
  changing any ``LV_`` option, delete LVGL's objects: its build does not
  notice.
* With ``PNUT_SHELL_KEYFIFO`` the shell takes keys from
  ``/var/run/pnut-shell-keys``, which makes it scriptable from the USB
  console (``echo enter > /var/run/pnut-shell-keys``).  The keys are real:
  Enter in a conversation sends.

* The shell speaks English or Polish (``sys.language``).  Its catalogues
  are gettext ``.mo`` files (``LIBC_LOCALE_GETTEXT``) in a ROMFS image
  linked into pnut-os; svcd registers it as a ROM disk
  (``BOARDCTL_ROMDISK``, ``/dev/ram2``) and mounts it at
  ``/usr/share/pnut``, which ``LIBC_LOCALE_PATH`` points into.  The fonts
  cover Latin Extended-A, so Polish letters show; typing them is not done
  yet.

To see what is on the panel without looking at it, read the driver's shadow
framebuffer over JTAG (``g_epaperdev.shadow_fb``, 9600 bytes, the panel's
format)::

    (gdb) dump binary memory screen.bin &g_epaperdev.shadow_fb[0] &g_epaperdev.shadow_fb[9600]

and open it as a 240x320 1-bit image (``Image.frombytes("1", (240, 320),
data)`` in Pillow).

System log, crash reports and adb
=================================

The ``full`` configuration keeps a log across resets and can be reached
from a computer over the network; pnut-os's ``docs/logging.md`` describes
how it is used.

* **The RAM log** (``/dev/kmsg``) is 64 KB in PSRAM
  (``RAMLOG_BUFSIZE``), in a section the start code does not clear:
  ``.ext_ram.noinit``, at the end of the external-RAM BSS
  (``XTENSA_EXTMEM_BSS``; the linker script's ``_ext_ram_noinit_start``
  marks where ``esp32s3_start.c`` stops clearing).  The RAM log keeps its
  contents when its header's magic number survived, so after a software
  reset, a flash or an EN reset it still holds the boot before; a power
  cut clears it.  ``SPIRAM_MEMTEST`` is off, as it overwrote all of PSRAM
  at boot.  Moving the external-RAM BSS on also moved the Wi-Fi
  libraries' 9 KB of BSS out of internal RAM.
* **Lines** carry a millisecond timestamp, the priority and the task
  (``SYSLOG_TIMESTAMP``, ``SYSLOG_TIMESTAMP_MS``, ``SYSLOG_PRIORITY``,
  ``SYSLOG_PROCESS_NAME``): ``[   12.345] [  WARN] modemd: ...``.
* **A panic resets the board** (``BOARD_RESET_ON_ASSERT=1``) instead of
  hanging.  ``board_reset()`` writes the PSRAM data cache back first
  (``Cache_WriteBack_All``, from ROM), so the panic's dump is in the RAM
  log at the next boot, where logd saves it as a crash report.
* ``/var/log`` is a pseudo-filesystem soft link to ``/data/log``
  (``FS_LINKS``).  A soft link into a mounted volume used to lose the
  rest of the path: opening ``/var/log/system.log`` opened the directory.
  ``inode_search()`` kept that rest as a pointer into its buffer, which
  following the link freed and reused; it now keeps a copy.
* ``/bin`` is binfs (``FS_BINFS``), mounted by the board: the built-in
  programs as files, for ``posix_spawn()``.
* **adb** (``SYSTEM_ADBD``, microADB with libuv) runs over TCP, port 5555,
  with shell, file and logcat services; pnut-os's sysd starts it when the
  setting ``dev.adb`` is on.  Only computers whose keys are in
  ``/data/adb/adb_keys`` connect: microADB's own key check was a stub
  that accepted everyone, and ``apps/system/adb/adb_auth.c`` now verifies
  the RSA signature.  ``/dev/urandom`` comes from the hardware RNG
  (``DEV_URANDOM_ARCH``) for adb's tokens.  USB adb is not set up: the
  one USB port stays the USB-Serial-JTAG console and JTAG.
* **kill** works (``SIG_DEFAULT``), and killing a task ends its threads at
  once (``GROUP_KILL_CHILDREN_TIMEOUT_MS=0``; the default waited for ever
  for them to exit and left the group half torn down).  A thread killed
  while it waited in a call (``poll``, ``accept``, ``read``) used to exit
  from inside it, leaving the call's poll registrations in drivers and
  its references to files behind: a later notification called into the
  dead thread's stack (killing meshd crashed msgd).  With
  ``CANCELLATION_POINTS`` the cancelled threads are woken from their
  calls and unwind, and the default kill action (``sig_default.c``) lets
  the signalled thread leave its call first, then exit.  A local socket's
  address can also be taken over by a new server once its old owner's
  process is gone (``net/local/local_bind.c``), so init can restart a
  daemon.
* Ctrl-C on the USB console interrupts the running command
  (``TTY_SIGINT``).

On-device terminal
==================

The panel and the keyboard can also form a terminal running NSH, so the
device can be used without a computer; it is independent of the USB
console, which keeps its own shell.  In ``full`` it no longer starts at
boot, because the pnut-os shell owns the panel: to get it back, disable
``PNUT_SHELL`` and enable ``LILYGO_TDECK_MAX_BOOT_TERMINAL``, or run
``nxterm`` from the USB console in a build without the shell.  The two
cannot share the panel.

``nxterm`` (``apps/examples/nxterm``) draws the text through NX onto
``/dev/lcd0`` in the X11 6x13 font.  Its NSH session runs on a
pseudo-terminal, and a bridge task, ``NxTermPTY``, turns ``/dev/kbd0`` events
into input for that session and passes the session's output to the screen.

The terminal is started by NxInit.  The board carries its own
``src/etc/init.d/init.rc``, which replaces the common ESP32-S3 one (the
board directory comes first on the build's ``VPATH``) and adds a
``terminal`` service when ``LILYGO_TDECK_MAX_BOOT_TERMINAL`` is enabled.
The service discards nxterm's start-up messages; if the screen stays blank,
disable the option and run ``nxterm`` from the USB console to see them.

* A keystroke costs a small partial refresh of about 0.7 s.  Keys typed
  faster than that are merged into one refresh.  Output that scrolls redraws
  most of the screen as one partial refresh, and every sixteenth refresh is
  a full one, which flashes.
* Typing ``exit`` ends the terminal until the next reset.  In PTY mode
  nxterm leaves its window, ``/dev/nxterm0`` and the bridge task behind
  when its shell ends, so it cannot simply be started again; the service is
  ``oneshot`` for that reason.

.. warning::

   Use NX in LCD mode (``NX_LCDDRIVER``, the default) at 1 bpp.  In
   framebuffer mode NX's fill code is broken below 8 bpp: it writes the raw
   colour where it should repeat the pixel across the byte, and uses a pixel
   count as a byte count.  On this panel that drew vertical stripes and no
   text.

Colours at 1 bpp default to 0, which is black on this panel.  The
configuration sets ``NX_BGCOLOR`` and the NxTK border colours to 1, white,
or every start would first paint the screen black.

``NSH_DISABLE_ECHOBACK`` must stay disabled.  NuttX's ``readline`` echoes
only its own line edits and leaves ordinary characters to the terminal
driver's ``ECHO`` flag, so the option switches echo off altogether on the
terminal's pseudo-terminal.

Memory model with the radios enabled
====================================

``ESP32S3_SPIRAM_COMMON_HEAP`` (PSRAM appended to the single heap) is not
available together with Wi-Fi, so the ``wifi`` and ``blewifi`` configurations
fall through to ``ESP32S3_SPIRAM_USER_HEAP``.  That selects
``MM_KERNEL_HEAP`` and splits the heap in two, which ``free`` then reports
separately: roughly 260-280 KB of internal RAM as ``Kmem`` and the whole
8 MB of PSRAM as ``Umem``.

Two consequences:

* The board must provide ``include/board_memorymap.h``; the allocator uses it
  as soon as the kernel heap exists.  Every other ESP32-S3 board carries the
  same file.
* Switching a configured tree between heap models needs ``make clean``.  The
  object files carry the old model and the link then fails with an undefined
  reference to ``up_allocate_kheap``.

Status
======

Verified on hardware (2026-09-20/21):

* Boot to NSH over USB-Serial-JTAG; the 8 MB quad PSRAM is detected and
  appears in the heap.
* XL9555: ``lora_en``, ``gps_en``, ``modem_pwr`` and ``modem_pwrkey`` were
  toggled with the expected effect; ``imu_en``, ``motor_en``, ``touch_rst``
  and ``key_rst`` are in their default state (their I2C devices answer).
  ``lora_ant`` was toggled and the real pin level (XL9555 input register)
  followed it; ``amp_en`` switches the speaker amplifier (see Audio);
  ``audio_sel`` was not exercised.
* I2C scan finds the ES8311 (0x18), touch (0x1a), XL9555 (0x20), BHI260AP
  (0x28), TCA8418 (0x34), BQ27220 (0x55), DRV2605 (0x5a) and SY6970 (0x6a).
* microSD: a FAT32 card mounts; 8 KB of unique data written, unmounted,
  remounted and read back identical.  256 KB writes at about 113 KB/s and
  reads at about 1.4 MB/s (polled SPI, no DMA).
* BQ27220 fuel gauge: voltage, current, temperature, state of charge and
  capacities read back sensibly (it is still at factory defaults: 1500 mAh
  design capacity, 0 cycles, so its percentage is not yet meaningful).  The
  SY6970 charger reports the battery charging.
* SX1262 over the shared SPI bus: the sync-word register reads ``14 24``; the
  radio starts receiving with the TCXO (2.4 V on DIO3) and DIO2 as RF switch.
* GNSS: NMEA received on UART1 at 38400 baud, and parsed into uORB satellite
  messages through the GNSS upper half.
* IMU: accelerometer (about 1 g at rest) and gyroscope samples through uORB
  at 50 and 100 Hz, on USB and on battery, after the firmware upload.
* Audio: a 500 Hz tone played with ``nxplayer`` is heard from the speaker;
  a voice recorded with ``nxrecorder`` (16 kHz mono) and played back is
  clear, with no clicks or lost samples (about -22 dBFS speech); recording
  and playback work back to back in either order; with the kernel heap
  exhausted a recording stops short and ``stop`` still returns; idle current
  on battery is the same with and without the audio driver.
* LoRa: packets transmitted through ``/dev/lora0``, with send times matching
  the time on air; DIO1 wakes the chip from light sleep.  ``lora_ant``
  switches between the internal antenna and the external connector (see
  LoRa).
* Modem: it answers AT commands on UART2 and identifies itself, reports
  the cell it hears and that no SIM is inserted, and powers up and down
  through ``modem on``/``modem off`` (see Modem).
* JTAG: OpenOCD and GDB over the USB Serial/JTAG unit, with a breakpoint,
  backtrace, memory reads and ``finish`` inside the board bring-up code.
* Wi-Fi: ``wlan0`` registers, scanning returns real access points, and a
  WPA2-CCMP station association to a 2.4 GHz hotspot completes: DHCP leases an
  address, ICMP reaches both the gateway and the public internet with no loss,
  DNS resolves, and a TCP connection to a public HTTP server returns a full
  response.  PSRAM stays available as the user heap.
* Bluetooth LE: ``bnep0`` registers, the controller reports its address and
  feature pages, scanning decodes real advertisers (including their names),
  and advertising starts.
* Wi-Fi and BLE scanning concurrently, both returning results, with the
  coexistence arbiter enabled.
* The e-paper panel: full and partial refreshes, including erasing, and the
  first refresh after boot clearing it to white.
* Both backlights, and every layer and modifier of the keyboard.
* Fuel gauge and charger: voltage, state of charge and temperature read
  back; on USB the gauge reports full and the charger charge done; on the
  battery alone the gauge reports discharging, at 102 mA with the board
  idle; plugged back in, the charger reports charging.  The gauge's design
  capacity is set and reads back as 1400 mAh.
* The touch controller answers, reports its resolution and wakes from deep
  sleep when ``/dev/input0`` is opened.
* Idle current: 39 mA with the terminal up and the radios off or asleep,
  down from 98 mA, with the e-paper refreshing, the microSD card mounting,
  the SX1262 waking on its first SPI access and GPS NMEA arriving with its
  rail on.
* The vibration motor driver: ``haptic`` commands complete with the expected
  timing, the chip returns to standby afterwards, and effect slots are freed.
* The on-device terminal: it starts at boot, shows the NSH banner and
  prompt, echoes typed characters, handles backspace and shows command
  output.
* The pnut-os user interface (2026-09-23): it starts at boot, and every
  screen was driven through its key FIFO and read back from the panel's
  shadow framebuffer over JTAG, taps included (fed to the LVGL pointer the
  same way the touch panel feeds it).

Not verified:

* A GNSS position fix: tried only indoors, from a cold start.
* With a SIM (Orange Polska), registration on LTE, the network's time and
  SMS both ways are verified; calls fail (no voice over LTE, and no GSM
  found to fall back to, see Modem), and a data connection is not tried.
* The user interface with fingers and keys on the device itself (so far
  only scripted from the USB console), and the ``sym`` + enter, backspace
  and space codes.
* Recovery from repeated failed associations: after a few attempts against an
  access point that was no longer present, ``wapi scan wlan0`` began returning
  an empty list, including for networks that were definitely in range.  A board
  reset cleared it each time, but the underlying cause was not investigated.
* Wi-Fi throughput: ``iperf`` needs a peer on the same subnet and none was
  reachable, so no throughput figure has been measured.
* Wi-Fi credentials do not survive a reboot.  ``WIRELESS_WAPI_CONFIG_PATH``
  points at ``/tmp/wapi.conf`` on tmpfs, because the board has no dedicated
  place to keep them yet; the microSD card or a flash partition would be the
  natural home.
* BLE advertising is not fully characterised: the first attempt after boot
  once failed with ``EIO`` (an HCI command timeout) and every attempt since
  has succeeded, with Wi-Fi both up and down.
* The debug session through the VS Code UI (only OpenOCD and GDB were driven).
* Charging from a mostly empty cell, and how closely the state of charge
  follows the cell over a full discharge.

Not implemented yet: the modem's audio path through ``audio_sel``.
