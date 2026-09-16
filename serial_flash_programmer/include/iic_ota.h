//###########################################################################
// FILE:   iic_ota.h
// TITLE:  Reentrant C API for Live DFU firmware updates over SCI.
//
// This is a standalone, dependency-free reimplementation of the SCI/F021
// "Live DFU" wire protocol used interactively by serial_flash_programmer's
// "8-Live DFU" menu item. It shares no code and no global state with the
// CLI (serial_flash_programmer.cpp / source/f021_*.cpp) and does not alter
// its behavior in any way.
//
// iic_ota() assumes the target's SCI flash kernel is ALREADY RUNNING and
// autobaud-ready on the device -- it does not download a kernel, and for
// F28379_CPU2 it does not perform any CPU1->CPU2 SCI hand-off. The caller
// is responsible for getting the target CPU's kernel running and owning
// the SCI/UART before calling this function, exactly as the existing CLI
// assumes for its F280049 Live DFU flow.
//###########################################################################

#ifndef IIC_OTA_H
#define IIC_OTA_H

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

	//*************************************************************************
	//
	// Selects which flash target on the device the image is written to.
	// These are the public API selector values only -- they are distinct
	// from (and never sent as) the SCI wire-protocol command codes; the
	// mapping from firmware_type_t to a wire command is internal to
	// iic_ota.cpp.
	//
	//*************************************************************************
	typedef enum
	{
		F280049_BANK0 = 0xaa,
		F280049_BANK1 = 0xbb,
		F28379_CPU1 = 0xcc,
		F28379_CPU2 = 0xdd
	} firmware_type_t;

	//*************************************************************************
	//
	// iic_ota() return codes. IIC_OTA_SUCCESS is 0; all failures are
	// negative. iic_ota() never blocks indefinitely -- every internal wait
	// is bounded by a timeout, after which the corresponding *_TIMEOUT code
	// is returned instead of hanging the caller.
	//
	//*************************************************************************
#define IIC_OTA_SUCCESS 0			 // image sent and ACKed successfully
#define IIC_OTA_ERR_INVALID_ARG -1	 // bad firmware_type_t, or a NULL/empty path
#define IIC_OTA_ERR_UNSUPPORTED_BAUD -2 // baudrate not in the supported set
#define IIC_OTA_ERR_FILE_OPEN -3	 // could not open firmware_file for reading
#define IIC_OTA_ERR_PORT_OPEN -4	 // could not open serial_port
#define IIC_OTA_ERR_PORT_CONFIG -5	 // tcgetattr/tcsetattr on serial_port failed
#define IIC_OTA_ERR_AUTOBAUD_TIMEOUT -6 // no/garbled echo to the autobaud byte
#define IIC_OTA_ERR_COMMAND_NAK -7	 // device NAKed the Live DFU command packet
#define IIC_OTA_ERR_COMMAND_TIMEOUT -8	 // no ACK/NAK reply within the timeout
#define IIC_OTA_ERR_IMAGE_FORMAT -9	 // firmware_file is not valid SCI-8 boot format
#define IIC_OTA_ERR_IMAGE_TRANSFER -10	 // checksum mismatch or timeout during image transfer

	//*************************************************************************
	//
	// Performs a Live DFU firmware update over a serial UART.
	//
	// Sequence: open and configure the serial port -> autobaud lock -> send
	// the target-specific Live DFU command packet -> stream firmware_file
	// using the SCI-8 ASCII boot-format checksum protocol -> close the port.
	//
	// Params:
	//   type          - which flash target to update (see firmware_type_t).
	//   firmware_file - path to an ASCII SCI-8 boot-format file, i.e. the
	//                   output of `hex2000 -boot -a -sci8 app.out -o app.txt`.
	//   serial_port   - POSIX device path, e.g. "/dev/ttyUSB0".
	//   baudrate      - one of 300, 600, 1200, 1800, 2400, 4800, 9600, 19200,
	//                   38400, 57600, 115200. Any other value is rejected.
	//
	// Returns IIC_OTA_SUCCESS (0) on success, or a negative IIC_OTA_ERR_*
	// code otherwise. Reentrant and thread-safe: uses no global or static
	// mutable state, so it may be called concurrently from multiple threads
	// against different serial ports.
	//
	//*************************************************************************
	int iic_ota(firmware_type_t type, const char *firmware_file,
				const char *serial_port, int baudrate);

#ifdef __cplusplus
}
#endif

#endif // IIC_OTA_H
