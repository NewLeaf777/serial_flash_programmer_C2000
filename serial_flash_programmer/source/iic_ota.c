//###########################################################################
// FILE:   iic_ota.c
// TITLE:  Reentrant C API for Live DFU firmware updates over SCI.
//
// This reimplements, standalone, the wire protocol used by
// serial_flash_programmer.cpp's interactive "8-Live DFU" menu item
// (constructPacket()/f021_SendPacket() in source/f021_SendMessage.cpp,
// autobaudLock() in source/f021_DownloadKernel.cpp, and
// loadProgram_checksum() in source/f021_DownloadImage.cpp). It shares no
// code and no global state with those files or with the CLI, and does not
// alter the CLI's behavior in any way -- this file is not built into the
// serial_flash_programmer executable.
//
// Unlike the CLI's protocol functions, every wait here is bounded by a
// timeout (no `while(1){}` spins), and there is no process-wide mutable
// state (no global fd/checksum/verbosity flags) -- the whole implementation
// is reentrant and safe to call concurrently against different ports.
//
// Linux-only: this targets the POSIX termios serial API, which is the only
// platform this repository actually builds and tests against.
//###########################################################################

#include "../include/iic_ota.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <fcntl.h>
#include <termios.h>
#include <unistd.h>

//*************************************************************************
// Constants
//*************************************************************************
static const uint8_t kAck = 0x2D;

// F280049 Live DFU bank-select readback (see read_bank_select() below).
static const uint8_t kBankSelect0 = 0xB0;
static const uint8_t kBankSelect1 = 0xB1;

// PROPOSED / UNVERIFIED: the existing LIVE_DFU_CPU1 command already used
// by serial_flash_programmer.cpp's case-8 Live DFU for F280049-class
// (single-core) devices -- reused here unchanged to preserve today's
// working behavior. The device, not this command value, now decides
// which bank gets written (see read_bank_select() below).
static const uint16_t kF280049LiveDfuCommand = 0x0700;

// Matches g_bBlockSize in source/f021_DownloadImage.cpp -- number of
// 16-bit words transmitted between device-checksum handshakes.
static const unsigned int kBlockSizeWords = 0x80;

// F28379 Live DFU traffic does not reach the F28379 directly: the F280049
// forwards it over SPI with a 16-bit data width, so a lone byte from the
// host never completes an SPI word and stalls in the forwarder. Everything
// iic_ota_f28379() sends is therefore written in byte pairs (see
// write_pair()). Once the transfer is over, this 8-byte (4 SPI word)
// sequence tells the F280049 to leave traffic-forward mode and resume
// normal protocol processing. It deliberately avoids every byte value the
// protocol already assigns a meaning to (0x2D ACK, 0xA5 NAK, 0xE4/0x1B
// packet framing, 0xAA/0x08 SCI-8 key, 0xB0/0xB1 bank select, 'A'
// autobaud). The F280049 firmware must match these exact bytes, in this
// order, and must not forward them to the F28379.
static const uint8_t kForwardExitMagic[8] = {0xDE, 0xAD, 0xBE, 0xEF, 0xFE, 0xED, 0xFA, 0xCE};

// Host-side patience. These are new values -- not part of the wire
// protocol -- chosen to be generous for low-baud SCI transfers while
// still guaranteeing iic_ota_f28379()/iic_ota_f280049() eventually
// return.
static const int kAutobaudTimeoutMs = 3000;
static const int kCommandAckTimeoutMs = 3000;
static const int kByteTimeoutMs = 5000;

//*************************************************************************
// Target -> wire command mapping.
//
// PROPOSED / UNVERIFIED: these follow this codebase's existing command
// numbering conventions (DFU_CPU1/DFU_CPU2 = +0x0100 per role) and MUST
// be confirmed against (or replaced with) whatever the actual device
// kernel firmware expects before use against real hardware.
//*************************************************************************
static bool wire_command_for(f28379_target_t target, uint16_t *command)
{
	switch (target)
	{
	case F28379_CPU1:
		*command = 0x0710;
		return true;
	case F28379_CPU2:
		*command = 0x0810;
		return true;
	default:
		return false;
	}
}

