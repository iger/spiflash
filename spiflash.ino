/**
 * \file SPI Flash reader for the Teensy 3.
 *
 * Fast reader for SPI flashes, using the native SPI hardware of the Teensy 3.
 * Build this with the Teensyduino environment and flash it to the
 * microcontroller.
 *
 * An 8-pin SOIC chip clip makes it easy to attach to the motherboard ROM.
 * Be sure to disconnect the laptop battery before powering up the Teensy!
 *
 * Black = ground
 * Red = 3.3 V
 * Green = clock
 * White = CS#
 * Blue = MOSI (SI on chip)
 * Brown = MISO (SO on chip)
 *
 *   White   CS   --- 1    8 --- VCC     Red
 *   Brown   SO   --- 2    7 --- HOLD#
 *           WP   --- 3    6 --- SCLK    Green
 *   Black   GND  --- 4    5 --- SI      Blue
 *
 * Bus pirate commands:
 * 0x4B 01001011 -- power, no pullup, aux=1, cs=1
 * 0x67 01100111 -- spi speed == 8 MHz
 * 0x8A 10001010 -- spi config 3.3v, CKP idle low, CKE active to idle, sample middle
 * 0x03 00000011 -- cs high
 *
 * Manual mode:
Bus Pirate v3b                                                                  
Firmware v5.10 (r559)  Bootloader v4.4                                          
DEVID:0x0447 REVID:0x3043 (24FJ64GA002 B5)                                      
http://dangerousprototypes.com                                                  
CFG1:0xFFDF CFG2:0xFF7F                                                         
*----------*                                                                    
Pinstates:                                                                      
1.(BR)  2.(RD)  3.(OR)  4.(YW)  5.(GN)  6.(BL)  7.(PU)  8.(GR)  9.(WT)  0.(Blk) 
GND     3.3V    5.0V    ADC     VPU     AUX     CLK     MOSI    CS      MISO    
P       P       P       I       I       I       O       O       O       I       
GND     3.22V   4.91V   0.00V   0.00V   L       L       L       H       H       
Power supplies ON, Pull-up resistors OFF, Normal outputs (H=3.3v, L=GND)        
MSB set: MOST sig bit first, Number of bits read/write: 8                       
a/A/@ controls AUX pin                                                          
SPI (spd ckp ske smp csl hiz)=( 4 0 1 0 1 0 )                                   
*----------*
 *
 * {0x95,0,0]
 *
 */
#include <SPI.h>
#include "xmodem.h"
#include "ADC.h"
#include "VREF.h"

#ifdef CONFIG_SKETCHSAVER
#include "SketchSaver/SketchSaver.h"
#endif

#if 1
// teensy 3 alt pins
#define SPI_CS    2 // white or yellow
#define SPI_SCLK 14 // green
#define SPI_MOSI  7 // blue or purple
#define SPI_MISO  8 // brown
#define SPI_WP    0
#define SPI_HOLD  1
#define SPI_VCC  15
#else
// teensy 2 pins
#define SPI_CS   0 // white or yellow
#define SPI_SCLK 1 // green
#define SPI_MOSI 3 // blue or purple
#define SPI_MISO 4 // brown
#endif

#define SPI_PAGE_SIZE	4096
#define SPI_PAGE_MASK	(SPI_PAGE_SIZE - 1)

// Flash commands
#define SPI_CMD_WRDI		0x04 // Write Disable
#define SPI_CMD_WREN		0x06 // Write Enable
#define SPI_CMD_RDID		0x9F // Read ID
#define SPI_CMD_RDID90		0x90 // Read ID
#define SPI_CMD_RDSR		0x05 // Read status register
#define SPI_CMD_WRSR		0x01 // Write status register
#define SPI_CMD_READ		0x03 // Read data bytes
#define SPI_CMD_READ4		0x13 // Read data bytes with 4-byte address
#define SPI_CMD_FAST_READ	0x0B // Read at higher speed
#define SPI_CMD_SE		0x20 // Sector erase
#define SPI_CMD_SE4		0x21 // Sector erase with 4-byte address
#define SPI_CMD_PP		0x02 // Page Program (write to flash)
#define SPI_CMD_PP4		0x12 // Page Program with 4-byte address
#define SPI_CMD_BRRD		0x16 // Read bank address register
#define SPI_CMD_BRWR		0x17 // Write bank address register
#define SPI_CMD_RDSCUR		0x2B // Read security register
#define SPI_CMD_ENSO		0xB1 // Enter secured OTP
#define SPI_CMD_EXSO		0xC1 // Exit secured OTP


