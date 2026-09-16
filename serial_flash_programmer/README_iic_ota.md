# iic_ota library

A small, reentrant C API (with a Python `ctypes` wrapper) that performs a "Live DFU"
firmware update over a serial UART to a TI C2000 device, for four targets:

| `firmware_type_t`  | value  | device            |
|---------------------|--------|--------------------|
| `F280049_BANK0`      | `0xaa` | F280049, flash bank 0 |
| `F280049_BANK1`      | `0xbb` | F280049, flash bank 1 |
| `F28379_CPU1`        | `0xcc` | F28379 (f2837xD), CPU1 |
| `F28379_CPU2`        | `0xdd` | F28379 (f2837xD), CPU2 |

It reimplements the same wire protocol as `serial_flash_programmer`'s interactive
"8-Live DFU" menu item, standalone: no global state, no code shared with the CLI, and
every wait is timeout-bounded (it never hangs the caller). See `include/iic_ota.h` for
the full API doc comments and `source/iic_ota.cpp` for the implementation.

**Assumptions / limitations, read before use:**
- The target's SCI flash kernel must already be running and autobaud-ready on the
  device before calling `iic_ota()` — it does **not** download a kernel, and for
  `F28379_CPU2` it does **not** perform any CPU1→CPU2 SCI hand-off.
- Linux only (POSIX `termios`). Not built or tested on Windows.
- Supported baud rates: 300, 600, 1200, 1800, 2400, 4800, 9600, 19200, 38400, 57600,
  115200. Anything else is rejected before any I/O happens.
- `firmware_file` must be ASCII SCI-8 boot format, i.e. the output of
  `hex2000 -boot -a -sci8 app.out -o app.txt`.
- **The wire command codes are proposals, not verified protocol.** Only `0x0700`
  (used for `F280049_BANK0`) matches what the existing, already-working CLI sends
  today. The other mappings in `wire_command_for()` (`source/iic_ota.cpp`) must be
  confirmed against (or replaced with) whatever the actual device kernel firmware
  expects before this is used against real hardware.

## Building

The library is an additive CMake target (`iic_ota`) alongside the existing
`serial_flash_programmer` executable — building it never changes the executable.

```bash
cd serial_flash_programmer
mkdir -p build && cd build
cmake ..
make iic_ota
```

This produces a **static** library, `build/libiic_ota.a`, by default.

### Shared library (required for the Python wrapper)

```bash
cmake -DIIC_OTA_SHARED=ON ..
make iic_ota
```

Produces `build/libiic_ota.so` instead.

### Skipping the library entirely

```bash
cmake -DBUILD_IIC_OTA_LIB=OFF ..
```

No `iic_ota`/`libiic_ota.*` target is built at all; `serial_flash_programmer` is
unaffected.

## C usage

```c
#include "iic_ota.h"   // serial_flash_programmer/include/iic_ota.h
#include <stdio.h>

int main(void)
{
    int rc = iic_ota(F280049_BANK0, "blinky.txt", "/dev/ttyUSB0", 9600);
    if (rc != IIC_OTA_SUCCESS)
    {
        fprintf(stderr, "update failed, code %d\n", rc);
        return 1;
    }
    printf("update succeeded\n");
    return 0;
}
```

`iic_ota()` is reentrant/thread-safe (no global state) and can be called concurrently
against different serial ports from multiple threads.

### Compiling against the static library

```bash
gcc -o my_app my_app.c \
    -Iserial_flash_programmer/include \
    serial_flash_programmer/build/libiic_ota.a \
    -lstdc++
```

(`-lstdc++` is needed because `iic_ota.cpp` is compiled as C++ internally; the
public API in `iic_ota.h` is plain C, wrapped in `extern "C"`, so it's callable
from a C or C++ program.)

### Compiling against the shared library

```bash
gcc -o my_app my_app.c \
    -Iserial_flash_programmer/include \
    -Lserial_flash_programmer/build -liic_ota

LD_LIBRARY_PATH=serial_flash_programmer/build ./my_app
```

## Python usage

Requires the **shared** library (`-DIIC_OTA_SHARED=ON`, see above) — `ctypes` needs a
`.so`, not a `.a`. The wrapper is `python/iic_ota.py`.

### As a library

```python
import sys
sys.path.insert(0, "serial_flash_programmer/python")
from iic_ota import iic_ota, F280049_BANK0, IicOtaError

try:
    iic_ota(F280049_BANK0, "blinky.txt", "/dev/ttyUSB0", 9600)
    print("update succeeded")
except IicOtaError as e:
    print("update failed:", e, "(code", e.code, ")")
```

`iic_ota()` auto-locates `libiic_ota.so` next to `python/iic_ota.py` or in
`../build/`. If it's somewhere else, pass it explicitly:

```python
iic_ota(F280049_BANK0, "blinky.txt", "/dev/ttyUSB0", 9600,
        library_path="/path/to/libiic_ota.so")
```

### As a CLI

```bash
python3 serial_flash_programmer/python/iic_ota.py \
    --type F280049_BANK0 --file blinky.txt --port /dev/ttyUSB0 --baud 9600 \
    [--lib /path/to/libiic_ota.so]
```

`--type` accepts a name (`F280049_BANK0`, `F280049_BANK1`, `F28379_CPU1`,
`F28379_CPU2`) or a numeric value (e.g. `0xaa`).

## Return / error codes

Defined in `include/iic_ota.h` (and mirrored in `python/iic_ota.py`):

| Code | Name                              | Meaning |
|------|-----------------------------------|---------|
| `0`   | `IIC_OTA_SUCCESS`                 | image sent and ACKed successfully |
| `-1`  | `IIC_OTA_ERR_INVALID_ARG`         | bad `firmware_type_t`, or a NULL/empty file or port path |
| `-2`  | `IIC_OTA_ERR_UNSUPPORTED_BAUD`    | `baudrate` isn't one of the supported values |
| `-3`  | `IIC_OTA_ERR_FILE_OPEN`           | could not open `firmware_file` for reading |
| `-4`  | `IIC_OTA_ERR_PORT_OPEN`           | could not open `serial_port` |
| `-5`  | `IIC_OTA_ERR_PORT_CONFIG`         | `tcsetattr`/`tcgetattr` on the serial port failed |
| `-6`  | `IIC_OTA_ERR_AUTOBAUD_TIMEOUT`    | no, or a garbled, echo to the autobaud byte |
| `-7`  | `IIC_OTA_ERR_COMMAND_NAK`         | device NAKed the Live DFU command packet |
| `-8`  | `IIC_OTA_ERR_COMMAND_TIMEOUT`     | no ACK/NAK reply to the command within the timeout |
| `-9`  | `IIC_OTA_ERR_IMAGE_FORMAT`        | `firmware_file` isn't valid SCI-8 boot format |
| `-10` | `IIC_OTA_ERR_IMAGE_TRANSFER`      | checksum mismatch or timeout during image transfer |

In C, check the return value directly. In Python, a non-success return raises
`IicOtaError`, whose `.code` attribute holds the value above.

## Testing without hardware

There's no unit-test target checked into the build. If you need to exercise this
without a real device, the protocol is simple enough to fake over a Linux PTY pair
(`os.openpty()`): echo the autobaud byte, ACK the 10-byte command packet, then reply
to each checksum handshake with the running sum of bytes actually received so far
(see `source/iic_ota.cpp`'s `download_image_checksum()` for the exact block/checkpoint
framing to replicate on the fake-device side).