//*************************************************************************
// Supported baud rates -- the same 11 values get_serial_speed() in
// serial_flash_programmer.cpp accepts, validated explicitly here up
// front instead of silently passing an unchecked -1 to cfsetospeed().
//*************************************************************************
typedef struct
{
	int baud;
	speed_t speed;
} BaudEntry;

static const BaudEntry kBaudTable[] = {
	{300, B300},
	{600, B600},
	{1200, B1200},
	{1800, B1800},
	{2400, B2400},
	{4800, B4800},
	{9600, B9600},
	{19200, B19200},
	{38400, B38400},
	{57600, B57600},
	{115200, B115200},
};

static bool lookup_baud(int baudrate, speed_t *speed)
{
	for (size_t i = 0; i < sizeof(kBaudTable) / sizeof(kBaudTable[0]); i++)
	{
		if (kBaudTable[i].baud == baudrate)
		{
			*speed = kBaudTable[i].speed;
			return true;
		}
	}
	return false;
}

//*************************************************************************
// Time helper for wall-clock-bounded retry loops.
//*************************************************************************
static long long monotonic_ms()
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ((long long)(ts.tv_sec)) * 1000 + ts.tv_nsec / 1000000;
}

//*************************************************************************
// Bounded I/O primitives. These replace the CLI's
// "while (dwRead == 0) { read(...); }" (poll-forever) and
// "while (1) {}" (spin-forever on mismatch) patterns with a real
// timeout: each read() is already capped at ~0.5s by VTIME=5, so we
// just keep retrying until the accumulated wall-clock budget expires.
//*************************************************************************
static int read_byte_timeout(int fd, uint8_t *out, int timeout_ms)
{
	long long deadline = monotonic_ms() + timeout_ms;
	for (;;)
	{
		unsigned char b;
		ssize_t n = read(fd, &b, 1);
		if (n == 1)
		{
			*out = b;
			return 0;
		}
		if (n < 0 && errno != EAGAIN && errno != EINTR)
		{
			return -1;
		}
		if (monotonic_ms() >= deadline)
		{
			return -1;
		}
	}
}

static int write_all(int fd, const uint8_t *buf, size_t len)
{
	size_t written = 0;
	while (written < len)
	{
		ssize_t n = write(fd, buf + written, len - written);
		if (n < 0)
		{
			if (errno == EINTR)
			{
				continue;
			}
			return -1;
		}
		written += ((size_t)(n));
	}
	return 0;
}

static int write_byte(int fd, uint8_t b)
{
	return write_all(fd, &b, 1);
}

// Writes two bytes in a single write() -- one 16-bit SPI word once the
// F280049 forwards it to the F28379.
static int write_pair(int fd, uint8_t b0, uint8_t b1)
{
	uint8_t buf[2] = {b0, b1};
	return write_all(fd, buf, 2);
}

static void flush_port(int fd)
{
	tcflush(fd, TCIOFLUSH);
}

//*************************************************************************
// Serial port configuration -- same termios settings as the CLI's
// serial_flash_programmer.cpp:283-311 (CS8|CLOCAL|CREAD, IGNPAR, raw
// output, raw input, VTIME=5/VMIN=0).
//*************************************************************************
static int configure_port(int fd, speed_t speed)
{
	struct termios newtio;
	memset(&newtio, 0, sizeof(newtio));
	newtio.c_cflag = CS8 | CLOCAL | CREAD;
	newtio.c_iflag = IGNPAR;
	newtio.c_oflag = 0;
	cfsetospeed(&newtio, speed);
	cfsetispeed(&newtio, speed);
	newtio.c_lflag = 0;
	newtio.c_cc[VTIME] = 5;
	newtio.c_cc[VMIN] = 0;

	if (tcflush(fd, TCIFLUSH) != 0)
	{
		return -1;
	}
	if (tcsetattr(fd, TCSANOW, &newtio) != 0)
	{
		return -1;
	}
	return 0;
}

