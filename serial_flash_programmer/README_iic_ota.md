# iic_ota library

A small, reentrant C API (with a Python `ctypes` wrapper) that performs a "Live DFU"
firmware update over a serial UART to a TI C2000 device. There are two entry points,
one per device family:

- **`iic_ota_f28379(target, ...)`** — F28379 (f2837xD), where the caller picks which
  CPU core to update via `f28379_target_t`:

  | `f28379_target_t` | value  | device                 |
  |--------------------|--------|------------------------|
  | `F28379_CPU1`      | `0xcc` | F28379 (f2837xD), CPU1 |
  | `F28379_CPU2`      | `0xdd` | F28379 (f2837xD), CPU2 |

- **`iic_ota_f280049(bank0_firmware_file, bank1_firmware_file, ...)`** — F280049,
  where the **device**, not the caller, decides which flash bank gets written: it
  sends a single Live DFU trigger, reads back the 2-byte bank-select the device
  replies with (`0xB0 0xB0` for bank 0, `0xB1 0xB1` for bank 1), and streams
  whichever of the two firmware files the caller passed in matches. There's no
  target selector for this one — both bank files are required up front, and the
  device's readback decides which one actually gets sent.

It reimplements the same wire protocol as `serial_flash_programmer`'s interactive
"8-Live DFU" menu item, standalone: no global state, no code shared with the CLI, and
every wait is timeout-bounded (it never hangs the caller). See `include/iic_ota.h` for
the full API doc comments and `source/iic_ota.cpp` for the implementation.

**Assumptions / limitations, read before use:**
- The target's SCI flash kernel must already be running and autobaud-ready on the
  device before calling either function — neither one downloads a kernel, and
  `iic_ota_f28379(F28379_CPU2, ...)` does **not** perform any CPU1→CPU2 SCI hand-off.
- Linux only (POSIX `termios`). Not built or tested on Windows.
- Supported baud rates: 300, 600, 1200, 1800, 2400, 4800, 9600, 19200, 38400, 57600,
  115200. Anything else is rejected before any I/O happens.
- `firmware_file` (and `bank0_firmware_file`/`bank1_firmware_file`) must be ASCII
  SCI-8 boot format, i.e. the output of `hex2000 -boot -a -sci8 app.out -o app.txt`.
- **The wire command codes are proposals, not verified protocol.** Only the command
  `iic_ota_f280049()` sends (`0x0700`, matching the existing `LIVE_DFU_CPU1` code) is
  what the existing, already-working CLI sends today. The `f28379_target_t` mappings
  in `wire_command_for()` (`source/iic_ota.cpp`) are new proposals and must be
  confirmed against (or replaced with) whatever the actual device kernel firmware
  expects before this is used against real hardware. The `0xB0 0xB0` / `0xB1 0xB1`
  bank-select readback `iic_ota_f280049()` expects after its command ACK is likewise
  unverified against real device firmware.

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
    int rc = iic_ota_f28379(F28379_CPU1, "blinky.txt", "/dev/ttyUSB0", 9600);
    if (rc != IIC_OTA_SUCCESS)
    {
        fprintf(stderr, "update failed, code %d\n", rc);
        return 1;
    }
    printf("update succeeded\n");
    return 0;
}
```

`iic_ota_f28379()` is reentrant/thread-safe (no global state) and can be called
concurrently against different serial ports from multiple threads.

For the F280049 device-picks-the-bank case:

```c
#include "iic_ota.h"
#include <stdio.h>

int main(void)
{
    int rc = iic_ota_f280049("bank0.txt", "bank1.txt", "/dev/ttyUSB0", 9600);
    if (rc != IIC_OTA_SUCCESS)
    {
        fprintf(stderr, "update failed, code %d\n", rc);
        return 1;
    }
    printf("update succeeded\n");
    return 0;
}
```

Both `bank0.txt` and `bank1.txt` paths are required even though only one is actually
transferred — the device tells `iic_ota_f280049()` which one after the command ACK.
`iic_ota_f280049()` has the same reentrancy/thread-safety guarantees as
`iic_ota_f28379()`.

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
from iic_ota import iic_ota_f28379, F28379_CPU1, IicOtaError

try:
    iic_ota_f28379(F28379_CPU1, "blinky.txt", "/dev/ttyUSB0", 9600)
    print("update succeeded")
except IicOtaError as e:
    print("update failed:", e, "(code", e.code, ")")
```

`iic_ota_f28379()` auto-locates `libiic_ota.so` next to `python/iic_ota.py` or in
`../build/`. If it's somewhere else, pass it explicitly:

```python
iic_ota_f28379(F28379_CPU1, "blinky.txt", "/dev/ttyUSB0", 9600,
                library_path="/path/to/libiic_ota.so")
```

For the F280049 device-picks-the-bank case, use `iic_ota_f280049()` instead:

```python
from iic_ota import iic_ota_f280049, IicOtaError

try:
    iic_ota_f280049("bank0.txt", "bank1.txt", "/dev/ttyUSB0", 9600)
    print("update succeeded")
except IicOtaError as e:
    print("update failed:", e, "(code", e.code, ")")
```

### As a CLI

```bash
python3 serial_flash_programmer/python/iic_ota.py \
    --target f28379_cpu1 --file blinky.txt --port /dev/ttyUSB0 --baud 9600 \
    [--lib /path/to/libiic_ota.so]
```

`--target`/`-t` (case-insensitive) is one of `f280049`, `f28379_cpu1`, `f28379_cpu2`.
`f28379_cpu1`/`f28379_cpu2` call `iic_ota_f28379()` and require `--file`. `f280049`
calls `iic_ota_f280049()` (the device picks the bank) and requires `--bank0-file` and
`--bank1-file` instead of `--file`:

```bash
python3 serial_flash_programmer/python/iic_ota.py \
    -t f280049 --bank0-file bank0.txt --bank1-file bank1.txt \
    --port /dev/ttyUSB0 --baud 9600 [--lib /path/to/libiic_ota.so]
```

## Return / error codes

Defined in `include/iic_ota.h` (and mirrored in `python/iic_ota.py`):

| Code | Name                              | Meaning | Upper Layer Action Recommended |
|------|-----------------------------------|---------|---------------------------------| 
| `0`   | `IIC_OTA_SUCCESS`                 | image sent and ACKed successfully | retrieve firmware version to confirm,and then reinitialize communication session to the IIC |
| `-1`  | `IIC_OTA_ERR_INVALID_ARG`         | bad target, or a NULL/empty file or port path | check arguments. Nothing has changed on the DSP side. |
| `-2`  | `IIC_OTA_ERR_UNSUPPORTED_BAUD`    | `baudrate` isn't one of the supported values | check arguments. Nothing has changed on the DSP side. |
| `-3`  | `IIC_OTA_ERR_FILE_OPEN`           | could not open `firmware_file` for reading |check file path or permission. Nothing has changed on the DSP side. |
| `-4`  | `IIC_OTA_ERR_PORT_OPEN`           | could not open `serial_port` | check port or permission. Nothing has changed on the DSP side. |
| `-5`  | `IIC_OTA_ERR_PORT_CONFIG`         | `tcsetattr`/`tcgetattr` on the serial port failed | fix configuration. Nothing has changed on the DSP side. |
| `-6`  | `IIC_OTA_ERR_AUTOBAUD_TIMEOUT`    | no, or a garbled, echo to the autobaud byte | retry by calling ota_iic_f280049/f28379(). Nothing has changed on the DSP side yet. |
| `-7`  | `IIC_OTA_ERR_COMMAND_NAK`         | device NAKed the Live DFU command packet | retriger dsp entering live update mode by sending 0xC2 command, then re-call ota_iic_f280049/f28379()|
| `-8`  | `IIC_OTA_ERR_COMMAND_TIMEOUT`     | no ACK/NAK reply to the command within the timeout | wait for 10 seconds, then re-call ota_iic_f280049/f28379() |
| `-9`  | `IIC_OTA_ERR_IMAGE_FORMAT`        | `firmware_file` isn't valid SCI-8 boot format | replace firmware_file. wait for at least 7 seconds->retriger LFU mode->re-call ota_iic_f280049/f28379() |
| `-10` | `IIC_OTA_ERR_IMAGE_TRANSFER`      | checksum mismatch or timeout during image transfer | wait for at least 7 seconds->retriger LFU mode->re-call ota_iic_f280049/f28379() |
| `-11` | `IIC_OTA_ERR_BANK_SELECT`         | device's bank-select readback bytes were mismatched or unrecognized (`iic_ota_f280049()` only) | wait for at least 7 seconds->retriger LFU mode->re-call ota_iic_f280049/f28379() |

In C, check the return value directly. In Python, a non-success return raises
`IicOtaError`, whose `.code` attribute holds the value above.

## Upper Layer Design Consideration

1. The live update process must start by calling `iic_ota_f28xxx()` within 10 seconds after switching to firmware update mode. Otherwise, the DSP will return to normal mode. A `0xC2` command is required to re-trigger the DSP to enter live update mode.

2. Refer to 'Return/Error codes" section for handling errors.




