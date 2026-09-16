#!/usr/bin/env python3
"""
Python ctypes wrapper for the iic_ota() C API (../include/iic_ota.h,
../source/iic_ota.cpp).

This calls into libiic_ota.so, so the library must be built as a shared
object first:

    cd serial_flash_programmer
    mkdir -p build && cd build
    cmake -DIIC_OTA_SHARED=ON ..
    make iic_ota

Library usage:

    from iic_ota import iic_ota, F280049_BANK0, IicOtaError

    try:
        iic_ota(F280049_BANK0, "blinky.txt", "/dev/ttyUSB0", 9600)
    except IicOtaError as e:
        print("update failed:", e)

Command-line usage:

    python3 iic_ota.py --type F280049_BANK0 --file blinky.txt \
        --port /dev/ttyUSB0 --baud 9600
"""

import argparse
import ctypes
import os
import sys

#*****************************************************************************
# firmware_type_t -- must match include/iic_ota.h
#*****************************************************************************
F280049_BANK0 = 0xAA
F280049_BANK1 = 0xBB
F28379_CPU1 = 0xCC
F28379_CPU2 = 0xDD

FIRMWARE_TYPES = {
    "F280049_BANK0": F280049_BANK0,
    "F280049_BANK1": F280049_BANK1,
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

_ERROR_MESSAGES = {
    IIC_OTA_SUCCESS: "success",
    IIC_OTA_ERR_INVALID_ARG: "invalid argument (bad firmware type, or empty file/port path)",
    IIC_OTA_ERR_UNSUPPORTED_BAUD: "unsupported baud rate",
    IIC_OTA_ERR_FILE_OPEN: "could not open firmware file",
    IIC_OTA_ERR_PORT_OPEN: "could not open serial port",
    IIC_OTA_ERR_PORT_CONFIG: "failed to configure serial port",
    IIC_OTA_ERR_AUTOBAUD_TIMEOUT: "autobaud lock timed out (no/garbled echo from device)",
    IIC_OTA_ERR_COMMAND_NAK: "device NAKed the Live DFU command",
    IIC_OTA_ERR_COMMAND_TIMEOUT: "timed out waiting for ACK/NAK to the command packet",
    IIC_OTA_ERR_IMAGE_FORMAT: "firmware file is not valid SCI-8 boot format",
    IIC_OTA_ERR_IMAGE_TRANSFER: "image transfer failed (checksum mismatch or timeout)",
}


class IicOtaError(RuntimeError):
    """Raised by iic_ota() for any non-success IIC_OTA_ERR_* return code."""

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
    lib.iic_ota.argtypes = [
        ctypes.c_int,  # firmware_type_t
        ctypes.c_char_p,  # firmware_file
        ctypes.c_char_p,  # serial_port
        ctypes.c_int,  # baudrate
    ]
    lib.iic_ota.restype = ctypes.c_int

    if path is None:
        _lib = lib
    return lib


def iic_ota(firmware_type, firmware_file, serial_port, baudrate, library_path=None):
    """
    Python wrapper for iic_ota() (include/iic_ota.h). Performs a Live DFU
    firmware update over a serial UART; assumes the target's SCI flash
    kernel is already running and autobaud-ready.

    Params:
        firmware_type: one of F280049_BANK0, F280049_BANK1, F28379_CPU1,
                        F28379_CPU2.
        firmware_file:  path to an ASCII SCI-8 boot-format file.
        serial_port:    POSIX device path, e.g. "/dev/ttyUSB0".
        baudrate:       300/600/1200/1800/2400/4800/9600/19200/38400/
                        57600/115200.
        library_path:   optional explicit path to libiic_ota.so.

    Raises IicOtaError on any failure. Returns None on success.
    """
    lib = _load_library(library_path)
    rc = lib.iic_ota(
        firmware_type,
        firmware_file.encode("utf-8"),
        serial_port.encode("utf-8"),
        baudrate,
    )
    if rc != IIC_OTA_SUCCESS:
        raise IicOtaError(rc)


def _parse_firmware_type(text):
    if text in FIRMWARE_TYPES:
        return FIRMWARE_TYPES[text]
    try:
        return int(text, 0)
    except ValueError:
        raise argparse.ArgumentTypeError(
            "must be one of %s, or a numeric value" % ", ".join(FIRMWARE_TYPES)
        )


def main(argv=None):
    parser = argparse.ArgumentParser(description="Call iic_ota() to perform a Live DFU firmware update.")
    parser.add_argument("--type", required=True, type=_parse_firmware_type,
                         help="firmware target: %s (or a numeric value)" % ", ".join(FIRMWARE_TYPES))
    parser.add_argument("--file", required=True, help="path to the SCI-8 boot-format firmware file")
    parser.add_argument("--port", default="/dev/ttyACM0", help="serial device (default /dev/ttyACM0)")
    parser.add_argument("--baud", type=int, default=38400, help="baud rate (default 38400)")
    parser.add_argument("--lib", default=None, help="path to libiic_ota.so (default: auto-detect)")
    args = parser.parse_args(argv)

    print("iic_ota: type=0x%02x file=%s port=%s baud=%d" % (args.type, args.file, args.port, args.baud))
    try:
        iic_ota(args.type, args.file, args.port, args.baud, library_path=args.lib)
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