//*************************************************************************
// Reentrant reimplementation of constructPacket()
// (source/f021_SendMessage.cpp:120-146). Byte-identical output for the
// same inputs. All iic_ota_f28379()/iic_ota_f280049() commands use
// length=0/data=NULL, exactly as case 8 (Live DFU) does today.
//*************************************************************************
static uint32_t construct_packet(uint8_t *packet, uint16_t command, uint16_t length, const uint8_t *data)
{
	uint16_t checksum = 0;
	packet[0] = 0xE4;
	packet[1] = 0x1B;
	packet[2] = ((uint8_t)(length & 0xFF));
	packet[3] = ((uint8_t)((length & 0xFF00) >> 8));
	packet[4] = ((uint8_t)(command & 0xFF));
	checksum += (command & 0xFF);
	packet[5] = ((uint8_t)((command & 0xFF00) >> 8));
	checksum += ((command & 0xFF00) >> 8);
	uint32_t index = 6;
	for (int i = 0; i < length; i++)
	{
		checksum += data[i];
		packet[index++] = data[i];
		i++;
		checksum += data[i];
		packet[index++] = data[i];
	}
	packet[index++] = ((uint8_t)(checksum & 0xFF));
	packet[index++] = ((uint8_t)((checksum & 0xFF00) >> 8));
	packet[index++] = 0x1B;
	packet[index++] = 0xE4;
	return index;
}

//*************************************************************************
// Reentrant reimplementation of autobaudLock()
// (source/f021_DownloadKernel.cpp:130-173): write 'A', require the same
// byte echoed back. Returns 0 on success, -1 on timeout/mismatch instead
// of busy-hanging forever.
//*************************************************************************
static int autobaud_lock(int fd, int timeout_ms)
{
	flush_port(fd);
	if (write_byte(fd, 'A') != 0)
	{
		return -1;
	}
	uint8_t echoByte;
	if (read_byte_timeout(fd, &echoByte, timeout_ms) != 0)
	{
		return -1;
	}
	return (echoByte == 'A') ? 0 : -1;
}

//*************************************************************************
// Paired variant of autobaud_lock() for the F28379 behind the F280049 SPI
// forwarder: write 'A','A' as one pair and require both echoed back.
//*************************************************************************
static int autobaud_lock_paired(int fd, int timeout_ms)
{
	flush_port(fd);
	if (write_pair(fd, 'A', 'A') != 0)
	{
		return -1;
	}
	for (int i = 0; i < 2; i++)
	{
		uint8_t echoByte;
		if (read_byte_timeout(fd, &echoByte, timeout_ms) != 0)
		{
			return -1;
		}
		if (echoByte != 'A')
		{
			return -1;
		}
	}
	return 0;
}

//*************************************************************************
// Sends kForwardExitMagic so the F280049 leaves traffic-forward mode, then
// waits for it to actually leave the UART -- flush_port()'s TCIOFLUSH
// would otherwise discard any of it still sitting in the output queue.
//*************************************************************************
static int send_forward_exit(int fd)
{
	if (write_all(fd, kForwardExitMagic, sizeof(kForwardExitMagic)) != 0)
	{
		return -1;
	}
	return (tcdrain(fd) == 0) ? 0 : -1;
}

//*************************************************************************
// Reentrant reimplementation of f021_SendPacket()
// (source/f021_SendMessage.cpp:158-209): write the packet, wait for a
// single ACK/NAK reply byte, flush on ACK.
//
// Returns 0 if a reply byte was received (check *nak for ACK vs NAK),
// -1 on a write failure or on timeout waiting for the reply.
//*************************************************************************
static int send_packet_and_wait_ack(int fd, const uint8_t *packet, uint32_t length, int timeout_ms, bool *nak)
{
	*nak = false;
	if (write_all(fd, packet, length) != 0)
	{
		return -1;
	}
	uint8_t reply;
	if (read_byte_timeout(fd, &reply, timeout_ms) != 0)
	{
		return -1;
	}
	if (reply == kAck)
	{
		// flush_port(fd);
		return 0;
	}
	*nak = true;
	return 0;
}

