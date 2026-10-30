/*
 * The display controller of the Redmi K40 (alioth): what it is doing, read only, step by step.
 * Boot-arg display_step=N performs steps 1 to N and stops; nothing here writes to the controller.
 *
 * Why it matters: the panel is driven "bl_ctrl_dcs" (its own device tree, qcom,mdss-dsi-bl-pmic-
 * control-type), which means the backlight is not a pin of a PMIC and not a register of a WLED --
 * it is DCS command 0x51 sent to the panel over DSI. The panel's own on-sequence says so:
 *
 *   39 01 00 00 00 00 03 51 00 00    brightness 0 (mi,mdss-dsi-bl-max-level = 0x7ff)
 *   39 01 00 00 00 00 02 53 20       CTRL_DISPLAY: dimming off
 *   05 01 00 00 14 00 02 28 00       display off   (the OFF command's first half)
 *   05 01 00 00 78 00 02 10 00       sleep in
 *
 * So both brightness and a real "display off" need the DSI controller's command path, which is
 * something this kernel has never driven. This file begins by finding out what state the
 * controller is in, because the bootloader sets the panel up and the panel is on now: the kernel's
 * own framebuffer is being scanned out of 0x9c000000 by this controller, which means MDSS and the
 * DSI link are running and their clocks are on. Reading their registers is therefore safe and
 * meaningful -- unlike a blind write, which is what the GPU's SMMU lesson was about.
 *
 * Addresses, from the phone's own device tree (dump/twrp-fdt.dts):
 *   qcom,mdss_mdp@ae00000               reg = <0xae00000 0x...>; the display subsystem's top
 *   qcom,mdss_dsi_ctrl0@ae94000         reg = <0xae94000 0x400 0xaf08000 0x04>; the DSI host
 *     ... "disp_cc_base" 0xaf08000      the display clock controller (its own register block)
 *   qcom,mdss_dsi_phy0@ae94400          reg = <0xae94400 0x7c0 0xae94200 0x100>; the DSI PHY
 *
 * The dump is raw and complete rather than a handful of named registers: the layout of this
 * controller's register block differs between hardware versions, and the version is one of the
 * things being found out. A register that was assumed and wrong is how the GPU's CBAR left-over
 * happened; here the numbers come first and the meaning after.
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/systm.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern kern_return_t thread_terminate(thread_t thread);

/* Every line goes to the loader's log directly as well, as the other bring-up drivers do. */
extern void loader_log_putc(char c);

static int say(const char *format, ...) __printflike(1, 2);

static int
say(const char *format, ...)
{
	char line[400];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	for (const char *c = line; *c != 0; c++) {
		loader_log_putc(*c);
	}
	return printf("%s", line);
}
#define printf say

static inline uint32_t
rd(vm_offset_t address)
{
	return *(volatile uint32_t *)address;
}

#define MDSS_MDP_PHYS           0x0ae00000UL
#define MDSS_MDP_SIZE           0x00100000UL
#define MDSS_DSI0_PHYS          0x0ae94000UL
#define MDSS_DSI0_SIZE          0x00000400UL
#define MDSS_DSI0_PHY_PHYS      0x0ae94400UL
#define MDSS_DSI0_PHY_SIZE      0x00000800UL
#define DISP_CC_PHYS            0x0af08000UL
#define DISP_CC_SIZE            0x00000400UL

static vm_offset_t mdp, dsi, phy, dcc;

/*
 * A block of registers, 16 to a line, as they read now. `why` says what these are expected to be
 * for, so the log says what was looked for and not only what was found.
 */
static void
dump(const char *who, vm_offset_t base, uint32_t from, uint32_t to)
{
	printf("display: --- %s, +%#x to +%#x ---\n", who, from, to);
	/* (a line is 16 registers, so 16 words: the outer step is 16 words too, not 16 bytes) */
	for (uint32_t at = from; at < to; at += 16 * 4) {
		printf("display: %s +%03x:", who, at);
		for (uint32_t i = 0; i < 16 && at + i * 4 < to; i++) {
			printf(" %08x", rd(base + at + i * 4));
		}
		printf("\n");
	}
}

