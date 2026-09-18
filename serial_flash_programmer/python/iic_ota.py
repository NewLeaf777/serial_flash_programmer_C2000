#!/usr/bin/env python3
"""
Python ctypes wrapper for the iic_ota_f28379() / iic_ota_f280049() C API
(../include/iic_ota.h, ../source/iic_ota.cpp).

This calls into libiic_ota.so, so the library must be built as a shared
object first:

    cd serial_flash_programmer
    mkdir -p build && cd build
    cmake -DIIC_OTA_SHARED=ON ..
    make iic_ota

Library usage:

    from iic_ota import iic_ota_f28379, F28379_CPU1, IicOtaError

    try:
        iic_ota_f28379(F28379_CPU1, "blinky.txt", "/dev/ttyUSB0", 9600)
    except IicOtaError as e:
        print("update failed:", e)

    # F280049 Live DFU where the DEVICE picks the bank:
    from iic_ota import iic_ota_f280049, IicOtaError

    try:
        iic_ota_f280049("bank0.txt", "bank1.txt", "/dev/ttyUSB0", 9600)
    except IicOtaError as e:
        print("update failed:", e)

Command-line usage:

    python3 iic_ota.py --target f28379_cpu1 --file blinky.txt \
        --port /dev/ttyUSB0 --baud 9600

    python3 iic_ota.py -t f280049 --bank0-file bank0.txt --bank1-file bank1.txt \
        --port /dev/ttyUSB0 --baud 9600

`--target`/`-t` is one of: f280049, f28379_cpu1, f28379_cpu2.
"""

import argparse
import ctypes
import os
import select
import sys
import termios
import time

#*****************************************************************************
# f28379_target_t -- must match include/iic_ota.h
#*****************************************************************************
F28379_CPU1 = 0xCC
F28379_CPU2 = 0xDD

F28379_TARGETS = {
    "F28379_CPU1": F28379_CPU1,
    "F28379_CPU2": F28379_CPU2,
}

#*****************************************************************************
# IIC_OTA_ERR_* -- must match include/iic_ota.h
#*****************************************************************************
IIC_OTA_SUCCESS = 0
IIC_OTA_ERR_INVALID_ARG = -1
IIC_OTA_ERR_UNSUPPORTED_BAUD = -2
IIC_OTA_ERR_FILE_OPEN = -3
IIC_OTA_ERR_PORT_OPEN = -4
IIC_OTA_ERR_PORT_CONFIG = -5
IIC_OTA_ERR_AUTOBAUD_TIMEOUT = -6
IIC_OTA_ERR_COMMAND_NAK = -7
IIC_OTA_ERR_COMMAND_TIMEOUT = -8
IIC_OTA_ERR_IMAGE_FORMAT = -9
IIC_OTA_ERR_IMAGE_TRANSFER = -10
IIC_OTA_ERR_BANK_SELECT = -11

_ERROR_MESSAGES = {
    IIC_OTA_SUCCESS: "success",
    IIC_OTA_ERR_INVALID_ARG: "invalid argument (bad target, or empty file/port path)",
    IIC_OTA_ERR_UNSUPPORTED_BAUD: "unsupported baud rate",
    IIC_OTA_ERR_FILE_OPEN: "could not open firmware file",
    IIC_OTA_ERR_PORT_OPEN: "could not open serial port",
    IIC_OTA_ERR_PORT_CONFIG: "failed to configure serial port",
    IIC_OTA_ERR_AUTOBAUD_TIMEOUT: "autobaud lock timed out (no/garbled echo from device)",
    IIC_OTA_ERR_COMMAND_NAK: "device NAKed the Live DFU command",
    IIC_OTA_ERR_COMMAND_TIMEOUT: "timed out waiting for ACK/NAK to the command packet",
    IIC_OTA_ERR_IMAGE_FORMAT: "firmware file is not valid SCI-8 boot format",
    IIC_OTA_ERR_IMAGE_TRANSFER: "image transfer failed (checksum mismatch or timeout)",
    IIC_OTA_ERR_BANK_SELECT: "device's bank-select readback bytes were mismatched or unrecognized",
}