//*************************************************************************
// Reads the 2-byte bank-select readback the device sends immediately
// after ACKing the F280049 Live DFU command: 0xB0 0xB0 selects bank 0,
// 0xB1 0xB1 selects bank 1.
//
// Returns 0 if both bytes were received (check *invalid for whether they
// form a recognized bank-select pattern), -1 on timeout waiting for
// either byte.
//*************************************************************************
static int read_bank_select(int fd, int timeout_ms, uint8_t *bank, bool *invalid)
{
	*invalid = false;
	uint8_t b0, b1;
	if (read_byte_timeout(fd, &b0, timeout_ms) != 0)
	{
		return -1;
	}
	if (read_byte_timeout(fd, &b1, timeout_ms) != 0)
	{
		return -1;
	}
	if (b0 != b1 || (b0 != kBankSelect0 && b0 != kBankSelect1))
	{
		*invalid = true;
		return 0;
	}
	*bank = b0;
	return 0;
}

//*************************************************************************
// Reads one ASCII hex byte pair from the SCI-8 boot-format file, e.g.
// "3F" -> 0x3F. Equivalent to the CLI's fscanf_s(fh, "%hhx", ...).
//*************************************************************************
static int read_hex_byte(FILE *fh, uint8_t *out)
{
	return (fscanf(fh, "%hhx", out) == 1) ? 0 : -1;
}

//*************************************************************************
// Receives a device-side 2-byte checksum (LSB then MSB), ACKing each
// byte as it arrives -- matches the checksum-receive halves of
// loadProgram_checksum() (source/f021_DownloadImage.cpp).
//*************************************************************************
static int recv_ack_checksum_word(int fd, uint16_t *out, int timeout_ms)
{
	uint8_t lsb, msb;
	if (read_byte_timeout(fd, &lsb, timeout_ms) != 0)
	{
		return -1;
	}
	if (write_byte(fd, kAck) != 0)
	{
		return -1;
	}
	if (read_byte_timeout(fd, &msb, timeout_ms) != 0)
	{
		return -1;
	}
	if (write_byte(fd, kAck) != 0)
	{
		return -1;
	}
	*out = ((uint16_t)(lsb | (msb << 8)));
	return 0;
}

//*************************************************************************
// Paired variant of recv_ack_checksum_word(): receive both checksum bytes
// first, then ACK them together as one pair. The F28379 kernel must send
// LSB and MSB back-to-back before waiting for the two ACKs.
//*************************************************************************
static int recv_checksum_word_paired(int fd, uint16_t *out, int timeout_ms)
{
	uint8_t lsb, msb;
	if (read_byte_timeout(fd, &lsb, timeout_ms) != 0)
	{
		return -1;
	}
	if (read_byte_timeout(fd, &msb, timeout_ms) != 0)
	{
		return -1;
	}
	if (write_pair(fd, kAck, kAck) != 0)
	{
		return -1;
	}
	*out = ((uint16_t)(lsb | (msb << 8)));
	return 0;
}

// Dispatches to the paired or byte-at-a-time checksum handshake.
static int recv_checksum_word(int fd, uint16_t *out, int timeout_ms, bool paired)
{
	return paired ? recv_checksum_word_paired(fd, out, timeout_ms)
				  : recv_ack_checksum_word(fd, out, timeout_ms);
}

//*************************************************************************
// Reentrant reimplementation of loadProgram_checksum()
// (source/f021_DownloadImage.cpp:115-431): streams an SCI-8 ASCII
// boot-format file to the device in blocks, with a running (never
// reset, compared mod 65536) checksum handshake every kBlockSizeWords
// words and at the end of every block, terminated by a 0x0000 block
// size. Every mismatch/format/timeout path returns
// IIC_OTA_ERR_IMAGE_TRANSFER or IIC_OTA_ERR_IMAGE_FORMAT instead of
// busy-hanging forever.
//
// With `paired` set (F28379 behind the F280049 SPI forwarder), every
// write goes out as a byte pair and checksum handshakes use
// recv_checksum_word_paired(). With it clear, the bytes on the wire are
// identical to the original byte-at-a-time loader.
//*************************************************************************
static int send_byte_pair(int fd, uint8_t b0, uint8_t b1, bool paired)
{
	if (paired)
	{
		return write_pair(fd, b0, b1);
	}
	if (write_byte(fd, b0) != 0)
	{
		return -1;
	}
	return write_byte(fd, b1);
}