// Status Register bits
#define SPI_SRWD		0x80 // Status Register Write Disable
#define SPI_WIP			0x01 // Write in Progress
#define SPI_WEL			0x02 // Write Enable


static uint32_t chip_size; // in bytes

SPISettings spi_settings(20000000, MSBFIRST, SPI_MODE3);
ADC* adc = new ADC();

static inline void
spi_cs(int i)
{
//	if (i) {
//		SPI.beginTransaction(spi_settings);
//		delayMicroseconds(200);
//	} else {
//		SPI.endTransaction();
//	}
	
	if (i)
	{
		pinMode(SPI_CS, OUTPUT);
//		SPI.begin();
//		SPI.beginTransaction(spi_settings);
	} else {
//		SPI.endTransaction();
	}

	digitalWrite(SPI_CS, !i);
	if (i) {
//		delayMicroseconds(1);
	} else {
		delayMicroseconds(1);
		digitalWrite(SPI_CS, 0);
	}
}


void
setup()
{
	Serial.begin(115200);
	SPI.setSCK(SPI_SCLK);
	SPI.setMOSI(SPI_MOSI);
	SPI.setMISO(SPI_MISO);
	SPI.begin();
	SPI.beginTransaction(spi_settings);

	// keep the SPI flash unselected until we talk to it
	pinMode(SPI_CS, OUTPUT);
	spi_cs(0);

	chip_size = 8 * 1024 * 1024;
}


static int
usb_serial_getchar_echo()
{
	while (1)
	{
		int c = Serial.read();
		if (c == -1)
			continue;

		// echo back to the serial port
		Serial.print((char) c);
		if (c == '\r')
			Serial.print('\n');

		return c;
	}
}


static xmodem_block_t xmodem_block;


static char
hexdigit(
	uint8_t x
)
{
	x &= 0xF;
	if (x < 0xA)
		return x + '0';
	else
		return x + 'A' - 0xA;
}


static inline uint8_t
spi_send(
	uint8_t c
)
{
	return SPI.transfer(c);
}


// Select a 4-byte or 3-byte address for the read command
static void
spi_choose(
	uint32_t addr,
	uint8_t cmd3,
	uint8_t cmd4
)
{
	if ((addr >> 24) != 0x00)
	{
		spi_send(cmd4);
		spi_send(addr >> 24);
	} else {
		spi_send(cmd3);
	}

	spi_send(addr >> 16);
	spi_send(addr >>  8);
	spi_send(addr >>  0);
}


static void
spi_read_command(
	uint32_t addr
)
{
	spi_choose(addr, SPI_CMD_READ, SPI_CMD_READ4);
}


static void
spi_write_command(
	uint32_t addr
)
{
	spi_choose(addr, SPI_CMD_PP, SPI_CMD_PP4);
}

static void
spi_erase_command(
	uint32_t addr
)
{
	spi_choose(addr, SPI_CMD_SE, SPI_CMD_SE4);
}