class IicOtaError(RuntimeError):
    """Raised by iic_ota_f28379()/iic_ota_f280049() for any non-success
    IIC_OTA_ERR_* return code."""

    def __init__(self, code):
        self.code = code
        super().__init__(
            "iic_ota failed: %s (code %d)" % (_ERROR_MESSAGES.get(code, "unknown error"), code)
        )


def _default_library_path():
    """Look for libiic_ota.so in the usual build locations next to this
    file; fall back to letting the dynamic linker search (LD_LIBRARY_PATH,
    ldconfig, etc.) if none of those exist."""
    here = os.path.dirname(os.path.abspath(__file__))
    for candidate in (
        os.path.join(here, "libiic_ota.so"),
        os.path.join(here, "..", "build", "libiic_ota.so"),
        os.path.join(here, "..", "libiic_ota.so"),
    ):
        if os.path.isfile(candidate):
            return candidate
    return "libiic_ota.so"


_lib = None


def _load_library(path=None):
    global _lib
    if path is None and _lib is not None:
        return _lib

    lib = ctypes.CDLL(path or _default_library_path())
    lib.iic_ota_f28379.argtypes = [
        ctypes.c_int,  # f28379_target_t
        ctypes.c_char_p,  # firmware_file
        ctypes.c_char_p,  # serial_port
        ctypes.c_int,  # baudrate
    ]
    lib.iic_ota_f28379.restype = ctypes.c_int

    lib.iic_ota_f280049.argtypes = [
        ctypes.c_char_p,  # bank0_firmware_file
        ctypes.c_char_p,  # bank1_firmware_file
        ctypes.c_char_p,  # serial_port
        ctypes.c_int,  # baudrate
    ]
    lib.iic_ota_f280049.restype = ctypes.c_int

    if path is None:
        _lib = lib
    return lib


def iic_ota_f28379(target, firmware_file, serial_port, baudrate, library_path=None):
    """
    Python wrapper for iic_ota_f28379() (include/iic_ota.h). Performs a Live
    DFU firmware update over a serial UART to an F28379 (f2837xD) CPU core;
    assumes the target's SCI flash kernel is already running and
    autobaud-ready.

    Params:
        target:        one of F28379_CPU1, F28379_CPU2.
        firmware_file: path to an ASCII SCI-8 boot-format file.
        serial_port:   POSIX device path, e.g. "/dev/ttyUSB0".
        baudrate:      300/600/1200/1800/2400/4800/9600/19200/38400/
                       57600/115200.
        library_path:  optional explicit path to libiic_ota.so.

    Raises IicOtaError on any failure. Returns None on success.

    For F280049 targets, use iic_ota_f280049() -- this function is
    F28379-only.
    """
    lib = _load_library(library_path)
    rc = lib.iic_ota_f28379(
        target,
        firmware_file.encode("utf-8"),
        serial_port.encode("utf-8"),
        baudrate,
    )
    if rc != IIC_OTA_SUCCESS:
        raise IicOtaError(rc)


def iic_ota_f280049(bank0_firmware_file, bank1_firmware_file, serial_port, baudrate, library_path=None):
    """
    Python wrapper for iic_ota_f280049() (include/iic_ota.h). Performs a Live
    DFU firmware update for an F280049-class device where the DEVICE, not the
    caller, decides which flash bank gets written: after the command ACK, the
    device sends back a 2-byte bank-select readback (0xB0 0xB0 for bank 0,
    0xB1 0xB1 for bank 1), and whichever of bank0_firmware_file /
    bank1_firmware_file matches gets streamed.

    Params:
        bank0_firmware_file: path to the bank 0 ASCII SCI-8 boot-format file,
                              used only if the device selects bank 0.
        bank1_firmware_file: path to the bank 1 ASCII SCI-8 boot-format file,
                              used only if the device selects bank 1. Both
                              paths are required even though only one file is
                              actually transferred.
        serial_port:          POSIX device path, e.g. "/dev/ttyUSB0".
        baudrate:             300/600/1200/1800/2400/4800/9600/19200/38400/
                              57600/115200.
        library_path:         optional explicit path to libiic_ota.so.

    Raises IicOtaError on any failure (including IIC_OTA_ERR_BANK_SELECT if
    the device's readback bytes are mismatched or not one of 0xB0/0xB1).
    Returns None on success.

    For F28379 (CPU1/CPU2) targets, use iic_ota_f28379() -- this function is
    F280049-only.
    """
    lib = _load_library(library_path)
    rc = lib.iic_ota_f280049(
        bank0_firmware_file.encode("utf-8"),
        bank1_firmware_file.encode("utf-8"),
        serial_port.encode("utf-8"),
        baudrate,
    )
    if rc != IIC_OTA_SUCCESS:
        raise IicOtaError(rc)