static int download_image_checksum(int fd, FILE *fh, int byte_timeout_ms, bool paired)
{
	// Skip the single leading control byte (0x02) that precedes the
	// ASCII hex boot data in SCI-8 files, e.g.:
	//   02 0A 41 41 20 30 38 20 ...   ('\x02' '\n' "AA 08 " ...)
	// fscanf's whitespace skipping handles the newline for free.
	//
	// NOTE: this deliberately does NOT match
	// source/f021_DownloadImage.cpp's Linux path, which calls getc()
	// three times instead of once. Verified against a real sample file
	// (f28004x_fw_upgrade_example/led_ex1_blinky.txt) over a loopback
	// PTY: the CLI's extra two getc() calls eat into the first hex
	// token itself, so the device receives 0x0A instead of the correct
	// 0xAA "8-bit memory width" key value documented in README.txt as
	// the very first boot-format byte -- an apparent existing bug in
	// the CLI's Linux image loader, left untouched here per the
	// requirement not to modify existing CLI files. This function
	// intentionally reproduces the correct (Windows-path-equivalent)
	// single-byte skip instead.
	if (getc(fh) == EOF)
	{
		return IIC_OTA_ERR_IMAGE_FORMAT;
	}

	uint32_t checksum = 0; // running sum, compared mod 65536, never reset -- matches the original.

	// First 22 bytes are initialization data -- sent as 11 byte pairs.
	for (int i = 0; i < 22; i += 2)
	{
		uint8_t b0, b1;
		if (read_hex_byte(fh, &b0) != 0 || read_hex_byte(fh, &b1) != 0)
		{
			return IIC_OTA_ERR_IMAGE_FORMAT;
		}
		if (send_byte_pair(fd, b0, b1, paired) != 0)
		{
			return IIC_OTA_ERR_IMAGE_TRANSFER;
		}
		checksum += b0;
		checksum += b1;
	}

	uint16_t rcv;
	if (recv_checksum_word(fd, &rcv, byte_timeout_ms, paired) != 0)
	{
		return IIC_OTA_ERR_IMAGE_TRANSFER;
	}
	if ((checksum & 0xFFFF) != rcv)
	{
		return IIC_OTA_ERR_IMAGE_TRANSFER;
	}

	// Blocks: 2-byte size, then (if nonzero) a 4-byte destination
	// address and `size` 16-bit words. A 0x0000 size marks end of file.
	for (;;)
	{
		uint8_t sizeLsb, sizeMsb;
		if (read_hex_byte(fh, &sizeLsb) != 0 || read_hex_byte(fh, &sizeMsb) != 0)
		{
			return IIC_OTA_ERR_IMAGE_FORMAT;
		}
		unsigned int blockSize = ((unsigned int)(sizeLsb)) |
								  (((unsigned int)(sizeMsb)) << 8);

		if (send_byte_pair(fd, sizeLsb, sizeMsb, paired) != 0)
		{
			return IIC_OTA_ERR_IMAGE_TRANSFER;
		}
		checksum += sizeLsb;
		checksum += sizeMsb;

		if (blockSize == 0x0000)
		{
			break; // end of file
		}

		uint8_t addr[4];
		for (int i = 0; i < 4; i++)
		{
			if (read_hex_byte(fh, &addr[i]) != 0)
			{
				return IIC_OTA_ERR_IMAGE_FORMAT;
			}
		}
		for (int i = 0; i < 4; i += 2)
		{
			if (send_byte_pair(fd, addr[i], addr[i + 1], paired) != 0)
			{
				return IIC_OTA_ERR_IMAGE_TRANSFER;
			}
			checksum += addr[i];
			checksum += addr[i + 1];
		}

		for (unsigned int j = 0; j < blockSize; j++)
		{
			if ((j % kBlockSizeWords == 0) && (j > 0))
			{
				uint16_t blkChecksum;
				if (recv_checksum_word(fd, &blkChecksum, byte_timeout_ms, paired) != 0)
				{
					return IIC_OTA_ERR_IMAGE_TRANSFER;
				}
				if ((checksum & 0xFFFF) != blkChecksum)
				{
					return IIC_OTA_ERR_IMAGE_TRANSFER;
				}
			}

			uint8_t wLsb, wMsb;
			if (read_hex_byte(fh, &wLsb) != 0 || read_hex_byte(fh, &wMsb) != 0)
			{
				return IIC_OTA_ERR_IMAGE_FORMAT;
			}
			if (send_byte_pair(fd, wLsb, wMsb, paired) != 0)
			{
				return IIC_OTA_ERR_IMAGE_TRANSFER;
			}
			checksum += wLsb;
			checksum += wMsb;
		}

		// Unconditional end-of-block checksum handshake.
		uint16_t blkChecksum;
		if (recv_checksum_word(fd, &blkChecksum, byte_timeout_ms, paired) != 0)
		{
			return IIC_OTA_ERR_IMAGE_TRANSFER;
		}
		if ((checksum & 0xFFFF) != blkChecksum)
		{
			return IIC_OTA_ERR_IMAGE_TRANSFER;
		}
	}

	return IIC_OTA_SUCCESS;
}

