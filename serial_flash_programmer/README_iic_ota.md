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
- Supported baud rates: fixed at 115200 as DSP operating at this speed. This library supports 300, 600, 1200, 1800, 2400, 4800, 9600, 19200, 38400, 57600,
  115200 for future extension. Anything else is rejected before any I/O happens.
- `firmware_file` (and `bank0_firmware_file`/`bank1_firmware_file`) must be ASCII
  SCI-8 boot format, i.e. the output of `hex2000 -boot -a -sci8 app.out -o app.txt`.


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

### Shared library (required for the Python wrapper) for Ubuntu

```bash
cmake -DIIC_OTA_SHARED=ON ..
make iic_ota
```

Produces `build/libiic_ota.so` instead.

### Skipping the library entirely for Ubuntu

```bash
cmake -DBUILD_IIC_OTA_LIB=OFF ..
```

No `iic_ota`/`libiic_ota.*` target is built at all; `serial_flash_programmer` is
unaffected.

### Cross-compiling for TI AM62x (aarch64) with `Makefile.am62x`

`Makefile.am62x` builds only the `iic_ota` library for aarch64 targets such as the TI AM62x. It is
plain Make and does not use CMake.

```bash
cd serial_flash_programmer
make -f Makefile.am62x            # builds both libraries
make -f Makefile.am62x static     # only build-am62x/libiic_ota.a
make -f Makefile.am62x shared     # only build-am62x/libiic_ota.so
make -f Makefile.am62x clean
```

Requires `aarch64-linux-gnu-g++` and `aarch64-linux-gnu-ar` on `$PATH` (Ubuntu:
`apt install gcc-aarch64-linux-gnu g++-aarch64-linux-gnu`). Output goes to `build-am62x/`. Check it
with `file build-am62x/libiic_ota.so` (expect `ELF 64-bit ... ARM aarch64`).

To use a different toolchain, override the variables on the command line (or in the environment):

```bash
make -f Makefile.am62x CXX=aarch64-none-linux-gnu-g++ AR=aarch64-none-linux-gnu-ar
```

`CXXFLAGS` (default `-std=c++11 -O2 -Wall -Wextra -fPIC`) and `BUILD_DIR` can be overridden the
same way.

**glibc compatibility:** a library built with Ubuntu's cross toolchain links against Ubuntu's glibc.
Current Ubuntu releases pull in `__isoc23_fscanf@GLIBC_2.38` (from the `fscanf` in `source/iic_ota.cpp`),
so the `.so` will not load on a board with an older glibc (for example a kirkstone-based image, glibc
2.35). Check the requirement with
`aarch64-linux-gnu-objdump -T build-am62x/libiic_ota.so | grep GLIBC_` and compare it with
`ldd --version` on the board. If the board is older, build with the Yocto recipe below instead, which
compiles against the image's own libraries.

### Building with Yocto (`yocto/iic-ota_git.bb`)

`yocto/iic-ota_git.bb` is a BitBake recipe that builds `iic_ota` with the Yocto cross toolchain by
running `Makefile.am62x`. It installs `libiic_ota.so` and `libiic_ota.a` into `${libdir}` and
`iic_ota.h` into `${includedir}`. The `.so` goes in the main package (not `-dev`) so it can be loaded
by name, for example by the Python `ctypes` wrapper.

1. Copy the recipe into a layer of your own, for example
   `meta-yourlayer/recipes-support/iic-ota/iic-ota_git.bb`.
2. The recipe fetches this repository from GitHub, so `serial_flash_programmer/Makefile.am62x` must be
   pushed to the `master` branch first. For reproducible builds, replace `SRCREV = "${AUTOREV}"` with
   a specific commit hash.
3. Build it:
   ```bash
   bitbake iic-ota
   ```
4. Add it to your image, for example in `conf/local.conf`:
   ```
   IMAGE_INSTALL:append = " iic-ota"
   ```
   Add `iic-ota-dev` and `iic-ota-staticdev` too if you want the header and static library in the
   image or SDK.

To test the recipe against a local checkout instead of GitHub, use `devtool modify iic-ota` or
override `SRC_URI` with a `file://` or `git://` URL of your own.

The recipe has not been run through BitBake yet. If `bitbake iic-ota` fails, the error text will show
what to adjust (the likeliest spots are the `LIC_FILES_CHKSUM` line and the `SRCPV` variable, which
newer Yocto releases deprecate).

## C usage

```c
#include "iic_ota.h"   // serial_flash_programmer/include/iic_ota.h
#include <stdio.h>

int main(void)
{
    int rc = iic_ota_f28379(F28379_CPU1, "blinky.txt", "/dev/ttyUSB0", 115200);
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
    int rc = iic_ota_f280049("bank0.txt", "bank1.txt", "/dev/ttyUSB0", 115200);
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
    iic_ota_f28379(F28379_CPU1, "blinky.txt", "/dev/ttyUSB0", 115200)
    print("update succeeded")
except IicOtaError as e:
    print("update failed:", e, "(code", e.code, ")")
```

`iic_ota_f28379()` auto-locates `libiic_ota.so` next to `python/iic_ota.py` or in
`../build/`. If it's somewhere else, pass it explicitly:

```python
iic_ota_f28379(F28379_CPU1, "blinky.txt", "/dev/ttyUSB0", 115200,
                library_path="/path/to/libiic_ota.so")
```

For the F280049 device-picks-the-bank case, use `iic_ota_f280049()` instead:

```python
from iic_ota import iic_ota_f280049, IicOtaError

try:
    iic_ota_f280049("bank0.txt", "bank1.txt", "/dev/ttyUSB0", 115200)
    print("update succeeded")
except IicOtaError as e:
    print("update failed:", e, "(code", e.code, ")")
```

### As a CLI

```bash
python3 serial_flash_programmer/python/iic_ota.py \
    --target f28379_cpu1 --file blinky.txt --port /dev/ttyUSB0 --baud 115200 \
    [--lib /path/to/libiic_ota.so]
```

`--target`/`-t` (case-insensitive) is one of `f280049`, `f28379_cpu1`, `f28379_cpu2`.
`f28379_cpu1`/`f28379_cpu2` call `iic_ota_f28379()` and require `--file`. `f280049`
calls `iic_ota_f280049()` (the device picks the bank) and requires `--bank0-file` and
`--bank1-file` instead of `--file`:

```bash
python3 serial_flash_programmer/python/iic_ota.py \
    -t f280049 --bank0-file bank0.txt --bank1-file bank1.txt \
    --port /dev/ttyUSB0 --baud 115200 [--lib /path/to/libiic_ota.so]
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




