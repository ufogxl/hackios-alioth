/*
 * What more than one driver needs, and what used to live inside the Wi-Fi driver of the POCO X5 Pro.
 *
 * hakios.wifi.c was written first, for that phone's radio (a WCN6750 on the WPSS, no PCIe), and as
 * the other drivers grew they borrowed from it: the four locks below, the small image's fw/ file
 * loader, and the three lifecycle hooks that hakios.input.c and loader_return.c call. Nobody noticed
 * while the file kept being compiled.
 *
 * The Redmi K40's radio is a QCA6390 on PCIe and shares no code with that one, so the K40's kernel
 * leaves the whole Wi-Fi cluster of the POCO out of the build (bsd/conf/files). Doing that failed the
 * link on exactly eight symbols, which is how these were found:
 *
 *     hakios_aop_mailbox_lock  hakios_rsc_lock  hakios_smem_alloc_lock  hakios_smmu_lock
 *     hakios_wifi_file         hakios_wifi_init  hakios_wifi_quiesce     hakios_wifi_stop
 *
 * They are here now, where a kernel that needs them can have them without another phone's radio.
 *
 * The three hooks do nothing, and that is the whole of them: this kernel has no radio to start,
 * quieten or stop. They exist so that a caller does not have to know which kernel it is in.
 */

#include <libkern/libkern.h>
#include <kern/locks.h>
#include <mach/mach_types.h>
#include <sys/systm.h>

/* One group for the locks taken around the SoC's shared machinery (the AOP's mailbox, RPMh's
 * sequencer, the shared memory's allocator, the display-side SMMU), whoever the driver is. */
static LCK_GRP_DECLARE(shared_lock_group, "hakios-shared");

LCK_MTX_DECLARE(hakios_aop_mailbox_lock, &shared_lock_group);
LCK_MTX_DECLARE(hakios_smem_alloc_lock, &shared_lock_group);
/* RPMh's sequencer (TCS 0 of the RSC): the audio DSP's and the modem's drivers both ask through it */
LCK_MTX_DECLARE(hakios_rsc_lock, &shared_lock_group);
LCK_MTX_DECLARE(hakios_smmu_lock, &shared_lock_group);

/*
 * A file of the small image that travels with the kernel (hakios.image.c finds it in the RAM disk):
 * firmware, the board's data, an amplifier's waveform. It lived in the Wi-Fi driver because that was
 * the first thing here to need one; the haptics driver loads the aw8697's waveform through it and
 * the camera its calibration tables.
 */
extern const uint8_t *hakios_image_file(const char *dir, const char *name, uint64_t *size, int *disk_out);

const uint8_t *
hakios_wifi_file(const char *name, uint64_t *size)
{
	const uint8_t *data = hakios_image_file("fw", name, size, NULL);

	if (data == NULL) {
		printf("hakios: no fw/%s in the small image: put the file into fw/ and rebuild userland\n", name);
	}
	return data;
}

/* The radio's lifecycle, of which this kernel has none. */
void
hakios_wifi_init(void)
{
}

void
hakios_wifi_quiesce(void)
{
}

void
hakios_wifi_stop(void)
{
}