int iic_ota_f28379(f28379_target_t target, const char *firmware_file, const char *serial_port, int baudrate)
{
	uint16_t command;
	if (!wire_command_for(target, &command))
	{
		return IIC_OTA_ERR_INVALID_ARG;
	}
	if (!firmware_file || !firmware_file[0] || !serial_port || !serial_port[0])
	{
		return IIC_OTA_ERR_INVALID_ARG;
	}

	speed_t speed;
	if (!lookup_baud(baudrate, &speed))
	{
		return IIC_OTA_ERR_UNSUPPORTED_BAUD;
	}

	FILE *fh = fopen(firmware_file, "rb");
	if (!fh)
	{
		return IIC_OTA_ERR_FILE_OPEN;
	}

	int fd = open(serial_port, O_RDWR | O_NOCTTY);
	if (fd < 0)
	{
		fclose(fh);
		return IIC_OTA_ERR_PORT_OPEN;
	}

	if (configure_port(fd, speed) != 0)
	{
		close(fd);
		fclose(fh);
		return IIC_OTA_ERR_PORT_CONFIG;
	}

	// Matches the Sleep(6) immediately preceding autobaudLock() in
	// serial_flash_programmer.cpp's _tmain(), for both the dual-core and
	// single-core device branches.
	flush_port(fd);
	usleep(6 * 1000);

	if (autobaud_lock_paired(fd, kAutobaudTimeoutMs) != 0)
	{
		close(fd);
		fclose(fh);
		return IIC_OTA_ERR_AUTOBAUD_TIMEOUT;
	}

	// Live DFU command packet -- always length=0/data=NULL, matching case 8.
	// 10 bytes, so it is already an even number of bytes for the SPI forwarder.
	uint8_t packet[16];
	uint32_t packetLength = construct_packet(packet, command, 0, NULL);

	// Matches the Sleep(500) preceding f021_SendPacket() in case 8.
	usleep(500 * 1000);

	int result;
	bool nak = false;
	if (send_packet_and_wait_ack(fd, packet, packetLength, kCommandAckTimeoutMs, &nak) != 0)
	{
		result = IIC_OTA_ERR_COMMAND_TIMEOUT;
	}
	else if (nak)
	{
		result = IIC_OTA_ERR_COMMAND_NAK;
	}
	else
	{
		// Matches the Sleep(500) preceding f021_DownloadImage() in case 8.
		usleep(500 * 1000);

		flush_port(fd);
		result = download_image_checksum(fd, fh, kByteTimeoutMs, true);
	}

	// Release the F280049 from traffic-forward mode on success AND on
	// failure, so it can process the normal protocol again (e.g. a retrigger
	// of LFU mode). Best effort: a failure here never masks `result`.
	// send_forward_exit() drains the output queue, so the final flush can
	// no longer discard the tail of the image or the magic bytes.
	send_forward_exit(fd);

	//Wait for the last two bytes to go out on the wire.
	usleep(1 * 1000);

	flush_port(fd);
	fclose(fh);
	close(fd);

	return result;
}