/** Read electronic manufacturer and device id */
static void
spi_rdid(uint8_t cmd)
{
	//delay(2);

	spi_cs(1);
	delayMicroseconds(100);
	uint8_t b1, b2, b3, b4;

	if (cmd == SPI_CMD_RDID90) {
		// RES -- read electronic id
		spi_send(cmd);
		spi_send(0x0);
		spi_send(0x0);
		spi_send(0x0);
		b1 = spi_send(0xFF);
		b2 = spi_send(0xFF);
		b3 = 0;
		b4 = 0;
	} else {
		// JEDEC RDID: 1 byte out, three bytes back
		spi_send(cmd);

		// read 3 bytes back
		b1 = spi_send(0x01);
		b2 = spi_send(0x02);
		b3 = spi_send(0x04);
		b4 = spi_send(0x17);
		//b4 = 99;
	}

	spi_cs(0);
	delay(1);

	char buf[16];
	uint8_t off = 0;
	buf[off++] = hexdigit(b1 >> 4);
	buf[off++] = hexdigit(b1 >> 0);
	buf[off++] = hexdigit(b2 >> 4);
	buf[off++] = hexdigit(b2 >> 0);
	buf[off++] = hexdigit(b3 >> 4);
	buf[off++] = hexdigit(b3 >> 0);
	buf[off++] = hexdigit(b4 >> 4);
	buf[off++] = hexdigit(b4 >> 0);
	buf[off++] = '\r';
	buf[off++] = '\n';
	buf[off++] = '\0';

	Serial.print(buf);
}


/** Read the status register (RDSR) */
static uint8_t
spi_status(void)
{
	spi_cs(1);
	spi_send(SPI_CMD_RDSR);
	uint8_t r1 = spi_send(0x00);
	spi_cs(0);
	return r1;
}


static void
spi_status_interactive(void)
{
	// read the status register
	uint8_t sr = spi_status();
	char buf[16];
	uint8_t off = 0;
	buf[off++] = hexdigit(sr >> 4);
	buf[off++] = hexdigit(sr >> 0);
	buf[off++] = '\0';
	Serial.println(buf);
}


static void
spi_write_status(uint8_t sr)
{
	spi_cs(1);
	spi_send(SPI_CMD_WREN);
	spi_cs(0);
	delay(1);
	spi_cs(1);
	spi_send(SPI_CMD_WRSR);
	spi_send(sr);
	spi_cs(0);
}


static void
spi_bank_address_register_interactive(void)
{
	spi_cs(1);
	spi_send(SPI_CMD_BRRD);
	uint8_t brac = spi_send(0x00);
	brac = spi_send(0x00);
	spi_cs(0);

	char buf[16];
	uint8_t off = 0;
	buf[off++] = hexdigit(brac >> 4);
	buf[off++] = hexdigit(brac >> 4);
	buf[off++] = '\0';
	Serial.println(buf);
}


static uint8_t
spi_rdscur(void)
{
	spi_cs(1);
	spi_send(SPI_CMD_RDSCUR);
	uint8_t r1 = spi_send(0x00);
	spi_cs(0);
	return r1;
}


static uint32_t
usb_serial_readhex(void)
{
	uint32_t val = 0;

	while (1)
	{
		int c = usb_serial_getchar_echo();
		if ('0' <= c && c <= '9')
			val = (val << 4) | (c - '0');
		else
		if ('A' <= c && c <= 'F')
			val = (val << 4) | (c - 'A' + 0xA);
		else
		if ('a' <= c && c <= 'f')
			val = (val << 4) | (c - 'a' + 0xA);
		else
			return val;
	}
}


void
usb_serial_writehex(uint32_t hex, int8_t digits)
{
	char buf[16];
	char * p = buf + 16;
	*--p = '\0';
	for (int8_t i = 0; i < digits && i < 16; ++i) {
		*--p = hexdigit(hex);
		hex >>= 4;
	}
	Serial.print(p);
}


/** Set the Write Enable (WEL) bit in the status register */
static void
spi_write_enable(bool enable = true)
{
	delay(2);

	uint8_t r1 = spi_status();
	(void) r1; // unused

	spi_cs(1);
	if (enable)
		spi_send(SPI_CMD_WREN);
	else
		spi_send(SPI_CMD_WRDI);
	spi_cs(0);
}


static void
spi_write_enable_interactive(bool enable)
{
	spi_write_enable(enable);

	uint8_t r2 = spi_status();

	char buf[16];
	uint8_t off =0;
	buf[off++] = hexdigit(r2 >> 4);
	buf[off++] = hexdigit(r2 >> 0);
	if (!!(r2 & SPI_WEL) != !!enable)
		buf[off++] = '!';

	buf[off++] = '\r';
	buf[off++] = '\n';
	buf[off++] = '\0';
	Serial.print(buf);
}


