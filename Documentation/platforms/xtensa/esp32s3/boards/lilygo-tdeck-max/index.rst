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
* TCA8418 keyboard controller, CST328/CST3530 touch controller
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

The ``BOOT`` button is on GPIO0.  To enter the ROM download mode by hand,
hold ``BOOT``, press and release ``RST`` on the back, then release ``BOOT``.
``esptool`` normally does this automatically over USB-Serial-JTAG.

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

To power the modem, switch on ``modem_pwr``, wait a second, then pulse
``modem_pwrkey`` high for about 50 ms, as the vendor firmware does.  The
vendor notes that the battery must be connected to use the A7682E.

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
ES8311 I2S              MCLK 38, BCLK 39, WS 18, DOUT 17, DIN 40
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

epaper
------

``nsh`` plus the e-paper panel and the ``fb`` example.  The board carries a
GoodDisplay GDEQ031T10, a 3.1 inch 240x320 monochrome panel driven by a
UC8253 controller, on the SPI2 bus it shares with the microSD slot and the
SX1262.  The driver is ``drivers/lcd/uc8253.c``.

The panel appears both as ``/dev/fb0`` (through the LCD framebuffer front
end) and as ``/dev/lcd0``::

    nsh> fb -p smpte

Refreshing e-paper takes about a second and wears the panel, so the driver
never refreshes on its own.  ``putrun`` and ``putarea`` only update a shadow
framebuffer, and the panel is written and refreshed when ``redraw`` is
called, which through ``/dev/fb0`` is an ``FBIO_UPDATE`` ioctl.  An
application therefore draws as often as it likes and pays for one refresh
when it asks for one.

A few properties worth knowing:

* Pixels are packed **most significant bit first** and a **set bit is
  white**, which is the controller's own layout.  ``LCD_UC8253`` selects
  ``LCD_PACKEDMSFIRST``; enable ``NX_PACKEDMSFIRST`` as well if you drive
  the panel through NX.
* The panel is presented in its **native portrait orientation**, 240x320.
  The driver does not rotate.
* The **first** refresh after a reset drives the panel to white before
  showing the frame, so that it starts from a known image rather than
  whatever the previous firmware left on the glass.  That costs one extra
  refresh, once.
* ``LCD_UC8253_FASTUPDATE`` (on by default) forces the waveform the
  controller would pick at a high temperature, which shortens a full
  refresh from about 3 s to about 1 s.  Turn it off if the panel has to
  work in the cold.
* Boot does **not** refresh the panel: a handheld should not pay a second
  of screen flashing on every reset.  The image already on the glass stays
  there until something asks for an update.

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
  followed it; ``amp_en`` and ``audio_sel`` were not exercised.
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
* GNSS: NMEA received on UART1 at 38400 baud.
* Modem: data from the modem (unsolicited ``+CPIN`` lines) is received on
  UART2.
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

Not verified:

* The radio effect of ``lora_ant``.  A receive-strength comparison of the two
  settings across 863-870 MHz found identical readings: the receiver is
  limited by its own noise floor, so a passive measurement cannot tell the
  antennas apart.  An 868 MHz signal source, or touching the external antenna
  while watching the readings, is needed.
* Transmission to the modem (no reply to ``AT`` was seen).
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

Not implemented yet: drivers for the e-paper, touch, keyboard, IMU, haptic
driver, charger and fuel gauge, the ES8311 audio path, PWM backlights, Wi-Fi
and Bluetooth LE.