int iic_ota_f280049(const char *bank0_firmware_file, const char *bank1_firmware_file,
					 const char *serial_port, int baudrate)
{
	// Single generic F280049 trigger -- the device, not this command value,
	// now decides which bank gets written (see read_bank_select() below).
	const uint16_t command = kF280049LiveDfuCommand;

	if (!bank0_firmware_file || !bank0_firmware_file[0] ||
		!bank1_firmware_file || !bank1_firmware_file[0] ||
		!serial_port || !serial_port[0])
	{
		return IIC_OTA_ERR_INVALID_ARG;
	}

	speed_t speed;
	if (!lookup_baud(baudrate, &speed))
	{
		return IIC_OTA_ERR_UNSUPPORTED_BAUD;
	}

	FILE *fh0 = fopen(bank0_firmware_file, "rb");
	if (!fh0)
	{
		return IIC_OTA_ERR_FILE_OPEN;
	}
	FILE *fh1 = fopen(bank1_firmware_file, "rb");
	if (!fh1)
	{
		fclose(fh0);
		return IIC_OTA_ERR_FILE_OPEN;
	}

	int fd = open(serial_port, O_RDWR | O_NOCTTY);
	if (fd < 0)
	{
		fclose(fh0);
		fclose(fh1);
		return IIC_OTA_ERR_PORT_OPEN;
	}

	if (configure_port(fd, speed) != 0)
	{
		close(fd);
		fclose(fh0);
		fclose(fh1);
		return IIC_OTA_ERR_PORT_CONFIG;
	}

	// Matches the Sleep(6) immediately preceding autobaudLock() in
	// serial_flash_programmer.cpp's _tmain(), for both the dual-core and
	// single-core device branches.
	flush_port(fd);
	usleep(6 * 1000);

	if (autobaud_lock(fd, kAutobaudTimeoutMs) != 0)
	{
		close(fd);
		fclose(fh0);
		fclose(fh1);
		return IIC_OTA_ERR_AUTOBAUD_TIMEOUT;
	}

	// Live DFU command packet -- always length=0/data=NULL, matching case 8.
	uint8_t packet[16];
	uint32_t packetLength = construct_packet(packet, command, 0, NULL);

	// Matches the Sleep(500) preceding f021_SendPacket() in case 8.
	usleep(500 * 1000);

	bool nak = false;
	if (send_packet_and_wait_ack(fd, packet, packetLength, kCommandAckTimeoutMs, &nak) != 0)
	{
		close(fd);
		fclose(fh0);
		fclose(fh1);
		return IIC_OTA_ERR_COMMAND_TIMEOUT;
	}
	if (nak)
	{
		close(fd);
		fclose(fh0);
		fclose(fh1);
		return IIC_OTA_ERR_COMMAND_NAK;
	}

	uint8_t bank;
	bool invalidBank = false;
	if (read_bank_select(fd, kCommandAckTimeoutMs, &bank, &invalidBank) != 0)
	{
		close(fd);
		fclose(fh0);
		fclose(fh1);
		return IIC_OTA_ERR_COMMAND_TIMEOUT;
	}

	if (invalidBank)
	{
		close(fd);
		fclose(fh0);
		fclose(fh1);
		return IIC_OTA_ERR_BANK_SELECT;
	}

	FILE *fh = (bank == kBankSelect0) ? fh0 : fh1;
	fclose(bank == kBankSelect0 ? fh1 : fh0);

	// Matches the Sleep(500) preceding f021_DownloadImage() in case 8.
	usleep(500 * 1000);

	flush_port(fd);
	int result = download_image_checksum(fd, fh, kByteTimeoutMs, false);
	
	//Wait for the last two bytes to go out on the wire.
	usleep(1 * 1000);

	flush_port(fd);
	fclose(fh);
	close(fd);

	return result;
}