static void
spi_enter_otp_mode(bool enter)
{
	spi_cs(1);
	spi_send(enter ? SPI_CMD_ENSO : SPI_CMD_EXSO);
	spi_cs(0);
}


static void
spi_erase_sector(
	uint32_t addr
)
{
	spi_cs(1);
	spi_erase_command(addr);
	spi_cs(0);

	while (spi_status() & SPI_WIP)
		;
}


static void
spi_erase_sector_interactive(void)
{
	uint32_t addr = usb_serial_readhex();

	if ((spi_status() & SPI_WEL) == 0)
	{
		Serial.print("wp!\r\n");
		return;
	}

	spi_erase_sector(addr);

	char buf[16];
	uint8_t off = 0;
	buf[off++] = 'E';
	buf[off++] = hexdigit(addr >> 28);
	buf[off++] = hexdigit(addr >> 24);
	buf[off++] = hexdigit(addr >> 20);
	buf[off++] = hexdigit(addr >> 16);
	buf[off++] = hexdigit(addr >> 12);
	buf[off++] = hexdigit(addr >>  8);
	buf[off++] = hexdigit(addr >>  4);
	buf[off++] = hexdigit(addr >>  0);
	buf[off++] = '\r';
	buf[off++] = '\n';
	buf[off++] = '\0';

	Serial.print(buf);
}

static void
spi_read(
	uint32_t addr
)
{
	//delay(2);

	spi_cs(1);
	//delay(1);

	// read a page
	spi_read_command(addr);

	uint8_t data[16];

	for (int i = 0 ; i < 16 ; i++)
		data[i] = spi_send(0);

	spi_cs(0);

	char buf[16*3+8+2+3];
	uint8_t off = 0;
	buf[off++] = hexdigit(addr >> 28);
	buf[off++] = hexdigit(addr >> 24);
	buf[off++] = hexdigit(addr >> 20);
	buf[off++] = hexdigit(addr >> 16);
	buf[off++] = hexdigit(addr >> 12);
	buf[off++] = hexdigit(addr >>  8);
	buf[off++] = hexdigit(addr >>  4);
	buf[off++] = hexdigit(addr >>  0);
	buf[off++] = ':';
	buf[off++] = ' ';

	for (int i = 0 ; i < 16 ; i++)
	{
		buf[off++] = hexdigit(data[i] >> 4);
		buf[off++] = hexdigit(data[i] >> 0);
		buf[off++] = ' ';
	}
	buf[off++] = '\r';
	buf[off++] = '\n';
	buf[off++] = '\0';

	Serial.print(buf);
//	Serial.send_now();
}

static void
serial_write_nofail(const uint8_t * buffer, size_t size)
{
	while (size > 0) {
		int w = Serial.write(buffer, size);
		if (w > 0) {
			buffer += w;
			size -= w;
		}
	}
}

/** Read the entire ROM out to the serial port. */
static void
spi_dump(void)
{
	delay(1);

	uint32_t addr = 0;
//	uint8_t buf[256];
	uint8_t buf[64];

	while (1)
	{
		spi_cs(1);
		spi_read_command(addr);

		for (uint32_t off = 0 ; off < sizeof(buf) ; off++)
			buf[off] = spi_send(0);

		spi_cs(0);

		serial_write_nofail(buf, sizeof(buf));
		Serial.send_now();
		Serial.flush();

		addr += sizeof(buf);
		if (addr >= chip_size)
			break;
	}

}

static void
prom_send(void)
{
	// We have already received the first nak.
	// Fire it up!
	if (xmodem_init(&xmodem_block, 1) < 0)
		return;

	//delay(1);

	uint32_t addr = 0;

	while (1)
	{
		spi_cs(1);
		spi_read_command(addr);

		for (uint8_t off = 0 ; off < sizeof(xmodem_block.data) ; off++)
			xmodem_block.data[off] = spi_send(0);

		spi_cs(0);

		if (xmodem_send(&xmodem_block, 1) < 0)
			return;

		addr += sizeof(xmodem_block.data);
		if (addr >= chip_size)
			break;
	}


	xmodem_fini(&xmodem_block);
}