LIVE_UPDATE_TRIGGER_PACKET = bytes.fromhex("68 06 00 06 00 68 43 00 c2 49 00 4e 16")
LIVE_UPDATE_TRIGGER_ACK = bytes.fromhex("68 06 00 06 00 68 93 01 D1 C2 00 27 16")

RETRIEVE_FIRMWARE_VERSION_PACKET = bytes.fromhex("68 04 00 04 00 68 43 00 c1 04 16")
RETRIEVE_FIRMWARE_VERSION_PACKET_RESPONSE = bytes.fromhex("68 10 00 10 00 68 83 01 c1 01 00 00 00 02 00 00 00 00 00 00 00 48 16")

_TERMIOS_BAUDS = {
    300: termios.B300,
    600: termios.B600,
    1200: termios.B1200,
    1800: termios.B1800,
    2400: termios.B2400,
    4800: termios.B4800,
    9600: termios.B9600,
    19200: termios.B19200,
    38400: termios.B38400,
    57600: termios.B57600,
    115200: termios.B115200,
}

def retrieve_firmware_version(serial_port, baudrate, timeout=3.0):
    """
    Sends RETRIEVE_FIRMWARE_VERSION_PACKET to the DSP and waits for its reply.

    Returns the reply as bytes if successful, or False on failure.
    """
    speed = _TERMIOS_BAUDS.get(baudrate)
    if speed is None:
        return False

    try:
        fd = os.open(serial_port, os.O_RDWR | os.O_NOCTTY)
    except OSError:
        return False

    try:
        attrs = termios.tcgetattr(fd)
        attrs[0] = termios.IGNPAR  # iflag
        attrs[1] = 0  # oflag
        attrs[2] = termios.CS8 | termios.CLOCAL | termios.CREAD  # cflag
        attrs[3] = 0  # lflag
        attrs[4] = speed  # ispeed
        attrs[5] = speed  # ospeed
        attrs[6][termios.VMIN] = 0
        attrs[6][termios.VTIME] = 0
        termios.tcsetattr(fd, termios.TCSANOW, attrs)
        termios.tcflush(fd, termios.TCIOFLUSH)

        os.write(fd, RETRIEVE_FIRMWARE_VERSION_PACKET)

        print("Waiting for firmware version from device...")

        reply = b""
        deadline = time.monotonic() + timeout
        while len(reply) < len(RETRIEVE_FIRMWARE_VERSION_PACKET_RESPONSE):
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                break
            ready, _, _ = select.select([fd], [], [], remaining)
            if not ready:
                break
            chunk = os.read(fd, len(RETRIEVE_FIRMWARE_VERSION_PACKET_RESPONSE) - len(reply))
            if not chunk:
                break
            reply += chunk

        print("Received %d bytes: %s" % (len(reply), reply.hex()))
        return reply
    except (OSError, termios.error):
        print("Error sending RETRIEVE_FIRMWARE_VERSION_PACKET or reading reply", file=sys.stderr)
        return False
    finally:
        os.close(fd)