static void
display_thread(void *parameter, __unused wait_result_t result)
{
	uint32_t steps = (uint32_t)(uintptr_t)parameter;

	printf("display: step 1: map the display controller's blocks and read what they say of themselves\n");
	mdp = ml_io_map(MDSS_MDP_PHYS, MDSS_MDP_SIZE);
	dsi = ml_io_map(MDSS_DSI0_PHYS, MDSS_DSI0_SIZE);
	phy = ml_io_map(MDSS_DSI0_PHY_PHYS, MDSS_DSI0_PHY_SIZE);
	dcc = ml_io_map(DISP_CC_PHYS, DISP_CC_SIZE);
	printf("display: mdss mdp %#lx, dsi0 %#lx, dsi0 phy %#lx, disp_cc %#lx\n", mdp, dsi, phy, dcc);
	printf("display: mdp HW_VERSION %#x, HW_INTR_STATUS %#x\n", rd(mdp + 0x0000), rd(mdp + 0x0010));
	/*
	 * The DSI host's first words. Hardware versions of this block put the version and the status
	 * here; which is which is part of what this dump is for, so both are simply said.
	 */
	printf("display: dsi +0x000 %#x, +0x004 %#x, +0x008 %#x, +0x00c %#x\n", rd(dsi + 0x000), rd(dsi + 0x004),
	    rd(dsi + 0x008), rd(dsi + 0x00c));
	printf("display: dsi +0x010 %#x, +0x014 %#x, +0x018 %#x, +0x01c %#x\n", rd(dsi + 0x010), rd(dsi + 0x014),
	    rd(dsi + 0x018), rd(dsi + 0x01c));
	printf("display: disp_cc +0x000 %#x, +0x004 %#x\n", rd(dcc + 0x000), rd(dcc + 0x004));
	if (steps < 2) {
		goto out;
	}

	/*
	 * The whole low half of the DSI host: this is where the clocks, the lane configuration, the
	 * command DMA and the error reporting live. Reading it all costs a few hundred words of log
	 * and removes every guess about which register is where at this hardware version.
	 */
	printf("display: step 2: the DSI host's registers, +0x000 to +0x0ff\n");
	dump("dsi0 host", dsi, 0x000, 0x100);
	if (steps < 3) {
		goto out;
	}

	printf("display: step 3: the DSI host's registers, +0x100 to +0x1ff\n");
	dump("dsi0 host", dsi, 0x100, 0x200);
	if (steps < 4) {
		goto out;
	}

	printf("display: step 4: the DSI PHY's registers, +0x000 to +0x0ff (common and lane)\n");
	dump("dsi0 phy", phy, 0x000, 0x100);
	if (steps < 5) {
		goto out;
	}

	printf("display: step 5: the display clock controller, +0x000 to +0x07f\n");
	dump("disp_cc", dcc, 0x000, 0x080);
	if (steps < 6) {
		goto out;
	}

	/*
	 * The MDP's top registers and its interrupt/status words. The panel is being scanned out of
	 * 0x9c000000 right now, so these say whether the MDP is in command mode (frames pushed by the
	 * DSI host on a TE signal) or video mode (a continuous stream), which decides how a DCS write
	 * has to be timed.
	 */
	printf("display: step 6: the MDP's top registers, +0x000 to +0x07f, and its display interface\n");
	dump("mdss mdp", mdp, 0x000, 0x080);
	dump("mdss mdp", mdp, 0x1000, 0x1040);
	if (steps < 7) {
		goto out;
	}

	/*
	 * Step 7, the last one and still read-only: the command buffer the bootloader itself last used.
	 *
	 * This is the DSI host's register layout as Linux defines it (drivers/gpu/drm/msm/dsi/dsi.xml,
	 * with the +4 that DSI_6G_REG_SHIFT adds -- this controller reads VERSION 3 at +0x1f4, so it is
	 * a 6G host and every register below is shifted by four). The words the last run read back:
	 *
	 *   +0x03c CMD_DMA_CTRL 0x14000000   FROM_FRAME_BUFFER | LOW_POWER
	 *   +0x044 CMD_CFG1     0x0013c2c    WR_MEM_START 44, WR_MEM_CONTINUE 60, INSERT_DCS_COMMAND
	 *   +0x048 DMA_BASE     0x9aeb2000   where the command's bytes were
	 *   +0x04c DMA_LEN      8
	 *   +0x084 TRIG_CTRL    4            TRIGGER_SW: the last transfer was started by software
	 *
	 * INSERT_DCS_COMMAND and a length of eight say the bootloader sent one short DCS write through
	 * this very path, so the bytes are still in that buffer. Reading them settles the one thing
	 * that would otherwise be guessed when a brightness command is written: the MSM command format
	 * in memory, which is not the MIPI packet -- dsi_host.c's dsi_cmd_dma_add() rearranges the
	 * header (data[0] = header[1], data[1] = header[2], data[2] = header[0], data[3] a flag word
	 * with bit 7 "last packet", bit 6 "long packet", bit 5 "read back") and pads with 0xff.
	 *
	 * Reading a buffer the hardware has already finished with changes nothing.
	 */
	printf("display: step 7: the command buffer the bootloader last used\n");
	{
		uint32_t dma_ctrl = rd(dsi + 0x03c), cfg1 = rd(dsi + 0x044);
		uint32_t base_phys = rd(dsi + 0x048), length = rd(dsi + 0x04c);
		uint32_t trigger = rd(dsi + 0x084);
		vm_offset_t buffer;
		uint32_t words[8];

		printf("display: cmd dma: ctrl %#x, cfg1 %#x (start %u, continue %u, insert DCS %u)\n", dma_ctrl, cfg1,
		    cfg1 & 0xff, cfg1 >> 8 & 0xff, cfg1 >> 16 & 1);
		printf("display: cmd dma: base %#x, length %u, trigger control %#x\n", base_phys, length, trigger);
		if (base_phys == 0 || length == 0 || length > 64 || (base_phys & 3) != 0) {
			printf("display: the command buffer is not one to read (%#x bytes at %#x)\n", length, base_phys);
			goto out;
		}
		buffer = ml_io_map(base_phys & ~0xfffu, 0x1000);
		if (buffer == 0) {
			printf("display: the command buffer at %#x could not be mapped\n", base_phys);
			goto out;
		}
		bzero(words, sizeof(words));
		for (uint32_t i = 0; i < 8; i++) {
			words[i] = rd(buffer + (base_phys & 0xfff) + i * 4);
		}
		printf("display: cmd dma: the bytes there:");
		for (uint32_t i = 0; i < 8; i++) {
			printf(" %08x", words[i]);
		}
		printf("\n");
		/* the same bytes, one at a time, so a header byte and a flag byte are told apart by eye */
		printf("display: cmd dma: as bytes:");
		for (uint32_t i = 0; i < 8; i++) {
			for (uint32_t b = 0; b < 4; b++) {
				printf(" %02x", (words[i] >> (8 * b)) & 0xff);
			}
		}
		printf("\n");
	}
out:
	if (steps < 7) {
		printf("display: stopping here (display_step=%u)\n", steps);
	}
	thread_terminate(current_thread());
}

void
hakios_display_init(void)
{
	uint32_t steps = 0;
	thread_t thread;

	if (!PE_parse_boot_argn("display_step", &steps, sizeof(steps)) || steps == 0) {
		return;
	}
	if (kernel_thread_start(display_thread, (void *)(uintptr_t)steps, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