static uint32_t unmatch_addr;
static uint8_t unmatch_data;
static uint8_t unmatch_rom;

/** Read from SPI flash and check if matches supplied data. */
bool
spi_flash_matches_data(uint32_t addr, const uint8_t data[], uint32_t size)
{
	bool matched = true;
	spi_cs(1);
	spi_read_command(addr);

	for (uint32_t i = 0; i < size; i++)
	{
		uint8_t rom = spi_send(0);

		if (data[i] == rom)
			continue;

		unmatch_addr = addr + i;
		unmatch_data = data[i];
		unmatch_rom = rom;

		matched = false;
		break;
	}
	spi_cs(0);
	return matched;
}

/** Write some number of pages into the PROM. */
static void
spi_upload(void)
{
	uint32_t addr = usb_serial_readhex();
	uint32_t len = usb_serial_readhex();

	// addr and len must be 4k aligned
	const int fail = ((len & SPI_PAGE_MASK) != 0) || ((addr & SPI_PAGE_MASK) != 0);

	if (fail) {
		Serial.print("bad request to write at address 0x");
	} else {
		Serial.print("writing at address 0x");
	}
	usb_serial_writehex(addr, 8);
	Serial.print(" bytes 0x");
	usb_serial_writehex(len, 8);
	Serial.println();
	if (fail)
		return;

	uint32_t offset = 0;
#if 0
	const size_t chunk_size = sizeof(xmodem_block.data);
	uint8_t * const buf = xmodem_block.data;

	for (offset = 0 ; offset < len ; offset += chunk_size)
	{
		// read 128 bytes into the xmodem data block
		for (uint8_t i = 0 ; i < chunk_size; i++)
		{
			int c;
			while ((c = Serial.read()) == -1)
				;
			buf[i] = c;
		}

		if ((addr & SPI_PAGE_MASK) == 0)
		{
			// new sector; erase this one
			spi_write_enable();
			spi_erase_sector(addr);

			off = 0;
			outbuf[off++] = hexdigit(addr >> 20);
			outbuf[off++] = hexdigit(addr >> 16);
			outbuf[off++] = hexdigit(addr >> 12);
			outbuf[off++] = hexdigit(addr >>  8);
			outbuf[off++] = hexdigit(addr >>  4);
			outbuf[off++] = hexdigit(addr >>  0);
			outbuf[off++] = '\r';
			outbuf[off++] = '\n';
			outbuf[off++] = '\0';
			Serial.print(outbuf);
		}


		spi_write_enable();
		uint8_t r2 = spi_status();
		(void) r2; // unused

		spi_cs(1);
		spi_write_command(addr);

		for (uint8_t i = 0 ; i < chunk_size ; i++)
			spi_send(buf[i]);

		spi_cs(0);

		// wait for write to finish
		while (spi_status() & SPI_WIP)
			;

		//Serial.print(".");
		addr += chunk_size;
	}

	Serial.print("done!\r\n");
#else
	// read an entire page, then compare it to what is in the ROM
	const size_t chunk_size = SPI_PAGE_SIZE;
	uint8_t buf[SPI_PAGE_SIZE];
	int empty_count = 0;
	int match_count = 0;
	int write_count = 0;
	int retry_count = 0;

	for (offset = 0 ; offset < len ; offset += chunk_size, addr += chunk_size)
	{
		// print the address every 256 KB
		if ((addr & ((64 * SPI_PAGE_SIZE) - 1)) == 0)
		{
			Serial.println();
			usb_serial_writehex(addr, 8);
			Serial.print(": ");
			Serial.flush();
		}

		// read a chunk of data from the serial port
		// keeping track if this is an empty page (all 0xff)
		bool input_ff = true;
		for (uint16_t i = 0 ; i < chunk_size; i++)
		{
			int c;
			while ((c = Serial.read()) == -1)
				;
			buf[i] = c;
			if (c != 0xff)
				input_ff = false;
		}

		const int32_t max_retries = 5;
		for (int32_t attempt = 0; attempt <= max_retries; ++attempt) {

			// read the flash and compare it to the buffer
			bool matched = spi_flash_matches_data(addr, buf, chunk_size);
			if (matched)
			{
				// everything matched, no need to touch this page
				Serial.print('.');
				match_count++;
				break;
			}

			// warn if retrying
			if (attempt > 0) {
				Serial.print("\nWriting ");
				usb_serial_writehex(addr, 8);
				Serial.print(" attempt ");
				usb_serial_writehex(attempt, 2);
				retry_count++;
			}

			// there was a mismatch. erase the page and write it
			spi_write_enable();
			spi_erase_sector(addr);


			// if the source was all 0xff, we do not need to write
			// after the erase has completed
			if (input_ff)
			{
				Serial.print('e');
				empty_count++;
				continue;
			}

			// write the 4K page in 256 byte chunks
			for (uint16_t i = 0 ; i < chunk_size ; i += 256)
			{
				spi_write_enable();
				uint8_t r2 = spi_status();
				(void) r2; // unused

				spi_cs(1);
				spi_write_command(addr+i);

				for (uint16_t j = 0 ; j < 256 ; j++)
					spi_send(buf[i+j]);

				spi_cs(0);

				// wait for write to finish
				while (spi_status() & SPI_WIP)
					;
			}
			Serial.print('w');
			write_count++;
		}
	}

	Serial.print("\r\nmatch: ");
	Serial.print(match_count);
	Serial.print(" empty: ");
	Serial.print(empty_count);
	Serial.print(" write: ");
	Serial.print(write_count);
	Serial.print(" retry: ");
	Serial.println(retry_count);
	Serial.write("Last unmatch @");
	usb_serial_writehex(unmatch_addr, 8);
	Serial.write(" ");
	usb_serial_writehex(unmatch_rom, 2);
	Serial.write(" -> ");
	usb_serial_writehex(unmatch_data, 2);
	Serial.println();
	Serial.flush();
#endif
}

