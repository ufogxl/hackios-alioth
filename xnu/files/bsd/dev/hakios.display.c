/*
 * The display controller of the Redmi K40 (alioth): what it is doing, read only, step by step.
 * Boot-arg display_step=N performs steps 1 to N and stops; nothing in those steps writes to the
 * controller.
 *
 * Three things here do write, and only when asked for by boot-arg: display_hz=60|90|120 sends the
 * panel the timing-switch sequence its device tree gives for that refresh rate (the command DMA
 * path, see the section before hakios_display_init()), display_bl=1..0x7ff sends the same path the
 * brightness command (the one visible proof that the panel receives these packets; display_bl_ramp
 * sweeps that command up and down for eight seconds, so the proof is a fade and not a step), and
 * display_te=SECONDS counts the panel's own TE pulses, so what the panel actually does is measured
 * and not assumed.
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
#include <mach/vm_param.h>
#include <os/atomic_private.h>
#include <pexpert/pexpert.h>
#include <sys/systm.h>
#include <vm/memory_types.h>
#include <vm/vm_kern.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern vm_offset_t ml_io_map_unmappable(vm_offset_t phys_addr, vm_size_t size, unsigned int flags);
extern uint64_t kvtophys(vm_offset_t va);       /* pmap_paddr_t */
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

