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

#define NO_RADIO        "radio: this kernel has no Wi-Fi radio built into it (the QCA6390 is a PCIe part and is not driven yet)\n"

int
hakios_wifi_say(const char *format, ...)
{
#pragma unused(format)
	return 0;
}

int
hakios_wifi_tell(const char *format, ...)
{
#pragma unused(format)
	return 0;
}

uint32_t
hakios_wifi_option(const char *name)
{
#pragma unused(name)
	return 0;                               /* no boot-argument has anything to say without a radio */
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

bool
hakios_wifi_radio_address(uint8_t *address)
{
#pragma unused(address)
	return false;
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