static void
read_voltage() {
	float voltage_target=3.3;
 
	adc->setReference(ADC_REFERENCE::REF_3V3);
	VREF::start(VREF_SC_MODE_LV_HIGHPOWERBUF);
	VREF::waitUntilStable();
	adc->setConversionSpeed(ADC_CONVERSION_SPEED::LOW_SPEED);
	adc->setSamplingSpeed(ADC_SAMPLING_SPEED::LOW_SPEED);
	adc->setConversionSpeed(ADC_CONVERSION_SPEED::LOW_SPEED, ADC_1);
	adc->setSamplingSpeed(ADC_SAMPLING_SPEED::LOW_SPEED, ADC_1);
	adc->setAveraging(32);
	adc->setResolution(16);
	adc->setAveraging(32, ADC_1);
	adc->setResolution(16, ADC_1);

	float va = (float) adc->analogRead(SPI_VCC, ADC_0) / adc->getMaxValue(ADC_0);
	float v33 = 1.0/adc->analogRead(ADC_INTERNAL_SOURCE::BANDGAP)*adc->getMaxValue(ADC_0);
	Serial.print("3.3V pin value: ");
	Serial.print(v33, 5);
	Serial.println(" V.");
	Serial.print("  vcc det value: ");
	Serial.print(va * v33, 5);
	Serial.print(" V, drop: ");
	Serial.print((1 - va) * v33, 5);
	Serial.println(" V.");

	Serial.print("Bandgap value: ");
	//Serial.print(3.3*adc->analogRead(ADC_INTERNAL_SOURCE::BANDGAP)/adc->getMaxValue(ADC_0), 5);
	adc->adc0->analogRead(ADC_INTERNAL_SOURCE::BANDGAP);
	adc->adc0->differentialMode(); // Use differential mode for better precision.
	Serial.print(voltage_target*adc->adc0->readSingle()/adc->getMaxValue(ADC_0), 5);
	Serial.println(" V. (Should be between 0.97 and 1.03 V.)");
}

static void
read_hold() {
	pinMode(SPI_HOLD, INPUT);
	bool v = digitalRead(SPI_HOLD);
	Serial.print("Hold: ");
	Serial.println(v ? "1" : "0");
}

