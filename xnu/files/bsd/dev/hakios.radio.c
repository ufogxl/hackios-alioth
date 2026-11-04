/*
 * The seam between the Wi-Fi's upper layers and whatever radio is underneath them.
 *
 * The upper layers -- Apple80211 answered in the kernel (hakios.apple80211.c), the station boundary
 * (hakios.sta.c), and the Linux driver's own primitives (hakios.lx.c) -- are the same for any radio:
 * they are what iOS talks to. The radio is not: the one they were written against is the POCO X5
 * Pro's WCN6750, a part that runs its own firmware on the WPSS and needs no PCIe at all, while the
 * Redmi K40's QCA6390 is a PCIe endpoint. Those two share nothing.
 *
 * So the radio was taken out of this kernel's build (bsd/conf/files) and these are the symbols the
 * upper layers still call. Here they answer for a kernel with no radio in it: nothing is up, there is
 * no firmware, there is no register window, there is no address, and the network the station would
 * join does not exist. When a radio is built again, it provides these instead.
 *
 * Every one of them is a question, not a command: nothing here can change the state of anything,
 * which is why the upper layers can be run and watched with no radio present.
 */
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <sys/systm.h>
#include <pexpert/pexpert.h>
extern void loader_log_putc(char);

static int
radio_vsay(const char *format, va_list list)
{
	char line[512];
	int length = vsnprintf(line, sizeof(line), format, list);
	for (const char *c = line; *c; c++) loader_log_putc(*c);
	printf("%s", line);
	return length;
}

int
hakios_wifi_say(const char *format, ...)
{
	uint32_t quiet = 0;
	PE_parse_boot_argn("wifi_quiet", &quiet, sizeof(quiet));
	if (quiet) return 0;
	va_list list;
	va_start(list, format);
	int length = radio_vsay(format, list);
	va_end(list);
	return length;
}

int
hakios_wifi_tell(const char *format, ...)
{
	va_list list;
	va_start(list, format);
	int length = radio_vsay(format, list);
	va_end(list);
	return length;
}

uint32_t
hakios_wifi_option(const char *name)
{
	uint32_t value = 0;
	PE_parse_boot_argn(name, &value, sizeof(value));
	return value;
}

bool
hakios_wifi_dsp_is_up(void)
{
	return false;
}

bool
hakios_wifi_radio_map(uint64_t physical, uint64_t size, unsigned long long *address)
{
#pragma unused(physical, size)

	if (address != NULL) {
		*address = 0;
	}
	return false;
}

bool
hakios_wifi_radio_msi(unsigned long long *address, uint32_t *data, uint32_t *first_spi)
{
	if (address != NULL) {
		*address = 0;
	}
	if (data != NULL) {
		*data = 0;
	}
	if (first_spi != NULL) {
		*first_spi = 0;
	}
	return false;
}

/* The same wifi_mac boundary as the original port; no device value in source. */
bool
hakios_wifi_radio_address(uint8_t *address)
{
	char text[32] = "";
	uint8_t bytes[6], any = 0;
	if (!address || !PE_parse_boot_arg_str("wifi_mac", text, sizeof(text)) || strlen(text) != 17) return false;
	for (unsigned i = 0; i < 6; i++) {
		unsigned value = 0;
		for (unsigned j = 0; j < 2; j++) {
			unsigned char c = text[i * 3 + j];
			int digit = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 :
			    c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
			if (digit < 0) return false;
			value = value * 16 + (unsigned)digit;
		}
		if (i < 5 && text[i * 3 + 2] != ':') return false;
		bytes[i] = (uint8_t)value;
		any |= bytes[i];
	}
	if (!any || (bytes[0] & 1)) return false;
	memcpy(address, bytes, sizeof(bytes));
	hakios_wifi_tell("wifi: station address supplied and validated\n");
	return true;
}

bool
hakios_wifi_network(uint8_t *name, unsigned *name_length, uint8_t *passphrase, unsigned *passphrase_length)
{
#pragma unused(name, name_length, passphrase, passphrase_length)
	return false;
}

const uint8_t *
hakios_wifi_driver_file(const char *name, uint64_t *size)
{
#pragma unused(name)

	if (size != NULL) {
		*size = 0;
	}
	return NULL;
}
