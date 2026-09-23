# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

Host-side (PC) command-line tool that flashes TI C2000 DSPs over a serial port. It is a fork of TI's
`serial_flash_programmer` example (from C2000Ware_1_00_06_00) with Linux/WSL/Raspberry Pi support added,
plus local modifications for an "Eversol-IIC" board. There is no test suite and no library — it is one
executable driven interactively from a terminal, and exercising it for real requires a C2000 device in
SCI boot mode on the other end of a UART.

## Build

The CMake project lives in `serial_flash_programmer/`, not at the repo root (the root only holds the
Visual Studio `.sln` and example firmware images).

```bash
cd serial_flash_programmer/build   # directory is gitignored; mkdir -p it if absent
cmake ..
make -j8
```

Produces `serial_flash_programmer/build/serial_flash_programmer`. The same `CMakeLists.txt` generates a
Visual Studio 2017 build on Windows (`serial_flash_programmer.sln` / MSBuild `/property:Configuration=Release`);
`.vscode/tasks.json` wires both up as `cmake` and `make` tasks.

### AM62x (aarch64) cross-compile of iic_ota

`iic_ota` (the standalone Live DFU library, see `README_iic_ota.md`) can be cross-compiled for TI
AM62x targets independently of the main CMake build, using a plain Makefile:

```bash
cd serial_flash_programmer
make -f Makefile.am62x
```

Requires `aarch64-linux-gnu-gcc`/`aarch64-linux-gnu-g++` on `$PATH` (Ubuntu:
`apt install gcc-aarch64-linux-gnu g++-aarch64-linux-gnu`). Produces `build-am62x/libiic_ota.a` and
`build-am62x/libiic_ota.so`. This does **not** cross-compile the `serial_flash_programmer` CLI itself
-- only `iic_ota` -- and is entirely separate from the CMake `build/` tree (different directory,
different tool, not wired into `CMakeLists.txt`). Override `CXX`/`AR`/`CXXFLAGS` on the command line
to point at a different aarch64 toolchain. Confirm the result with `file build-am62x/libiic_ota.so`
(expect `ELF ... ARM aarch64`).

## Running it

```bash
# Linux/RPi
./serial_flash_programmer -d f2806x -k f28069_sci_flash_kernel.txt -a Example_2806xLaunchPad.txt -b 38400 -p /dev/ttyUSB0
# Windows
serial_flash_programmer.exe -d f2837xD -k F2837xD_sci_flash_kernels_cpu01.txt -a blinky_cpu01.txt -b 9600 -p COM7
```

`-p` takes a device path on Linux (`/dev/ttyUSB0`, `/dev/ttyS8`) and `COM<n>` on Windows. On WSL the port
usually needs `sudo chmod 666 /dev/ttyS8` first. Baud defaults to 9600 and only the values enumerated in
`get_serial_speed()` are accepted. Input files must be ASCII SCI-8 boot format produced by the C2000
codegen tool: `hex2000 -boot -a -sci8 app.out -o app.txt`. Example images live in
`f28004x_fw_upgrade_example/`, `launchxl-f28069m_upgrade_example/`, and are described by the root
`README.txt` / `f2837xD_fw_upgrade_example.txt`.

## Architecture

Everything is driven by globals set in `ParseCommandLine()` → `setDeviceName()` (`serial_flash_programmer.cpp`).
`setDeviceName()` is the fork point: it sets the device flag (`g_bf2837xD`, `g_bf28004x`, …), picks the
kernel protocol (`g_bf021` vs `g_bf05`), and sets `gu32_SectorMask` to the sector count for that part.
`_tmain()` then branches three ways:

- **`g_bf021` + f2837xD** — dual-core menu (DFU/Erase/Verify/Unlock/Run/Reset per CPU). After
  "Run/Reset CPU1 Boot CPU2", CPU1 hands the SCI over to CPU2, so the `cpu1`/`cpu2` locals gate which
  menu items are legal from then on.
- **`g_bf021` + f2837xS/f2807x/f28004x** — single-core menu (adds Live DFU).
- **`g_bf05`** (f2802x/3x/5x/6x) — no menu at all; `f05_DownloadImage()` does kernel + app in one shot.

Two wire protocols, one per kernel generation:

- **F021 ("kernel B")** — `source/f021_*.cpp`. `autobaudLock()` sends `'A'` and requires the echo.
  `constructPacket()` builds `| 0x1BE4 | length | command | data | checksum | 0xE41B |` (all LSB-first);
  `f021_SendPacket()` writes it a byte at a time and waits for ACK `0x2D` / NAK `0xA5`. The device replies
  with a status packet parsed by `getPacket()`, whose `data[0]` is `NO_ERROR`/`BLANK_ERROR`/… and
  `data[2]:data[1]` is the address (join them with `formatMemAddr()`, never print them separately).
  Image transfer is `loadProgram_checksum()` in `f021_DownloadImage.cpp`: block-at-a-time with a device
  checksum handshake every `g_bBlockSize` (0x80) words. The plain byte-echo `loadProgram()` in
  `f021_DownloadKernel.cpp` is the fallback, selected by `#if checksum_enable`.
- **F05 ("kernel A")** — `source/f05_DownloadImage.cpp`. Byte-echo for the kernel, then chunked
  checksum for the application. Self-contained; shares no code with the F021 path.

Erase sectors are entered as letters and mapped in `setEraseSector()` — `A`–`P` for most parts, and for
f28004x `A`–`P` is bank 0 / `a`–`p` is bank 1, giving 32 bits in `gu32_EraseSectors1/2`.

Every transport call is `#ifdef __linux__` (POSIX `open`/`read`/`write` on the global `fd`, termios
config in `_tmain`) versus Windows (`CreateFile`/`ReadFile`/`WriteFile` on the global `file`, DCB config).
Keep both arms in sync when touching I/O — the Windows side is not built or tested here.

## Gotchas specific to this fork

- **`#define kernel` is commented out** in `serial_flash_programmer.cpp` ("undefined kernel for Eversol-IIC").
  With it off, the tool skips `f021_DownloadKernel()` entirely and autobauds straight to a kernel that is
  assumed to be already running on the device. Re-enable it to restore stock TI behavior of loading the
  kernel from `-k` first. `-k` is still required by `checkErrors()` regardless.
- **The Linux build compiles with `-DUNICODE`** (the CMake compile definitions are not guarded by platform).
  So in `linux_macros.h` the UNICODE branch is live: `_T("x")` expands to a *wide* literal `L"x"` and
  `_tprintf` is `wprintf`, while `wchar_t` is `#define`d to `char` so `TCHAR`, `argv`, and all the
  `g_psz*` file/port strings are narrow. This works because glibc's `wprintf` `%s` consumes a `char*` —
  do not "fix" it to `%ls`, and use `_tprintf(_T(...))` rather than bare `printf` in new code so both
  platforms stay consistent.
- Error paths in the protocol code frequently `while(1) {}` (busy-wait forever) instead of returning —
  the user is expected to Ctrl-C. `checkErrors()` likewise prints and continues rather than exiting.
- `-DWIN32` is also defined on Linux; it is not a reliable platform test. Use `__linux__`.