static inline void
wr(vm_offset_t address, uint32_t value)
{
	*(volatile uint32_t *)address = value;
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
 * The four register blocks, mapped once for whichever thread asks first. This used to sit inside
 * display_thread, which was harmless while that thread was the only reader; display_hz's thread
 * wants the DSI host as well, and on a run without display_step it never got a mapped one -- the
 * first read through a zero base was a data abort at address 0x48 (DMA_BASE's offset) on core 1.
 */
static bool
display_map(void)
{
	if (dsi != 0) {
		return true;
	}
	mdp = ml_io_map(MDSS_MDP_PHYS, MDSS_MDP_SIZE);
	dsi = ml_io_map(MDSS_DSI0_PHYS, MDSS_DSI0_SIZE);
	phy = ml_io_map(MDSS_DSI0_PHY_PHYS, MDSS_DSI0_PHY_SIZE);
	dcc = ml_io_map(DISP_CC_PHYS, DISP_CC_SIZE);
	if (dsi == 0) {
		printf("display: the DSI host at %#x could not be mapped\n", MDSS_DSI0_PHYS);
		return false;
	}
	return true;
}

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
	if (!display_map()) {
		thread_terminate(current_thread());
		return;
	}
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
	 *   +0x03c CMD_DMA_CTRL 0x14000000   FROM_FRAME_BUFFER (bit 28) | LOW_POWER (bit 26)
	 *   +0x044 CMD_CFG1     0x0013c2c    WR_MEM_START 44, WR_MEM_CONTINUE 60, INSERT_DCS_COMMAND
	 *   +0x048 DMA_BASE     0x9aeb2000   where the command's bytes were
	 *   +0x04c DMA_LEN      8
	 *   +0x084 TRIG_CTRL    4            DMA_TRIGGER = 4, which is TRIGGER_SW: the start is software's
	 *
	 * Those field names and offsets are from Linux's registers/display/dsi.xml (every one of the
	 * values above fits them), and the lesson of the first attempt at sending a command is in
	 * TRIG_CTRL: it configures the trigger *source* and is not what starts a transfer. TRIG_DMA at
	 * +0x090 is the one-shot, one line below.
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

/* ---- how fast the panel itself refreshes: the TE pin, counted ------------------------------------- */

/*
 * The ceiling for every frame this kernel puts in 0x9c000000 is not this kernel's: the panel is in
 * command mode (CTRL.CMD_MODE_EN, see the dump below) and the MDP left by the bootloader pushes a
 * frame to it on each TE pulse the panel raises. So the TE pin's rate IS the panel's refresh rate,
 * and fb_hz (the display driver's own beat, 60 by default) can only aim at it. The phone's device
 * tree has three timings for this panel -- 60, 90 and 120 a second -- and which one the bootloader
 * chose has been a guess until now; this turns it into a number.
 *
 * TE is GPIO 66. 66..68 are on the north tile (the interleaving table in devices/alioth/hardware.md:
 * tiles are not contiguous ranges of pins), and each pin's registers are 0x1000 apart, cfg at +0,
 * the pad's input bit at +4 bit 0 -- the same layout hakios.goodix.c and hakios.haptic.c write.
 * Nothing is written here and the pin is not reconfigured: a pulse a few microseconds wide is not
 * missed because the loop does nothing but read that one word. Boot-arg display_te=SECONDS.
 */
#define TLMM_NORTH_PHYS         0x0f900000UL
#define TLMM_SIZE               0x00100000UL
#define TLMM_GPIO_CFG(n)        (0x1000UL * (n))
#define TLMM_GPIO_IO(n)         (0x1000UL * (n) + 4)
#define PIN_TE                  66

static void
te_thread(void *parameter, __unused wait_result_t result)
{
	uint32_t seconds = (uint32_t)(uintptr_t)parameter;
	uint32_t delay = 0;
	vm_offset_t north = ml_io_map(TLMM_NORTH_PHYS, TLMM_SIZE);
	vm_offset_t cfg, io;
	uint64_t deadline, now;
	uint32_t edges = 0, samples = 0, high = 0, last;

	if (!PE_parse_boot_argn("display_te_delay", &delay, sizeof(delay))) {
		delay = 0;
	}
	if (delay != 0) {
		delay_for_interval(delay, NSEC_PER_SEC);
	}

	if (north == 0) {
		printf("display: TE: the north tile at %#x could not be mapped\n", TLMM_NORTH_PHYS);
		thread_terminate(current_thread());
		return;
	}
	cfg = north + TLMM_GPIO_CFG(PIN_TE);
	io = north + TLMM_GPIO_IO(PIN_TE);
	printf("display: TE: gpio %u at %#lx: cfg %#x (pull %u, function %u, drive %u, output %u), level %u\n", PIN_TE,
	    io, rd(cfg), rd(cfg) & 3, rd(cfg) >> 2 & 0xf, (rd(cfg) >> 6 & 7) + 1, rd(cfg) >> 9 & 1, rd(io) & 1);
	last = rd(io) & 1;
	nanoseconds_to_absolutetime((uint64_t)seconds * NSEC_PER_SEC, &deadline);
	deadline += mach_absolute_time();
	do {
		uint32_t level = rd(io) & 1;

		samples++;
		high += level;
		if (level != 0 && last == 0) {
			edges++;
		}
		last = level;
		now = mach_absolute_time();
	} while (now < deadline);
	printf("display: TE: %u pulses in %u s = %u.%03u a second (from %u samples of gpio %u, %u of them high)\n",
	    edges, seconds, edges / seconds, (edges % seconds) * 1000 / seconds, samples, PIN_TE, high);
	thread_terminate(current_thread());
}

/* ---- sending the panel a command: the DSI host's own command DMA --------------------------------- */

/*
 * The first thing this file ever writes to the display controller, and it is deliberately the
 * smallest write there is: one DCS packet at a time through the command DMA the bootloader itself
 * last used, in the buffer it left behind (DSI +0x048, eight bytes, -- see step 7).
 *
 * Why the refresh rate needs it: the panel is in command mode, so its refresh rate is a property of
 * the PANEL, set by its own timings, and the only way to change it is the DCS sequence the phone's
 * device tree calls qcom,mdss-dsi-timing-switch-command. For this panel (k11a) the device tree has
 * three of them, one per timing -- 60, 90 and 120 a second -- and 120's is the sequence below:
 *
 *   39 00 00 00 00 00 02 60 10      the panel's own "which timing" byte
 *   39 00 00 00 00 00 03 f0 5a 5a   vendor page unlock
 *   39 00 00 00 00 00 03 fc 5a 5a   vendor page unlock, second key
 *   39 00 00 00 00 00 02 b0 16      vendor page
 *   39 00 00 00 00 00 02 d1 2e      the timing's own parameter (90 Hz has 6e here instead)
 *   39 00 00 00 00 00 03 fc a5 a5   vendor page lock
 *   39 01 00 00 00 00 03 f0 a5 a5   vendor page lock, second key (0x01 = last command)
 *
 * The bytes in memory are not the MIPI packet: msm's dsi_cmd_dma_add() rearranges them, and the
 * bootloader's own eight bytes in that buffer showed the rearrangement rather than being guessed
 * (step 7). A short write (two bytes: command, parameter) becomes, from byte 0:
 *
 *   [0] the DCS command   [1] its parameter   [2] 0x15 (DCS short write, one parameter)
 *   [3] 0x80 (last command)   [4..7] 0xff
 *
 * and a long write (three bytes here) becomes:
 *
 *   [0] length low   [1] length high   [2] 0x39 (DCS long write)
 *   [3] 0xc0 (long packet | last command)   [4..] the bytes, padded with 0xff
 *
 * Eight bytes holds every packet the three timing sequences are made of. The bootloader's own
 * buffer was the tempting place to put them and it is not used: that page is not this kernel's to
 * write, and eight bytes dropped into whatever lives there now is a memory corruption that would
 * surface somewhere else entirely. The buffer is one page of our own instead (below), which is
 * also what Linux's dsi_host.c does -- the host reads the command from memory with its own bus
 * master, so the page is mapped uncached and its physical address goes into DMA_BASE.
 * Boot-arg display_hz=60|90|120, and display_hz_delay=SECONDS (default 20) so that a sequence that
 * leaves the panel dark is a run that still gets its log out.
 */
#define DSI_CMD_DMA_CTRL        0x03c
#define CMD_DMA_LOW_POWER       0x04000000      /* bit 26: send in low power instead of high speed */
#define DSI_CMD_CFG1            0x044
#define DSI_DMA_BASE            0x048
#define DSI_DMA_LEN             0x04c
#define DSI_STATUS0             0x008
#define DSI_TRIG_CTRL           0x084
#define DSI_TRIG_DMA            0x090
#define DSI_INTR_CTRL           0x110
#define DSI_ACK_ERR_STATUS      0x068
#define DSI_TIMEOUT_STATUS      0x0c0    /* bit 0 HS TX, bit 4 LP RX, bit 8 BTA: which wait expired */
#define DSI_RDBK_DATA_CTRL      0x1d4
#define DSI_RDBK(n)             (0x06c + 4 * (n))
/*
 * The test-pattern generator's command FIFO, the other way into the panel and the one the
 * reference port uses for both a write and a read. The two differ in where the packet is kept:
 * the command DMA above fetches it from memory through DMA_BASE, while this one is handed the
 * words a DWORD at a time and the engine reads nothing from memory. A read-back needs the bus
 * turned around, which the reference port only ever asks for through this path.
 */
#define DSI_TPG_CTRL            0x15c
#define DSI_TPG_INIT            0x17c
#define DSI_TPG_FIFO_RESET      0x1ec
#define CMD_DMA_EMBEDDED        0x10000000      /* bit 28: take the packet from the FIFO, not memory */
#define READ_BACK               0x20    /* data[3] bit 5: the panel is asked to answer */
#define CMD_DMA_DONE            0x1

static vm_offset_t dsi_buffer_stop;     /* our command buffer, mapped for the host to read */
static uint32_t dsi_buffer_phys;
static uint32_t dsi_buffer_bootloader;  /* the base the bootloader left in the host, put back after */
static uint32_t dsi_buffer_saved[4];    /* its first sixteen bytes, put back after every packet */
static bool dsi_buffer_restore;         /* true when the page is not ours and its bytes matter */
static bool dsi_high_speed;             /* display_hs=1: send in high speed, like the vendor's DCS path */
static bool dsi_quiet;                  /* a ramp sends dozens of packets; its own summary is enough */
static uint32_t dsi_in_flight;          /* the command DMA is one engine: one packet at a time */

/*
 * The command engine's control word, written out in full on every transfer the way the vendor
 * writes it (dsi_ctrl_hw_cmn.c, the kickoff paths): broadcast off, this controller the master,
 * the packet fetched from memory, and the speed following the argument.
 *
 * Bit 28, embedded mode, is not a detail that can be inherited. The bootloader leaves it set on
 * some boots -- 0x14000000, the value the first successful brightness round saw -- and clear on
 * others, where the register reads 0x4000001 and the engine announces a transfer it never made:
 * the packet is not fetched, the panel is never asked, every write comes back with an error packet,
 * and every register still looks healthy. Inheriting it is what made brightness work one night and
 * not the next, with the same code and the same packet.
 *
 * LOW_POWER is the one bit here that stays a boot-arg (display_hs). The bootloader leaves it set;
 * the vendor clears it for a normal DCS write and sets it back afterwards, keeping it only for
 * messages flagged MIPI_DSI_MSG_USE_LPM, of which the timing switch is one. Which of the two this
 * panel listens to is what the experiments say, so it is not fixed here.
 */
static void
dsi_power_mode(bool high_speed)
{
	uint32_t ctrl = rd(dsi + DSI_CMD_DMA_CTRL);

	ctrl &= ~((1u << 31) | (1u << 30));     /* broadcast off, master on */
	ctrl |= (1u << 28);                     /* embedded mode: fetch the packet from memory */
	ctrl &= ~((1u << 24) | (1u << 29));     /* packet type 0, WC_SEL 0 */
	if (high_speed) {
		ctrl &= ~CMD_DMA_LOW_POWER;
	} else {
		ctrl |= CMD_DMA_LOW_POWER;
	}
	wr(dsi + DSI_CMD_DMA_CTRL, ctrl);
}

/*
 * The DSI host reads a command out of memory with its own bus master, so the page it is pointed at
 * has to be one that master can reach -- not merely one this processor can write.
 *
 * That is what the first version got wrong. It took a page of this kernel's own memory
 * (kmem_alloc_contig, as hakios.lx.c and the GPU driver do for their shared memory) and mapped it
 * uncached: every packet then completed with CMD_DMA_DONE, status 0 and ack error 0, and the panel
 * never did anything -- eleven seconds of the brightness ramp, three times, with the screen not
 * changing once. The same packet sent from the page the bootloader had left in DMA_BASE dimmed the
 * screen at once. So the page to send from is the bootloader's: whatever address it was sending
 * from is by construction one the display's DMA master can read, and memory this kernel allocated
 * is not. display_dma_at=ADDR sends from some other address; display_dma_own=1 goes back to a page
 * of our own, the experiment that failed.
 */
static bool
dsi_buffer_ready(void)
{
	vm_offset_t cached = 0, uncached;
	uint64_t physical;
	uint32_t asked = 0, own = 0;
	bool use_own;

	if (dsi_buffer_stop != 0) {
		return true;
	}
	if (!display_map()) {
		return false;
	}
	dsi_buffer_bootloader = rd(dsi + DSI_DMA_BASE);
	PE_parse_boot_argn("display_dma_at", &asked, sizeof(asked));
	use_own = PE_parse_boot_argn("display_dma_own", &own, sizeof(own)) && own != 0;
	if (!use_own) {
		if (asked == 0) {
			asked = dsi_buffer_bootloader;
		}
		if (asked == 0) {
			printf("display: neither display_dma_at nor the bootloader's DMA_BASE says where to send from\n");
			return false;
		}
		uncached = ml_io_map_unmappable((vm_offset_t)asked, PAGE_SIZE, VM_WIMG_WCOMB);
		if (uncached == 0) {
			printf("display: the command page at %#x could not be mapped\n", asked);
			return false;
		}
		dsi_buffer_stop = uncached;
		dsi_buffer_phys = asked;
		for (uint32_t i = 0; i < 4; i++) {
			dsi_buffer_saved[i] = rd(dsi_buffer_stop + 4 * i);
		}
		dsi_buffer_restore = true;
		printf("display: command buffer: %#x, %s; the bootloader had left %#x\n", dsi_buffer_phys,
		    asked == dsi_buffer_bootloader ? "the bootloader's own page, the one the display can reach" :
		    "the address display_dma_at asked for", dsi_buffer_bootloader);
		return true;
	}
	if (kmem_alloc_contig(kernel_map, &cached, PAGE_SIZE, 0, 0, 0,
	    KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		printf("display: no page of memory for the DSI's commands\n");
		return false;
	}
	physical = kvtophys(cached);
	uncached = ml_io_map_unmappable((vm_offset_t)physical, PAGE_SIZE, VM_WIMG_WCOMB);
	if (physical == 0 || uncached == 0) {
		printf("display: the command page at %#llx could not be mapped for the display\n", physical);
		return false;
	}
	dsi_buffer_stop = uncached;
	dsi_buffer_phys = (uint32_t)physical;
	printf("display: command buffer: %#x, a page of our own (display_dma_own); the bootloader had left %#x\n",
	    dsi_buffer_phys, dsi_buffer_bootloader);
	return true;
}

/*
 * One packet, through the command DMA. `bytes[0]` is the DCS command, the rest its parameters.
 *
 * The register writes are the ones Linux's dsi_host.c ends a command with (msm_dsi_host_cmd_xfer_
 * commit: DMA_BASE, DMA_LEN, then a 1 written to TRIG_DMA), and the length is Linux's too: a short
 * write is the four bytes of its header and nothing else, a long write is its header plus payload
 * rounded up to four.
 *
 * TRIG_CTRL is read and not written: it says *which* source starts a transfer, and this display is
 * already configured for the software one (0x4 = TRIGGER_SW in DMA_TRIGGER, bits 0..2 -- the value
 * the bootloader left, it is in the log). TRIG_DMA at +0x90 is the one-shot start, and that is the
 * write the first attempt at this missed: it wrote the bit into TRIG_CTRL instead, which asked the
 * controller for "software and start-of-frame" and started nothing, so the transfer never ran.
 *
 * The transfer is done when CMD_DMA_DONE appears in INTR_CTRL, which is what the vendor's driver
 * waits on too; the register holds status and mask in one word and writing the status bit back
 * clears it.
 */
static bool
dsi_send(const uint8_t *bytes, uint32_t count)
{
	uint8_t packet[8] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
	uint32_t length, waited, trigger;

	if (count < 2 || count > 4 || !dsi_buffer_ready()) {
		return false;
	}
	if (!os_atomic_cmpxchg(&dsi_in_flight, 0, 1, relaxed)) {
		return false;                   /* the timing switch or a fade is using the engine: this one goes */
	}
	if (count == 2) {
		packet[0] = bytes[0];                   /* the command */
		packet[1] = bytes[1];                   /* its one parameter */
		packet[2] = 0x15;                       /* DCS short write, one parameter */
		packet[3] = 0x80;                       /* last command */
		length = 4;                             /* the header is the whole packet */
	} else {
		packet[0] = (uint8_t)count;             /* length, low */
		packet[1] = 0;                          /* length, high */
		packet[2] = 0x39;                       /* DCS long write */
		packet[3] = 0xc0;                       /* long packet | last command */
		memcpy(packet + 4, bytes, count);
		length = (4 + count + 3) & ~3u;         /* header and payload, to four bytes */
	}
	for (uint32_t i = 0; i < length; i++) {
		wr(dsi_buffer_stop + i, packet[i]);
	}
	wr(dsi + DSI_DMA_BASE, dsi_buffer_phys);
	wr(dsi + DSI_DMA_LEN, length);
	__asm__ volatile ("dsb sy" ::: "memory");   /* the host reads these bytes with its own master */
	if ((rd(dsi + DSI_INTR_CTRL) & CMD_DMA_DONE) != 0) {
		wr(dsi + DSI_INTR_CTRL, CMD_DMA_DONE);  /* the last transfer's done bit, cleared */
	}
	trigger = rd(dsi + DSI_TRIG_CTRL);
	dsi_power_mode(dsi_high_speed);
	wr(dsi + DSI_TRIG_DMA, 1);
	for (waited = 0; waited < 1000; waited++) {
		if ((rd(dsi + DSI_INTR_CTRL) & CMD_DMA_DONE) != 0) {
			break;
		}
		delay_for_interval(100, NSEC_PER_USEC);
	}
	if (!dsi_quiet) {
		printf("display: dcs %02x%s: len %u, %u us, intr %#x, status0 %#x, ack err %#x, trig ctrl %#x, dma ctrl %#x\n",
		    bytes[0], count > 2 ? " (long)" : "", length, waited * 100, rd(dsi + DSI_INTR_CTRL),
		    rd(dsi + DSI_STATUS0), rd(dsi + DSI_ACK_ERR_STATUS), trigger, rd(dsi + DSI_CMD_DMA_CTRL));
	}
	dsi_power_mode(false);                  /* back to what the bootloader left: low power */
	wr(dsi + DSI_DMA_BASE, dsi_buffer_bootloader);
	if ((rd(dsi + DSI_INTR_CTRL) & CMD_DMA_DONE) != 0) {
		wr(dsi + DSI_INTR_CTRL, CMD_DMA_DONE);
	}
	if (dsi_buffer_restore) {               /* the bootloader's page, back to what it held */
		for (uint32_t i = 0; i < 4; i++) {
			wr(dsi_buffer_stop + 4 * i, dsi_buffer_saved[i]);
		}
	}
	os_atomic_store(&dsi_in_flight, 0, relaxed);
	return waited < 1000;
}

/*
 * One DCS read: the same command DMA with the bit that asks the panel to answer (data[3] bit 5,
 * dsi_cmd_dma_add's "read back"), and the reply is left in the four RDBK words. A read is the only
 * thing that proves the panel itself is listening -- a write only proves the host put bytes on the
 * wire -- so 0x0a (get power mode) is the command to try it with: its answer is the panel's.
 */
static bool
dsi_read(uint8_t command, uint32_t *reply)
{
	uint32_t waited, ctrl;

	if (!dsi_buffer_ready()) {
		return false;
	}
	if (!os_atomic_cmpxchg(&dsi_in_flight, 0, 1, relaxed)) {
		return false;
	}
	/*
	 * The command path must be idle before a packet goes in. STATUS bit 2 is the command-mode MDP
	 * busy flag, and the vendor waits for it on every transfer (sendPacket's idle loop). Sent
	 * while it is set, the packet meets an engine already running and the controller reports an
	 * error packet instead of a read -- which is what status0 0x5 alongside ack err 0x1000800 said.
	 */
	for (waited = 0; waited < 2000; waited++) {
		if ((rd(dsi + DSI_STATUS0) & 0x04) == 0) {
			break;
		}
		delay_for_interval(2, NSEC_PER_USEC);
	}
	if ((rd(dsi + DSI_STATUS0) & 0x04) != 0) {
		printf("display: dcs read %02x: the command path never went idle (status0 %#x)\n",
		    command, rd(dsi + DSI_STATUS0));
		os_atomic_store(&dsi_in_flight, 0, relaxed);
		return false;
	}
	/*
	 * appends SET_MAXIMUM_RETURN_PACKET_SIZE to the same command buffer and clears that
	 * packet's last-command flag (dsi_ctrl.c, dsi_set_max_return_size: "batch max packet cmd
	 * to read command"), so the engine sends both and only the read ends the transfer. Sent
	 * separately, as this driver first did, the size goes out in one packet and the read in
	 * another with no size in force -- which latches no bytes at all.
	 *
	 * The bytes are laid out the way dsi_ctrl_copy_and_pad_cmd lays a packet out: length in
	 * bytes 0-1, the data type in byte 2, the flags in byte 3 (bit 6 payload present, bit 5 turn
	 * the lane around, bit 7 last command), then the payload, padded with 0xFF to a whole word.
	 * The 0x37 packet is eight bytes, the short read four, and both fit in the sixteen this
	 * driver already saves and puts back.
	 */
	{
		uint8_t packet[12];

		/*
		 * Drop whatever the last read left, after the size is set and before the packet goes
		 * out -- the order dsi_message_rx keeps (dsi_ctrl.c): set_max_return_size, then
		 * clear_rdbk_register, then the read itself.
		 */
		wr(dsi + DSI_RDBK_DATA_CTRL, 1);
		wr(dsi + DSI_RDBK_DATA_CTRL, 0);
		__asm__ volatile ("dsb sy" ::: "memory");
		packet[0] = 0x02;               /* the 0x37 packet carries two bytes */
		packet[1] = 0x00;
		packet[2] = 0x37;               /* SET_MAXIMUM_RETURN_PACKET_SIZE */
		packet[3] = 0x40;               /* long packet only: bit 7 would end the transfer here */
		packet[4] = 0x02;               /* size, low byte first (tx[2] = {size, size >> 8}) */
		packet[5] = 0x00;
		packet[6] = 0xff;               /* the pad a short packet gets */
		packet[7] = 0xff;
		packet[8] = command;            /* the DCS read itself */
		packet[9] = 0x00;
		packet[10] = 0x06;              /* DCS read; 0x05 is a short write and asks nothing */
		packet[11] = 0x80 | READ_BACK;  /* last command, and turn the lane around */

		for (uint32_t i = 0; i < 3; i++) {
			wr(dsi_buffer_stop + 4 * i,
			    (uint32_t)packet[4 * i] | (uint32_t)packet[4 * i + 1] << 8 |
			    (uint32_t)packet[4 * i + 2] << 16 | (uint32_t)packet[4 * i + 3] << 24);
		}
	}
	wr(dsi + DSI_DMA_LEN, 12);
	__asm__ volatile ("dsb sy" ::: "memory");
	if ((rd(dsi + DSI_INTR_CTRL) & CMD_DMA_DONE) != 0) {
		wr(dsi + DSI_INTR_CTRL, CMD_DMA_DONE);
	}
	dsi_power_mode(dsi_high_speed);
	wr(dsi + DSI_TRIG_DMA, 1);
	for (waited = 0; waited < 1000; waited++) {
		if ((rd(dsi + DSI_INTR_CTRL) & CMD_DMA_DONE) != 0) {
			break;
		}
		delay_for_interval(100, NSEC_PER_USEC);
	}
	/*
	 * The answer comes back after a bus turnaround, so CMD_DMA_DONE is not yet the reply: wait for
	 * the read-back count to reach the data instead of reading the words the moment the command is
	 * out (which is what left them all zero the first time).
	 */
	for (uint32_t tries = 0; tries < 100; tries++) {
		if (((rd(dsi + DSI_RDBK_DATA_CTRL) >> 16) & 0xff) != 0) {
			break;
		}
		delay_for_interval(100, NSEC_PER_USEC);
	}
	ctrl = rd(dsi + DSI_RDBK_DATA_CTRL);
	reply[0] = rd(dsi + DSI_RDBK(0));
	reply[1] = rd(dsi + DSI_RDBK(1));
	reply[2] = rd(dsi + DSI_RDBK(2));
	reply[3] = rd(dsi + DSI_RDBK(3));
	printf("display: dcs read %02x: %u us, intr %#x, status0 %#x, ack err %#x, timeout %#x, rdbk ctrl %#x, data %08x %08x %08x %08x\n",
	    command, waited * 100, rd(dsi + DSI_INTR_CTRL), rd(dsi + DSI_STATUS0), rd(dsi + DSI_ACK_ERR_STATUS),
	    rd(dsi + DSI_TIMEOUT_STATUS), ctrl, reply[0], reply[1], reply[2], reply[3]);
	dsi_power_mode(false);
	wr(dsi + DSI_DMA_BASE, dsi_buffer_bootloader);
	if ((rd(dsi + DSI_INTR_CTRL) & CMD_DMA_DONE) != 0) {
		wr(dsi + DSI_INTR_CTRL, CMD_DMA_DONE);
	}
	if (dsi_buffer_restore) {
		for (uint32_t i = 0; i < 4; i++) {
			wr(dsi_buffer_stop + 4 * i, dsi_buffer_saved[i]);
		}
	}
	os_atomic_store(&dsi_in_flight, 0, relaxed);
	return waited < 1000;
}

/*
 * DCS 0x52 the way the reference port reads it: through the test-pattern generator command FIFO
 * rather than the command DMA, so a read that comes back empty through the DMA is retried here.
 *
 * Measured on this phone: the transfer completes (intr 0x100000, status0 0) and the byte count in
 * RDBK_DATA_CTRL is still 0, so this path does not receive an answer either. It is kept because it
 * is the one the reference port uses, and the two paths failing the same way is what says the fault
 * is not in the transport -- a panel that answers neither has not been made to answer yet, rather
 * than having been asked wrongly twice.
 *
 *
 * The sequence is BacklightProbe.cpp sendPacket: wait for the command path to idle, arm the
 * generator for FIFO mode, hand it the words, ask the DMA engine to take them from the FIFO
 * (CMD_DMA_EMBEDDED) instead of memory, trigger, wait for the done bit, then put the generator
 * back. An odd word count needs one more zero because the FIFO works in pairs.
 *
 * Returns the four RDBK words as the DMA read leaves them, or false when the transfer did not
 * complete. It does not decode: the caller already knows how, and the raw words are worth seeing.
 */
static bool
dsi_tpg_read(uint8_t command, uint32_t *reply)
{
	uint32_t words[2];
	uint32_t waited, ctrl, rdbk, dma_ctrl_saved;
	bool done = false;

	if (!dsi_buffer_ready()) {
		return false;
	}
	if (!os_atomic_cmpxchg(&dsi_in_flight, 0, 1, relaxed)) {
		return false;
	}
	for (waited = 0; waited < 2000; waited++) {             /* the command path must be idle */
		if ((rd(dsi + DSI_STATUS0) & 0x04) == 0) {       /* bit 2: command-mode MDP busy */
			break;
		}
		delay_for_interval(2, NSEC_PER_USEC);
	}
	wr(dsi + DSI_RDBK_DATA_CTRL, 1);                        /* drop whatever the last read left */
	wr(dsi + DSI_INTR_CTRL, (rd(dsi + DSI_INTR_CTRL) & ~0x10100000u) | CMD_DMA_DONE);
	wr(dsi + DSI_TPG_CTRL, (1u << 1) | (1u << 2) | (3u << 16));
	/* A DCS read is one header word: 0xA0060000 | command (BTA, last, type 0x06). */
	words[0] = 0xA0060000u | (uint32_t)command;
	words[1] = 0;
	wr(dsi + DSI_TPG_INIT, words[0]);
	wr(dsi + DSI_TPG_INIT, words[1]);
	dma_ctrl_saved = rd(dsi + DSI_CMD_DMA_CTRL);   /* exactly what to put back afterwards */
	ctrl = dma_ctrl_saved & ~((1u << 31) | (1u << 30));
	wr(dsi + DSI_CMD_DMA_CTRL, ctrl | CMD_DMA_EMBEDDED);
	wr(dsi + DSI_DMA_LEN, 8);
	wr(dsi + DSI_TRIG_DMA, 1);
	for (waited = 0; waited < 2500; waited++) {
		if ((rd(dsi + DSI_INTR_CTRL) & CMD_DMA_DONE) != 0) {
			done = true;
			break;
		}
		delay_for_interval(20, NSEC_PER_USEC);
	}
	wr(dsi + DSI_INTR_CTRL, (rd(dsi + DSI_INTR_CTRL) & ~0x10100000u) | CMD_DMA_DONE);
	wr(dsi + DSI_TPG_CTRL, 0);
	wr(dsi + DSI_TPG_FIFO_RESET, 1);
	delay_for_interval(1, NSEC_PER_USEC);
	wr(dsi + DSI_TPG_FIFO_RESET, 0);
	delay_for_interval(2000, NSEC_PER_USEC);                /* the answer arrives after the turnaround */
	rdbk = rd(dsi + DSI_RDBK_DATA_CTRL);
	reply[0] = rd(dsi + DSI_RDBK(0));
	reply[1] = rd(dsi + DSI_RDBK(1));
	reply[2] = rd(dsi + DSI_RDBK(2));
	reply[3] = rd(dsi + DSI_RDBK(3));
	printf("display: dcs 52 via tpg: %s, intr %#x, status0 %#x, rdbk ctrl %#x, data %08x %08x %08x %08x\n",
	    done ? "done" : "no done bit", rd(dsi + DSI_INTR_CTRL), rd(dsi + DSI_STATUS0), rdbk,
	    reply[0], reply[1], reply[2], reply[3]);
	/*
	 * Put CMD_DMA_CTRL back to exactly what it was, from the value saved before the embedded
	 * bit went in. It used to be restored from the variable that then held RDBK_DATA_CTRL, so
	 * every read left the command engine programmed with a read-back register's contents --
	 * dma 0x4000001 instead of 0x14000000 -- and every brightness write after it failed with an
	 * error packet.
	 */
	wr(dsi + DSI_CMD_DMA_CTRL, dma_ctrl_saved);
	wr(dsi + DSI_DMA_BASE, dsi_buffer_bootloader);
	if (dsi_buffer_restore) {
		for (uint32_t i = 0; i < 4; i++) {
			wr(dsi_buffer_stop + 4 * i, dsi_buffer_saved[i]);
		}
	}
	os_atomic_store(&dsi_in_flight, 0, relaxed);
	return done && (ctrl >> 16) != 0;
}

/*
 * What the panel says its brightness is, after a level has been written to it.
 *
 * A write only proves the host put bytes on the wire; DCS 0x52 is the command that proves the
 * panel took them. Its answer is the level in the same two bytes 0x51 was given -- big-endian on
 * this panel (mi,mdss-dsi-bl-dcs-big-endian-type), 0x01..0x7ff -- and anything outside that range
 * is the panel refusing the command, not a level to believe.
 *
 * The reply lands in the first RDBK word rather than being spread over the four, so the level is
 * read out of it by whichever of the two plausible layouts puts a number inside the range: high
 * half for a two-byte answer, top byte for a one-byte answer. The four words are printed as they
 * came, so a panel that answers in some third layout can be recognised from the log instead of
 * being silently mis-read.
 *
 * Returns the level the panel reported, or UINT32_MAX if it did not report one.
 */
static uint32_t
dsi_brightness_readback(uint32_t written)
{
	uint32_t reply[4] = { 0, 0, 0, 0 };
	uint32_t level, swapped;

	/*
	 * Two ways in, tried in the order of what is known to work. The command DMA is what this tree
	 * already sends the timing switch through, so it goes first; when it completes and latches no
	 * bytes, the test-pattern FIFO is asked, because that is the path the reference port reads
	 * through and the only one on record to have received an answer from this panel.
	 */
	if (!dsi_read(0x52, reply) || ((rd(dsi + DSI_RDBK_DATA_CTRL) >> 16) & 0xff) == 0) {
		uint32_t again[4] = { 0, 0, 0, 0 };

		if (dsi_tpg_read(0x52, again)) {
			memcpy(reply, again, sizeof(reply));
		}
	}
	if (((rd(dsi + DSI_RDBK_DATA_CTRL) >> 16) & 0xff) == 0 &&
	    reply[0] == 0 && reply[1] == 0 && reply[2] == 0 && reply[3] == 0) {
		printf("display: brightness %u: no answer on either path\n", written);
		return UINT32_MAX;
	}
	/*
	 * The controller latches the answer highest word first and byte-swaps it
	 * (dsi_ctrl_hw_cmn.c get_cmd_read_data), so a two-byte level is reply[0] low two bytes, high
	 * byte first. The other layouts are tried only when that one lands outside the panels declared
	 * range, so a reply in a layout this was not written for is still read rather than called a
	 * refusal -- and the raw words are printed either way.
	 */
	swapped = __builtin_bswap32(reply[0]);
	level = swapped & 0xffff;
	if (level < 0x01 || level > 0x7ff) {
		level = (swapped >> 16) & 0xffff;
	}
	if (level < 0x01 || level > 0x7ff) {
		level = reply[0] >> 16;
	}
	if (level < 0x01 || level > 0x7ff) {
		level = reply[0] >> 24;
	}
	if (level < 0x01 || level > 0x7ff) {
		printf("display: brightness %u: dcs 52 answered %08x %08x %08x %08x, no level in it\n",
		    written, reply[0], reply[1], reply[2], reply[3]);
		return UINT32_MAX;
	}
	printf("display: brightness %u: the panel reads back %u%s\n", written, level,
	    level == written ? "" : " (not what was written)");
	return level;
}

/* The three timing-switch sequences of this panel, from the phone's own device tree (k11a). */
struct dcs_packet {
	uint8_t count;
	uint8_t bytes[3];
};

#define RAMP_STEP_US            40000   /* display_bl_ramp: 40 ms a step, about 1.3 s a direction */

static const struct dcs_packet timing_60[] = {
	{ 2, { 0x60, 0x00 } }, { 3, { 0xf0, 0x5a, 0x5a } }, { 3, { 0xfc, 0x5a, 0x5a } },
	{ 2, { 0xb0, 0x16 } }, { 2, { 0xd1, 0x2e } }, { 3, { 0xfc, 0xa5, 0xa5 } },
	{ 3, { 0xf0, 0xa5, 0xa5 } },
};

static const struct dcs_packet timing_90[] = {
	{ 2, { 0x60, 0x10 } }, { 3, { 0xf0, 0x5a, 0x5a } }, { 3, { 0xfc, 0x5a, 0x5a } },
	{ 2, { 0xb0, 0x16 } }, { 2, { 0xd1, 0x6e } }, { 3, { 0xfc, 0xa5, 0xa5 } },
	{ 3, { 0xf0, 0xa5, 0xa5 } },
};

static const struct dcs_packet timing_120[] = {
	{ 2, { 0x60, 0x10 } }, { 3, { 0xf0, 0x5a, 0x5a } }, { 3, { 0xfc, 0x5a, 0x5a } },
	{ 2, { 0xb0, 0x16 } }, { 2, { 0xd1, 0x2e } }, { 3, { 0xfc, 0xa5, 0xa5 } },
	{ 3, { 0xf0, 0xa5, 0xa5 } },
};

static void
hz_thread(void *parameter, __unused wait_result_t result)
{
	uint32_t hz = (uint32_t)(uintptr_t)parameter;
	uint32_t delay = 20, packets, brightness = 0, reading = 0, ramp = 0;
	const struct dcs_packet *sequence;

	PE_parse_boot_argn("display_hz_delay", &delay, sizeof(delay));
	delay_for_interval(delay, NSEC_PER_SEC);

	if (hz == 60) {
		sequence = timing_60;
		packets = sizeof(timing_60) / sizeof(timing_60[0]);
	} else if (hz == 90) {
		sequence = timing_90;
		packets = sizeof(timing_90) / sizeof(timing_90[0]);
	} else if (hz == 120) {
		sequence = timing_120;
		packets = sizeof(timing_120) / sizeof(timing_120[0]);
	} else {
		sequence = NULL;                        /* display_bl on its own: no timing sequence */
		packets = 0;
	}
	if (packets != 0) {
		printf("display: asking the panel for %u a second: %u packets, each one a DCS write\n", hz, packets);
		for (uint32_t i = 0; i < packets; i++) {
			if (!dsi_send(sequence[i].bytes, sequence[i].count)) {
				printf("display: the panel did not take packet %u of the %u sequence\n", i, hz);
				break;
			}
			delay_for_interval(200, NSEC_PER_USEC);
		}
		printf("display: the panel was asked for %u a second (display_te counts what it now does)\n", hz);
	}
	/*
	 * The panel's backlight is a DCS command on this phone (qcom,mdss-dsi-bl-pmic-control-type =
	 * "bl_ctrl_dcs"), so brightness is the one effect of the command path that can be seen without
	 * trusting a register: if the screen changes brightness, the panel is receiving these packets.
	 * Big-endian (mi,mdss-dsi-bl-dcs-big-endian-type), max 0x7ff.
	 */
	if (PE_parse_boot_argn("display_bl", &brightness, sizeof(brightness)) && brightness != 0 && brightness <= 0x7ff) {
		uint8_t level[3] = { 0x51, (uint8_t)(brightness >> 8), (uint8_t)(brightness & 0xff) };
		printf("display: brightness %u: dcs 51 %02x %02x\n", brightness, level[1], level[2]);
		dsi_send(level, 3);
		dsi_brightness_readback(brightness);
	}
	/*
	 * display_bl_ramp: the same command, swept from the top of the range down and back up, three
	 * times. One step is easy to miss while the phone is still showing iOS and the loader paints the
	 * screen over a few seconds later; eight seconds of fading is not.
	 */
	if (PE_parse_boot_argn("display_bl_ramp", &ramp, sizeof(ramp)) && ramp != 0) {
		const int top = 0x7ff, bottom = 0x20, stride = 0x40;
		uint8_t fade[3] = { 0x51, 0, 0 };

		printf("display: brightness ramp %#x -> %#x -> %#x, three times, %u us a step\n",
		    top, bottom, top, RAMP_STEP_US);
		dsi_quiet = true;                       /* sixty-odd packets; one summary is enough */
		for (int cycle = 0; cycle < 3; cycle++) {
			for (int value = top; value >= bottom; value -= stride) {
				fade[1] = (uint8_t)(value >> 8);
				fade[2] = (uint8_t)(value & 0xff);
				dsi_send(fade, 3);
				delay_for_interval(RAMP_STEP_US, NSEC_PER_USEC);
			}
			for (int value = bottom; value < top; value += stride) {
				fade[1] = (uint8_t)(value >> 8);
				fade[2] = (uint8_t)(value & 0xff);
				dsi_send(fade, 3);
				delay_for_interval(RAMP_STEP_US, NSEC_PER_USEC);
			}
		}
		dsi_quiet = false;
		printf("display: brightness ramp done, left at the top (%#x)\n", top);
	}
	if (PE_parse_boot_argn("display_read", &reading, sizeof(reading)) && reading != 0 && reading <= 0xff) {
		uint32_t reply[4];
		printf("display: asking the panel to read %02x\n", reading);
		dsi_read((uint8_t)reading, reply);
	}
	thread_terminate(current_thread());
}

/*
 * The panel's brightness, as the backlight service's user client asks for it
 * (iokit/Kernel/MiniAppleBacklight.cpp). k11a's backlight is DCS 0x51 over the same command DMA the
 * timing switch uses (bl_ctrl_dcs, big-endian, 0..0x7ff), so a level is one three-byte long write.
 *
 * One send at a time: the command DMA is one engine and the timing-switch thread uses it as well,
 * so a request that arrives while another transfer is in flight is dropped -- the window server
 * sends a whole fade, and the level after the dropped one comes a millisecond later anyway.
 *
 * The log line is only for a change of a sixteenth of the range or more: a fade is dozens of
 * levels, and every one of them in the log would bury what the rest of the run says.
 */
void
hakios_display_set_brightness(uint32_t level)
{
	static uint32_t last_level = 0xffffffff;
	uint8_t bytes[3];

	if (level > 0x7ff) {
		level = 0x7ff;
	}
	if (level == last_level) {
		return;
	}
	if (last_level == 0xffffffff || level > last_level + 0x40 || level + 0x40 < last_level) {
		printf("display: brightness %u: dcs 51 %02x %02x\n", level, (uint8_t)(level >> 8), (uint8_t)(level & 0xff));
	}
	last_level = level;
	bytes[0] = 0x51;
	bytes[1] = (uint8_t)(level >> 8);
	bytes[2] = (uint8_t)(level & 0xff);
	dsi_quiet = true;
	dsi_send(bytes, 3);
	dsi_quiet = false;
	dsi_brightness_readback(level);
}

void
hakios_display_init(void)
{
	uint32_t steps = 0, te = 0, hz = 0, bl = 0, rd = 0, hs = 0, ramp = 0;
	thread_t thread;

	if ((PE_parse_boot_argn("display_hz", &hz, sizeof(hz)) && (hz == 60 || hz == 90 || hz == 120)) ||
	    PE_parse_boot_argn("display_bl", &bl, sizeof(bl)) ||
	    PE_parse_boot_argn("display_bl_ramp", &ramp, sizeof(ramp)) ||
	    PE_parse_boot_argn("display_read", &rd, sizeof(rd))) {
		if (PE_parse_boot_argn("display_hs", &hs, sizeof(hs)) && hs != 0) {
			dsi_high_speed = true;
		}
		if (kernel_thread_start(hz_thread, (void *)(uintptr_t)((hz == 60 || hz == 90 || hz == 120) ? hz : 0), &thread) ==
		    KERN_SUCCESS) {
			thread_deallocate(thread);
		}
	}

	if (PE_parse_boot_argn("display_te", &te, sizeof(te)) && te >= 1 && te <= 30) {
		if (kernel_thread_start(te_thread, (void *)(uintptr_t)te, &thread) == KERN_SUCCESS) {
			thread_deallocate(thread);
		}
	}
	if (!PE_parse_boot_argn("display_step", &steps, sizeof(steps)) || steps == 0) {
		return;
	}
	if (kernel_thread_start(display_thread, (void *)(uintptr_t)steps, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