static const char usage[] =
"Commands:\r\n"
" i           Read RDID from the flash chip (cmd 9F)\r\n"
" I           Read RDID from the flash chip (cmd 90)\r\n"
" rADDR       Read 16 bytes from address\r\n"
" .           Read the next 16 bytes\r\n"
" R           SPI dump\r\n"
" w           Enable writes (interactive)\r\n"
" W           Disable writes (interactive)\r\n"
" eADDR       Erase a sector\r\n"
" uADDR LEN   Upload new code for a section of the ROM\r\n"
" sNN         Chip size in MB (in hex)\r\n"
" SNN         Chip size in bytes (in hex)\r\n"
" x           Read the status register\r\n"
" t           Tri-state the pins to release the bus\r\n"
" g           Read security register\r\n"
" o/O         ENSO/EXSO - Enter/leave OTP mode (area)\r\n"
" b           Read the bank address register\r\n"
" BX          Write the bank address register\r\n"
" h           Read hold pin\r\n"
" v           Measure analog voltages\r\n"
"\r\n"
"To read the entire ROM, start an x-modem transfer.\r\n"
"\r\n";

static uint32_t addr;

void
loop()
{
	int c;
	if ((c = Serial.read()) == -1)
		return;

	bool prompt = true;

	switch(c)
	{
	case 'i': spi_rdid(SPI_CMD_RDID); break;
	case 'I': spi_rdid(SPI_CMD_RDID90); break;
	case 'r':
		addr = usb_serial_readhex();
		spi_read(addr);
		break;

	case 's':
		chip_size = usb_serial_readhex() << 20;
		Serial.print("Chip size set to ");
		Serial.print(chip_size >> 20);
		Serial.print(" MiB.\r\n");
		break;

	case 'S':
		chip_size = usb_serial_readhex();
		Serial.print("Chip size set to ");
		Serial.print(chip_size);
		Serial.print(" B.\r\n");
		break;

	case '.':
		// read the next 16 bytes
		spi_read(addr += 16);
		break;

	case 'x':
		// read the status register
		spi_status_interactive();
		break;

	case 'X':
	{
		// set the status register; WEL will be set first
		uint8_t sr = usb_serial_readhex();
		spi_write_status(sr);
		spi_status_interactive();
		break;
	}

	case 'g':
		Serial.print("Security register: ");
		usb_serial_writehex(spi_rdscur(), 2);
		Serial.print("\r\n");
		break;

	case 'b':
	{
		// read the bank address register for large chips
		spi_bank_address_register_interactive();
		break;
	}

	case 'B':
	{
		// set the bank address register
		uint8_t brac = usb_serial_readhex();
		spi_cs(1);
		spi_send(SPI_CMD_BRWR);
		spi_send(brac);
		spi_cs(0);

		spi_bank_address_register_interactive();
		break;
	}

#ifdef CONFIG_SKETCHSAVER
	case 'S':
		sketch_output();
		break;
#endif

	case 't':
		pinMode(SPI_CS, INPUT);
		digitalWrite(SPI_CS, 0);
		SPI.end();
		Serial.println("TRISTATE");
		break;

	case 'R': spi_dump(); prompt = false; break;
	case 'w': spi_write_enable_interactive(true); break;
	case 'W': spi_write_enable_interactive(false); break;
	case 'o':
		spi_enter_otp_mode(true);
		Serial.print("Entered OTP mode.\r\n");
		break;
	case 'O':
		spi_enter_otp_mode(false);
		Serial.print("Exited OTP mode.\r\n");
		break;
	case 'e': spi_erase_sector_interactive(); break;
	case 'u': spi_upload(); break;
	case XMODEM_NAK:
		prom_send();
		Serial.print("xmodem done\r\n");
		break;
        case 'v': read_voltage(); break;
	case 'h': read_hold(); break;
	case '?': Serial.print(usage);
		break;
	case '\r':
	case '\n':
		Serial.print("\r\n");
		break;
	default:
		Serial.print("?\r\n");
		break;
	}

	if (prompt) {
		Serial.print(">");
        }
}