def send_live_update_trigger(serial_port, baudrate, timeout=3.0):
    """
    Sends LIVE_UPDATE_TRIGGER_PACKET to make the DSP enter live update mode
    and waits for its reply.

    Returns True only if the reply is exactly LIVE_UPDATE_TRIGGER_ACK.
    Returns False for a NACK, any other/short reply, a timeout, an
    unsupported baud rate, or a serial port that can't be opened/configured.

    Params:
        serial_port: POSIX device path, e.g. "/dev/ttyUSB0".
        baudrate:    300/600/1200/1800/2400/4800/9600/19200/38400/57600/115200.
        timeout:     seconds to wait for the full reply (default 1.0).
    """
    speed = _TERMIOS_BAUDS.get(baudrate)
    if speed is None:
        return False

    try:
        fd = os.open(serial_port, os.O_RDWR | os.O_NOCTTY)
    except OSError:
        return False

    try:
        attrs = termios.tcgetattr(fd)
        attrs[0] = termios.IGNPAR  # iflag
        attrs[1] = 0  # oflag
        attrs[2] = termios.CS8 | termios.CLOCAL | termios.CREAD  # cflag
        attrs[3] = 0  # lflag
        attrs[4] = speed  # ispeed
        attrs[5] = speed  # ospeed
        attrs[6][termios.VMIN] = 0
        attrs[6][termios.VTIME] = 0
        termios.tcsetattr(fd, termios.TCSANOW, attrs)
        termios.tcflush(fd, termios.TCIOFLUSH)

        os.write(fd, LIVE_UPDATE_TRIGGER_PACKET)

        print("Waiting for LIVE_UPDATE_TRIGGER_ACK from device...")

        reply = b""
        deadline = time.monotonic() + timeout
        while len(reply) < len(LIVE_UPDATE_TRIGGER_ACK):
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                break
            ready, _, _ = select.select([fd], [], [], remaining)
            if not ready:
                break
            chunk = os.read(fd, len(LIVE_UPDATE_TRIGGER_ACK) - len(reply))
            if not chunk:
                break
            reply += chunk
        
        print("Received %d bytes: %s" % (len(reply), reply.hex()))

        return reply == LIVE_UPDATE_TRIGGER_ACK
    except (OSError, termios.error):
        print("Error sending LIVE_UPDATE_TRIGGER_PACKET or reading reply", file=sys.stderr)
        return False
    finally:
        os.close(fd)


_CLI_TARGETS = {
    "f280049": "f280049",
    "f28379_cpu1": F28379_CPU1,
    "f28379_cpu2": F28379_CPU2,
}


def main(argv=None):
    parser = argparse.ArgumentParser(description="Call iic_ota_f28379()/iic_ota_f280049() to perform a Live DFU firmware update.")
    parser.add_argument("--target", "-t", required=True, type=str.lower, choices=sorted(_CLI_TARGETS),
                         help="update target: %s" % ", ".join(sorted(_CLI_TARGETS)))
    parser.add_argument("--file", "-f", help="path to the SCI-8 boot-format firmware file (with --target f28379_cpu1/f28379_cpu2)")
    parser.add_argument("--bank0-file", "-b0", help="bank 0 SCI-8 boot-format file (with --target f280049)")
    parser.add_argument("--bank1-file", "-b1", help="bank 1 SCI-8 boot-format file (with --target f280049)")
    parser.add_argument("--port", "-p", default="/dev/ttyACM0", help="serial device (default /dev/ttyACM0)")
    parser.add_argument("--baud", "-b", type=int, default=38400, help="baud rate (default 38400)")
    parser.add_argument("--lib", "-l", default=None, help="path to libiic_ota.so (default: auto-detect)")
    args = parser.parse_args(argv)

    target = _CLI_TARGETS[args.target]

    if target == "f280049":

        retrieve_firmware_version(args.port, 115200)
        send_live_update_trigger(args.port, 115200)
        time.sleep(0.01)  # give the device a moment to switch to live update mode

        if not args.bank0_file or not args.bank1_file:
            parser.error("--target f280049 requires --bank0-file and --bank1-file")
        print("iic_ota_f280049: bank0_file=%s bank1_file=%s port=%s baud=%d" %
              (args.bank0_file, args.bank1_file, args.port, args.baud))
        try:
            iic_ota_f280049(args.bank0_file, args.bank1_file, args.port, args.baud, library_path=args.lib)
        except IicOtaError as e:
            print("FAILED: %s" % e, file=sys.stderr)
            return 1
        except OSError as e:
            print("FAILED to load library: %s" % e, file=sys.stderr)
            return 1
    else:
        if not args.file:
            parser.error("--target %s requires --file" % args.target)
        print("iic_ota_f28379: target=0x%02x file=%s port=%s baud=%d" % (target, args.file, args.port, args.baud))
        try:
            iic_ota_f28379(target, args.file, args.port, args.baud, library_path=args.lib)
        except IicOtaError as e:
            print("FAILED: %s" % e, file=sys.stderr)
            return 1
        except OSError as e:
            print("FAILED to load library: %s" % e, file=sys.stderr)
            return 1

    print("SUCCESS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
