/*
 * The Redmi K40's amplifiers (alioth): find them on their I2C bus, and say what answers.
 * Boot-arg amp_step=N performs steps 1 to N and stops. Nothing here plays a sound.
 *
 * Why: this phone's speakers are not driven the way redwood's are. Redwood has one AW88261 per
 * speaker on a pin-driven path; alioth has two Cirrus CS35L41 parts behind an I2C bus --
 *
 *   i2c@98c000   cs35l41@40   the receiver's,  reset-gpios = <&tlmm 114 0>
 *                cs35l41@41   the speaker's,   same reset line, cirrus,right-channel-amp
 *
 * and their audio arrives over the tertiary TDM interface (pins 133/134/135/137, "mi2s2"), which
 * the audio DSP runs. So before any of the sound path can be attempted, the two chips have to be
 * talked to at all: they are I2C devices, this kernel had no I2C, and the bus they sit on
 * (QUPv3 wrapper 0, serial engine 3) is one of the blocks the touch screen's driver already
 * proved reachable -- same wrapper, same clock gate block, engine 4 instead of 3.
 *
 * The bus is driven here by pins, not by the serial engine. The haptic driver (hakios.haptic.c)
 * already bit-bangs 100 kHz I2C on this SoC through the TLMM and it works on the real phone; the
 * serial engine asks for a clock tree to be programmed correctly before the first byte, and a
 * wrong divider or a gate left off is a hang, not an error. A pin that is driven wrongly asks a
 * chip that is not there for its name and gets nothing back, which is the failure this file can
 * afford. The engine's registers are still read in step 1, so its state is known -- if it turns
 * out to be holding the bus, the log says so instead of two masters fighting.
 *
 * Pin numbers and the chip's identity come from the phone's own device tree
 * (dump/twrp-fdt.dts, i2c@98c000 and its two cs35l41 children); the register map and the byte
 * order come from Linux's cs35l41-regs.h and cs35l41-lib.c: 32-bit register addresses, 32-bit
 * values, both big-endian, DEVID at 0x00000000 reading 0x00035a40.
 *
 *   amp_step=1  map TLMM north, the clock controller and serial engine 3; read -- and only read
 *               -- the two bus pins' and the reset pin's configuration and level, the engine's
 *               firmware protocol, and whether its clocks are on. Nothing is written.
 *   amp_step=2  ... and the bus, if and only if step 1 found both pins in GPIO mode and at rest:
 *               the address bytes of DEVID and REVID are sent to 0x40 and 0x41, in both pin
 *               orders (the device tree lists the two pins but not which is which), and whatever
 *               answers is reported. The pins are put back exactly as they were found.
 *   amp_step=3  ... including taking the pins from the serial engine if it is idle: step 1's
 *               engine status must say no command is running, otherwise the step refuses.
 *   amp_step=4  ... and the reset line: driven low for the chip's 2 ms minimum, released, then
 *               five milliseconds of boot time, then the same reads again.
 *
 * The first two steps write nothing at all; the third writes the two pins' multiplexing and puts
 * it back; the fourth also drives the amplifiers' reset. That is the whole risk of this file, and
 * it is why the steps stop where they do rather than going on to configure the chip.
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

static inline void
wr(vm_offset_t address, uint32_t value)
{
	*(volatile uint32_t *)address = value;
}

/* ---- the blocks this file touches, all of them from the phone's device tree ---------------------- */

#define TLMM_NORTH_PHYS         0x0f900000UL    /* pins 114, 119 and 120 are all on the north tile */
#define TLMM_SIZE               0x00100000UL
#define GCC_PHYS                0x00100000UL
#define GCC_SIZE                0x00060000UL
#define GENI_SE3_PHYS           0x0098c000UL    /* i2c@98c000: wrapper 0, serial engine 3 */
#define GENI_SE3_SIZE           0x00004000UL

/* TLMM: one 4 KB block per pin, [1:0] pull, [5:2] function, [8:6] drive, [9] output, +4 the level */
#define GPIO_CFG(n)             (tlmm + 0x1000UL * (n))
#define GPIO_IO(n)              (GPIO_CFG(n) + 4)
#define CFG_PULL_UP             3u
#define CFG_OUTPUT              (1u << 9)
#define IO_OUT                  (1u << 1)       /* bit 0 is the level in, bit 1 the level out */

/* serial engine 3's clock generator: its command register is the anchor for the others */
#define GCC_SE3_CBCR            (gcc + 0x1739c) /* bit 31: the clock is off */
#define GCC_SE3_CFG             (gcc + 0x173a4) /* [4:0] divider, [10:8] source, [13:12] mode */
#define GCC_VOTE                (gcc + 0x52008) /* the application processors' votes */
#define VOTE_SE3                (1u << 13)

/* the serial engine's own registers, the general half (Linux: include/linux/soc/qcom/geni-se.h) */
#define GENI_STATUS             (se + 0x040)    /* bit 0: the primary sequencer has a command */
#define GENI_SER_M_CLK_CFG      (se + 0x048)
#define GENI_IF_DISABLE_RO      (se + 0x064)
#define GENI_FW_REVISION_RO     (se + 0x068)    /* [15:8] protocol: 3 = I2C */
#define SE_HW_PARAM_0           (se + 0xe24)    /* [21:16] FIFO depth in words */

#define PIN_RESET               114             /* both amplifiers: reset-gpios = <&tlmm 114 0> */
#define PIN_A                   119
#define PIN_B                   120

/*
 * The four pins the amplifiers' audio arrives on (dt: pinctrl "tert_tdm_*", function mi2s2_*):
 * 133 bit clock, 135 frame sync, 134 data in, 137 data out. They are on SM8250's *west* tile, which
 * this kernel has never touched -- the north one holds the touch screen's SPI pins and the bus
 * mapped above (8..11, 114, 119, 120) and the south one the haptic's (60, 61).
 */
#define PIN_TDM_CLK             133
#define PIN_TDM_SYNC            135
#define PIN_TDM_DIN             134
#define PIN_TDM_DOUT            137
#define TLMM_WEST_PHYS          0x0f100000UL

#define AMP_RECEIVER            0x40
#define AMP_SPEAKER             0x41
#define AMP_DEVID               0x00000000UL    /* 32-bit address, 32-bit value, big-endian */
#define AMP_REVID               0x00000004UL
#define AMP_DEVID_VALUE         0x00035a40UL

/* the status words the chip's own driver polls after reset (Linux: cs35l41-regs.h) */
#define AMP_IRQ1_STATUS1        0x00010010UL
#define AMP_IRQ1_STATUS2        0x00010014UL
#define AMP_IRQ1_STATUS3        0x00010018UL
#define AMP_IRQ1_STATUS4        0x0001001CUL
#define AMP_IRQ1_STATUS4_BOOT   (1u << 1)       /* OTP_BOOT_DONE */
#define AMP_IRQ1_STATUS3_ERR    (1u << 31)      /* OTP_BOOT_ERR */
#define AMP_PWRMGT_STS          0x00002908UL

/* the analog gain: a 5-bit field at bit 5, 0.5 dB at zero rising 1 dB a step (Linux: amp_gain_tlv) */
#define AMP_GAIN_CTRL           0x00006C04UL
#define AMP_GAIN_PCM_MASK       (0x1fu << 5)
#define AMP_GAIN_PCM_SHIFT      5

/*
 * The boost converter, configured to this phone's parts but never switched on here. The numbers are
 * Linux's cs35l41_boost_config() applied to the device tree's own values:
 *   cirrus,boost-ind-nanohenry = 0x3e8 (1000 nH)  -> bst_lbst_val 0
 *   cirrus,boost-cap-microfarad = 0x0f (15 uF)    -> bst_cbst_range 0
 *   cirrus,boost-peak-milliamp = 0xfa0 (4000 mA)  -> bst_ipk_scaled 0x40
 * and its tables at [0][0]: K1 0x24, K2 0x24, slope 0x75.
 */
#define AMP_BST_COEFF           0x00003810UL    /* [7:0] K1, [15:8] K2 */
#define AMP_BST_SLOPE_LBST      0x00003814UL    /* [7:0] inductor value, [15:8] slope */
#define AMP_BST_PEAK_CUR        0x00003808UL    /* [6:0] peak current, (mA - 1600) / 50 + 0x10 */
#define AMP_BST_COEFF_VALUE     0x00002424UL
#define AMP_BST_SLOPE_VALUE     0x00007500UL
#define AMP_BST_PEAK_VALUE      0x00000040UL

static vm_offset_t tlmm, gcc, se;

static void
spin_us(uint32_t us)
{
	uint64_t until;

	nanoseconds_to_absolutetime((uint64_t)us * NSEC_PER_USEC, &until);
	until += mach_absolute_time();
	while (mach_absolute_time() < until) {
	}
}

/* ---- pins --------------------------------------------------------------------------------------- */

static uint32_t saved_cfg[3], saved_io[3];      /* reset, A, B, as step 1 found them */

/* [5:2] of the configuration word is the pin's function; 0 is plain GPIO, 1 is qup3 (this bus). */
static uint32_t
function_of(uint32_t cfg)
{
	return cfg >> 2 & 0xf;
}

static uint32_t
level_of(unsigned pin)
{
	return rd(GPIO_IO(pin)) & 1;
}

/*
 * One bus line: low is the pin driving it, high is the pin letting go and the pull-up doing the
 * work. That is the same open-drain arrangement the haptic driver uses, and the reason the level
 * can be read back: a line the chip holds stretched still reads low after the pin lets go.
 */
static void
bus_line(unsigned pin, bool high)
{
	wr(GPIO_IO(pin), 0);
	wr(GPIO_CFG(pin), CFG_PULL_UP | (high ? 0 : CFG_OUTPUT));
}

static bool
i2c_start(unsigned sda, unsigned scl)
{
	bus_line(sda, true);
	bus_line(scl, true);
	spin_us(5);
	if (!level_of(sda) || !level_of(scl)) {
		return false;                   /* not at rest: somebody else is talking */
	}
	bus_line(sda, false);
	spin_us(5);
	bus_line(scl, false);
	spin_us(5);
	return true;
}

static void
i2c_stop(unsigned sda, unsigned scl)
{
	bus_line(sda, false);
	spin_us(5);
	bus_line(scl, true);
	spin_us(5);
	bus_line(sda, true);
	spin_us(5);
}

static bool
i2c_bit(unsigned sda, unsigned scl, bool out)
{
	bool in;

	bus_line(sda, out);
	spin_us(5);
	bus_line(scl, true);
	for (unsigned stretched = 0; !level_of(scl) && stretched < 100; stretched++) {
		spin_us(5);
	}
	spin_us(5);
	in = level_of(sda);
	bus_line(scl, false);
	return in;
}

static bool
i2c_write(unsigned sda, unsigned scl, uint8_t byte)
{
	for (int bit = 7; bit >= 0; bit--) {
		i2c_bit(sda, scl, (byte >> bit) & 1);
	}
	return !i2c_bit(sda, scl, true);        /* the ninth clock: the chip pulling low is its ack */
}

static uint8_t
i2c_read(unsigned sda, unsigned scl, bool more)
{
	uint8_t byte = 0;

	for (int bit = 7; bit >= 0; bit--) {
		byte = (uint8_t)(byte << 1 | i2c_bit(sda, scl, true));
	}
	i2c_bit(sda, scl, !more);
	return byte;
}

/*
 * One register read: address first as four big-endian bytes, a repeated start, then the index,
 * then four bytes back. A CS35L41 has 32-bit register addresses (Linux's reg_bits = 32,
 * reg_stride = 4) -- this is not a one-byte-register part, and reading it as one gets nonsense
 * from a chip that is answering perfectly well.
 */
static bool
amp_read(unsigned sda, unsigned scl, uint8_t address, uint32_t index, uint32_t *value)
{
	uint8_t bytes[4] = { 0, 0, 0, 0 };
	bool answered;

	if (!i2c_start(sda, scl)) {
		return false;
	}
	answered = i2c_write(sda, scl, (uint8_t)(address << 1)) &&
	    i2c_write(sda, scl, (uint8_t)(index >> 24)) &&
	    i2c_write(sda, scl, (uint8_t)(index >> 16)) &&
	    i2c_write(sda, scl, (uint8_t)(index >> 8)) &&
	    i2c_write(sda, scl, (uint8_t)index);
	if (answered) {
		i2c_start(sda, scl);
		answered = i2c_write(sda, scl, (uint8_t)(address << 1 | 1));
	}
	for (unsigned i = 0; answered && i < 4; i++) {
		bytes[i] = i2c_read(sda, scl, i < 3);
	}
	i2c_stop(sda, scl);
	if (answered) {
		*value = (uint32_t)bytes[0] << 24 | (uint32_t)bytes[1] << 16 |
		    (uint32_t)bytes[2] << 8 | bytes[3];
	}
	return answered;
}

/*
 * One register write: the same four address bytes, a stop instead of a repeated start, then the
 * four value bytes. A CS35L41's register is 32 bits of address and 32 bits of value, big-endian,
 * like its read side -- this is not a one-byte part.
 */
static bool
amp_write(unsigned sda, unsigned scl, uint8_t address, uint32_t index, uint32_t value)
{
	bool answered;

	if (!i2c_start(sda, scl)) {
		return false;
	}
	answered = i2c_write(sda, scl, (uint8_t)(address << 1)) &&
	    i2c_write(sda, scl, (uint8_t)(index >> 24)) &&
	    i2c_write(sda, scl, (uint8_t)(index >> 16)) &&
	    i2c_write(sda, scl, (uint8_t)(index >> 8)) &&
	    i2c_write(sda, scl, (uint8_t)index) &&
	    i2c_write(sda, scl, (uint8_t)(value >> 24)) &&
	    i2c_write(sda, scl, (uint8_t)(value >> 16)) &&
	    i2c_write(sda, scl, (uint8_t)(value >> 8)) &&
	    i2c_write(sda, scl, (uint8_t)value);
	i2c_stop(sda, scl);
	return answered;
}

/* Both pin orders, because the device tree lists the two pins but does not say which is which. */
static bool
amp_probe_order(unsigned sda, unsigned scl)
{
	uint32_t devid = 0, revid = 0;

	if (!amp_read(sda, scl, AMP_RECEIVER, AMP_DEVID, &devid)) {
		printf("amp: %u/%u: nothing answers 0x%02x (SDA %u, SCL %u)\n",
		    sda, scl, AMP_RECEIVER, sda, scl);
		return false;
	}
	printf("amp: %u/%u: 0x%02x answers, DEVID %#08x%s\n", sda, scl, AMP_RECEIVER, devid,
	    devid == AMP_DEVID_VALUE ? " (a CS35L41)" : " (not the expected 0x00035a40)");
	if (amp_read(sda, scl, AMP_RECEIVER, AMP_REVID, &revid)) {
		printf("amp: %u/%u: 0x%02x REVID %#08x\n", sda, scl, AMP_RECEIVER, revid);
	}
	devid = revid = 0;
	if (amp_read(sda, scl, AMP_SPEAKER, AMP_DEVID, &devid)) {
		printf("amp: %u/%u: 0x%02x answers, DEVID %#08x%s\n", sda, scl, AMP_SPEAKER, devid,
		    devid == AMP_DEVID_VALUE ? " (a CS35L41)" : " (not the expected 0x00035a40)");
		if (amp_read(sda, scl, AMP_SPEAKER, AMP_REVID, &revid)) {
			printf("amp: %u/%u: 0x%02x REVID %#08x\n", sda, scl, AMP_SPEAKER, revid);
		}
	} else {
		printf("amp: %u/%u: nothing answers 0x%02x (SDA %u, SCL %u)\n",
		    sda, scl, AMP_SPEAKER, sda, scl);
	}
	return true;
}

static void
pins_give_back(void)
{
	for (unsigned i = 0; i < 3; i++) {
		unsigned pin = i == 0 ? PIN_RESET : (i == 1 ? PIN_A : PIN_B);

		wr(GPIO_IO(pin), saved_io[i]);
		wr(GPIO_CFG(pin), saved_cfg[i]);
	}
}

static void
amp_thread(void *parameter, __unused wait_result_t result)
{
	uint32_t steps = (uint32_t)(uintptr_t)parameter;
	bool engine_idle, at_rest;

	printf("amp: step 1: map TLMM north, the clock controller and serial engine 3, and read them\n");
	tlmm = ml_io_map(TLMM_NORTH_PHYS, TLMM_SIZE);
	gcc = ml_io_map(GCC_PHYS, GCC_SIZE);
	se = ml_io_map(GENI_SE3_PHYS, GENI_SE3_SIZE);
	printf("amp: tlmm %#lx, gcc %#lx, se3 %#lx\n", tlmm, gcc, se);
	if (tlmm == 0 || gcc == 0 || se == 0) {
		printf("amp: the register blocks could not be mapped\n");
		goto out;
	}

	saved_cfg[0] = rd(GPIO_CFG(PIN_RESET));
	saved_io[0] = rd(GPIO_IO(PIN_RESET));
	saved_cfg[1] = rd(GPIO_CFG(PIN_A));
	saved_io[1] = rd(GPIO_IO(PIN_A));
	saved_cfg[2] = rd(GPIO_CFG(PIN_B));
	saved_io[2] = rd(GPIO_IO(PIN_B));

	/* what the pads say of themselves: pull, function, drive, output enable, and the level */
	for (unsigned i = 0; i < 3; i++) {
		unsigned pin = i == 0 ? PIN_RESET : (i == 1 ? PIN_A : PIN_B);

		/* [8:6] holds drive strength as (mA / 2) - 1, which is the encoding the touch driver writes */
		printf("amp: gpio %u: cfg %#010x (pull %u, function %u, drive %u mA, %s), level %u\n", pin,
		    saved_cfg[i], saved_cfg[i] & 3, function_of(saved_cfg[i]),
		    (((saved_cfg[i] >> 6) & 7) + 1) * 2, saved_cfg[i] & CFG_OUTPUT ? "driving" : "letting go",
		    level_of(pin));
	}

	printf("amp: se3: fw revision %#x (protocol %u, %s), fifo interface %s, fifo depth %u words\n",
	    rd(GENI_FW_REVISION_RO), rd(GENI_FW_REVISION_RO) >> 8 & 0xff,
	    (rd(GENI_FW_REVISION_RO) >> 8 & 0xff) == 3 ? "I2C" : "not I2C",
	    rd(GENI_IF_DISABLE_RO) & 1 ? "disabled" : "enabled", rd(SE_HW_PARAM_0) >> 16 & 0x3f);
	printf("amp: se3: status %#x (%s), clock control %#x, ser clock %#x\n", rd(GENI_STATUS),
	    rd(GENI_STATUS) & 1 ? "a command is running" : "idle", rd(GCC_SE3_CBCR), rd(GENI_SER_M_CLK_CFG));
	printf("amp: se3: clock configuration %#x, votes %#x (engine 3 %s)\n", rd(GCC_SE3_CFG),
	    rd(GCC_VOTE), rd(GCC_VOTE) & VOTE_SE3 ? "voted on" : "not voted on");
	engine_idle = (rd(GENI_STATUS) & 1) == 0;
	at_rest = level_of(PIN_A) && level_of(PIN_B);
	printf("amp: the bus: pins %u and %u are %s and the lines are %s\n", PIN_A, PIN_B,
	    function_of(saved_cfg[1]) == 0 && function_of(saved_cfg[2]) == 0 ? "plain GPIO" : "not plain GPIO",
	    at_rest ? "at rest" : "held low by something");

	if (steps < 2) {
		goto out;
	}

	/*
	 * Step 2 leaves the pads alone: it only speaks if the bus is already free and already in
	 * GPIO mode. If the bootloader or the loader set the pins up for the serial engine, this is
	 * the end of it, and the log says why rather than starting a second master on a live wire.
	 */
	printf("amp: step 2: speak on the bus, if it is ours to speak on\n");
	if (function_of(saved_cfg[1]) == 0 && function_of(saved_cfg[2]) == 0 && at_rest) {
		printf("amp: the bus can be taken: probing in both pin orders\n");
		amp_probe_order(PIN_A, PIN_B);
		amp_probe_order(PIN_B, PIN_A);
		pins_give_back();
	} else {
		printf("amp: not taking the bus: pins are %s and the lines are %s\n",
		    function_of(saved_cfg[1]) == 0 && function_of(saved_cfg[2]) == 0 ? "plain GPIO" : "muxed",
		    at_rest ? "at rest" : "not at rest");
	}

	if (steps < 3) {
		goto out;
	}

	/*
	 * Step 3 takes the pins from the engine, but only when the engine says no command is
	 * running: an engine in the middle of a transfer is a live master, and two masters on one
	 * bus is how a bus-in use hang starts. The engine is left alone otherwise.
	 */
	printf("amp: step 3: take the bus, muxing the pins to GPIO first (the engine is %s)\n",
	    engine_idle ? "idle" : "busy");
	if (!engine_idle) {
		printf("amp: not taking the bus: serial engine 3 has a command running\n");
		goto out;
	}
	wr(GPIO_IO(PIN_A), 0);
	wr(GPIO_CFG(PIN_A), CFG_PULL_UP);
	wr(GPIO_IO(PIN_B), 0);
	wr(GPIO_CFG(PIN_B), CFG_PULL_UP);
	spin_us(50);
	at_rest = level_of(PIN_A) && level_of(PIN_B);
	printf("amp: after muxing, the lines are %s (pins read %u and %u)\n",
	    at_rest ? "at rest" : "still low", level_of(PIN_A), level_of(PIN_B));
	if (at_rest) {
		amp_probe_order(PIN_A, PIN_B);
		amp_probe_order(PIN_B, PIN_A);
	}

	if (steps < 4) {
		goto give_back;
	}

	/*
	 * Step 4 resets the amplifiers first. Both share pin 114, the device tree says so, and the
	 * chip's own driver holds the line low for 2 ms (its "minimum reset pulse width") and then
	 * releases it; five milliseconds after that it expects the part to be out of boot.
	 */
	printf("amp: step 4: reset through gpio %u, then read again\n", PIN_RESET);
	{
		uint32_t cfg = saved_cfg[0] & ~(3u | 7u << 2 | 7u << 6);
		uint32_t io = saved_io[0] & ~(uint32_t)IO_OUT;

		wr(GPIO_IO(PIN_RESET), io);                     /* low ... */
		wr(GPIO_CFG(PIN_RESET), cfg | CFG_OUTPUT);      /* ... and driving */
		spin_us(2000);
		wr(GPIO_IO(PIN_RESET), io | IO_OUT);            /* release: high */
		spin_us(5000);
		printf("amp: reset released, line reads %u\n", level_of(PIN_RESET));
	}
	at_rest = level_of(PIN_A) && level_of(PIN_B);
	if (at_rest) {
		amp_probe_order(PIN_A, PIN_B);
		amp_probe_order(PIN_B, PIN_A);
	} else {
		printf("amp: the bus is not at rest after the reset (pins read %u and %u)\n",
		    level_of(PIN_A), level_of(PIN_B));
	}

	if (steps < 5) {
		goto give_back;
	}

	/*
	 * Step 5: the chip's boot. Step 4 showed it answering straight away, which only means its
	 * I2C block is up; a CS35L41's own driver does not trust that alone. It waits for
	 * IRQ1_STATUS4's OTP_BOOT_DONE -- up to 100 ms in 1 ms steps -- and then refuses the part if
	 * IRQ1_STATUS3 says OTP_BOOT_ERR. Both amplifiers are asked, because they share one reset
	 * line and are supposed to come out of it together; if they disagree, that is worth knowing
	 * before a single register is written to either.
	 */
	printf("amp: step 5: wait for the amplifiers' boot, and read their status words\n");
	for (unsigned amp = 0; amp < 2; amp++) {
		uint8_t address = amp == 0 ? AMP_RECEIVER : AMP_SPEAKER;
		const char *name = amp == 0 ? "0x40 (receiver)" : "0x41 (speaker)";
		uint32_t status4 = 0, status3 = 0, waited = 0;
		bool booted = false;

		while (waited < 100) {
			if (!amp_read(PIN_A, PIN_B, address, AMP_IRQ1_STATUS4, &status4)) {
				printf("amp: %s: went quiet while waiting for its boot\n", name);
				break;
			}
			if (status4 & AMP_IRQ1_STATUS4_BOOT) {
				booted = true;
				break;
			}
			spin_us(1000);
			waited++;
		}
		if (!booted && waited >= 100) {
			printf("amp: %s: no OTP_BOOT_DONE after 100 ms (status4 %#010x)\n", name, status4);
			continue;
		}
		if (!booted) {
			continue;
		}
		printf("amp: %s: booted after %u ms, status4 %#010x\n", name, waited, status4);
		if (amp_read(PIN_A, PIN_B, address, AMP_IRQ1_STATUS3, &status3)) {
			printf("amp: %s: status3 %#010x%s\n", name, status3,
			    status3 & AMP_IRQ1_STATUS3_ERR ? " -- OTP_BOOT_ERR: the part says its own boot failed" : "");
		}
		{
			uint32_t status1 = 0, status2 = 0, power = 0;

			if (amp_read(PIN_A, PIN_B, address, AMP_IRQ1_STATUS1, &status1) &&
			    amp_read(PIN_A, PIN_B, address, AMP_IRQ1_STATUS2, &status2)) {
				printf("amp: %s: status1 %#010x, status2 %#010x\n", name, status1, status2);
			}
			if (amp_read(PIN_A, PIN_B, address, AMP_PWRMGT_STS, &power)) {
				printf("amp: %s: power status %#010x\n", name, power);
			}
		}
	}

	if (steps < 6) {
		goto give_back;
	}

	/*
	 * Step 6, read-only: what the amplifiers look like before anything is written to them.
	 *
	 * The chip's own driver reads a good deal more than its name before it writes a register, and
	 * these are the words that decide how it is brought up: the power-control trio (driver on,
	 * boost on, whether the part is in software shutdown), the power-management control and status
	 * (the "blocked" bit the boost sequence waits on), the serial port enables, the amplifier gain,
	 * the two GPIOs, the clock-detect word and the input source routing. Knowing them now means the
	 * first write sequence can be written against the actual state rather than against the
	 * datasheet's reset values -- which is exactly the mistake the GPU's SMMU work was made to stop
	 * making.
	 */
	printf("amp: step 6: the registers as they are, before anything is written\n");
	for (unsigned amp = 0; amp < 2; amp++) {
		static const struct {
			const char *name;
			uint32_t index;
		} words[] = {
			{ "PWR_CTRL1 (driver, boost)", 0x00002014 },
			{ "PWR_CTRL2 (boost type)",     0x00002018 },
			{ "PWR_CTRL3",                  0x0000201C },
			{ "PWRMGT_CTL",                 0x00002900 },
			{ "PWRMGT_STS",                 0x00002908 },
			{ "SP_ENABLES",                 0x00004800 },
			{ "AMP_GAIN_CTRL",              0x00006C04 },
			{ "AMP_DIG_VOL_CTRL",           0x00006000 },
			{ "GPIO_STATUS1",               0x00011000 },
			{ "GPIO1_CTRL1",                0x00011008 },
			{ "GPIO2_CTRL1",                0x0001100C },
			{ "CLOCK_DETECT_1",             0x00014000 },
			{ "DAC_PCM1_SRC",               0x00004C00 },
			{ "ASP_CONTROL4",               0x00004854 },
		};
		uint8_t address = amp == 0 ? AMP_RECEIVER : AMP_SPEAKER;
		const char *name = amp == 0 ? "0x40 (receiver)" : "0x41 (speaker)";

		for (unsigned i = 0; i < sizeof(words) / sizeof(words[0]); i++) {
			uint32_t value = 0;

			if (!amp_read(PIN_A, PIN_B, address, words[i].index, &value)) {
				printf("amp: %s: %s: no answer\n", name, words[i].name);
				continue;
			}
			printf("amp: %s: %-26s %#010x\n", name, words[i].name, value);
		}
	}

	if (steps < 7) {
		goto give_back;
	}

	/*
	 * Step 7, the first write: can this kernel change a register, and does the change stay?
	 *
	 * The register is the analog gain, and the write lowers it. That direction is chosen
	 * deliberately: step 6 found both chips sitting at 19 of a maximum 20 (about +19.5 dB)
	 * straight out of reset, so clearing the field is something that has to happen before the
	 * amplifiers are ever enabled -- and doing it here, while the part is powered down
	 * (PWR_CTRL1 = 0), has no serial port enabled (SP_ENABLES = 0) and no clock (CLOCK_DETECT_1
	 * = 0), means the write cannot be heard. It also means a wrong write cannot make a sound.
	 *
	 * Then it is read back, and put back to what it was. A register that reads back what was
	 * written, and changes back when written back, is the whole of what the initialisation
	 * sequence needs from this bus.
	 */
	printf("amp: step 7: write the analog gain down to zero, read it back, and put it back\n");
	for (unsigned amp = 0; amp < 2; amp++) {
		uint8_t address = amp == 0 ? AMP_RECEIVER : AMP_SPEAKER;
		const char *name = amp == 0 ? "0x40 (receiver)" : "0x41 (speaker)";
		uint32_t before = 0, after = 0, restored = 0;
		uint32_t lowered;

		if (!amp_read(PIN_A, PIN_B, address, AMP_GAIN_CTRL, &before)) {
			printf("amp: %s: will not answer, so it is not written to\n", name);
			continue;
		}
		lowered = before & ~AMP_GAIN_PCM_MASK;           /* the PCM field to zero, everything else kept */
		printf("amp: %s: gain %#010x -> %#010x (PCM field %u -> 0)\n", name, before, lowered,
		    before >> AMP_GAIN_PCM_SHIFT & 0x1f);
		if (!amp_write(PIN_A, PIN_B, address, AMP_GAIN_CTRL, lowered)) {
			printf("amp: %s: the write was not acknowledged\n", name);
			continue;
		}
		spin_us(1000);
		if (!amp_read(PIN_A, PIN_B, address, AMP_GAIN_CTRL, &after)) {
			printf("amp: %s: no answer after the write\n", name);
			continue;
		}
		printf("amp: %s: reads back %#010x: %s\n", name, after,
		    after == lowered ? "THE WRITE TOOK" : "the write did not take");
		if (!amp_write(PIN_A, PIN_B, address, AMP_GAIN_CTRL, before)) {
			printf("amp: %s: THE GAIN COULD NOT BE PUT BACK (%#010x was its value)\n", name, before);
			continue;
		}
		spin_us(1000);
		if (amp_read(PIN_A, PIN_B, address, AMP_GAIN_CTRL, &restored)) {
			printf("amp: %s: put back, reads %#010x: %s\n", name, restored,
			    restored == before ? "as it was" : "NOT as it was");
		}
	}

	if (steps < 8) {
		goto give_back;
	}

	/*
	 * Step 8: configure the amplifiers for this phone's hardware, and leave them off.
	 *
	 * Two things are written, and neither can make a sound:
	 *
	 *   - the boost converter's coefficients, slope and peak current, from the device tree's own
	 *     parts (1000 nH, 15 uF, 4000 mA). Writing these arms the converter for the right inductor
	 *     and capacitor; it does not start it. That takes PWR_CTRL2's BST_EN and, above that,
	 *     PWR_CTRL1's GLOBAL_EN -- neither of which this step touches, and which step 6 found at
	 *     their "off" values.
	 *   - the analog gain, down to zero. Step 6 found both parts at 19 of 20 (about +19.5 dB)
	 *     out of reset, so this is the state that has to be replaced before anything is enabled.
	 *
	 * SP_ENABLES (the serial port) is left at zero as well, so the part has no input at all: it is
	 * configured, and silent. The one remaining step -- enabling it -- is the one that needs
	 * somebody listening, because that is the step that can make a noise.
	 *
	 * Each write is read back and compared here, so a register that does not take says so now
	 * rather than in the middle of the sequence that matters.
	 */
	printf("amp: step 8: configure the boost and the gain, and leave the amplifiers off\n");
	for (unsigned amp = 0; amp < 2; amp++) {
		static const struct {
			const char *name;
			uint32_t index, value, mask;
		} writes[] = {
			{ "boost coefficients", AMP_BST_COEFF,      AMP_BST_COEFF_VALUE, 0x0000ffff },
			{ "boost slope/inductor", AMP_BST_SLOPE_LBST, AMP_BST_SLOPE_VALUE, 0x0000ffff },
			{ "boost peak current", AMP_BST_PEAK_CUR,   AMP_BST_PEAK_VALUE,  0x0000007f },
		};
		uint8_t address = amp == 0 ? AMP_RECEIVER : AMP_SPEAKER;
		const char *name = amp == 0 ? "0x40 (receiver)" : "0x41 (speaker)";
		uint32_t gain = 0;

		for (unsigned i = 0; i < sizeof(writes) / sizeof(writes[0]); i++) {
			uint32_t before = 0, after = 0, wanted;

			if (!amp_read(PIN_A, PIN_B, address, writes[i].index, &before)) {
				printf("amp: %s: %s: no answer, not written\n", name, writes[i].name);
				continue;
			}
			wanted = (before & ~writes[i].mask) | (writes[i].value & writes[i].mask);
			if (!amp_write(PIN_A, PIN_B, address, writes[i].index, wanted)) {
				printf("amp: %s: %s: the write was not acknowledged\n", name, writes[i].name);
				continue;
			}
			spin_us(1000);
			if (amp_read(PIN_A, PIN_B, address, writes[i].index, &after)) {
				printf("amp: %s: %-22s %#010x -> %#010x: %s\n", name, writes[i].name, before, after,
				    after == wanted ? "took" : "DID NOT TAKE");
			}
		}

		if (!amp_read(PIN_A, PIN_B, address, AMP_GAIN_CTRL, &gain)) {
			printf("amp: %s: gain register: no answer\n", name);
			continue;
		}
		{
			uint32_t wanted = gain & ~AMP_GAIN_PCM_MASK, after = 0;

			if (!amp_write(PIN_A, PIN_B, address, AMP_GAIN_CTRL, wanted)) {
				printf("amp: %s: gain: the write was not acknowledged\n", name);
				continue;
			}
			spin_us(1000);
			if (amp_read(PIN_A, PIN_B, address, AMP_GAIN_CTRL, &after)) {
				printf("amp: %s: analog gain           %#010x -> %#010x: %s (PCM field %u -> %u)\n", name, gain, after,
				    after == wanted ? "took" : "DID NOT TAKE", gain >> AMP_GAIN_PCM_SHIFT & 0x1f,
				    after >> AMP_GAIN_PCM_SHIFT & 0x1f);
			}
		}
	}

	/* What must still be off afterwards, read back rather than assumed. */
	printf("amp: step 8: the state left behind\n");
	for (unsigned amp = 0; amp < 2; amp++) {
		uint8_t address = amp == 0 ? AMP_RECEIVER : AMP_SPEAKER;
		const char *name = amp == 0 ? "0x40 (receiver)" : "0x41 (speaker)";
		uint32_t pwr1 = 0, pwr2 = 0, sp = 0;

		if (!amp_read(PIN_A, PIN_B, address, 0x00002014, &pwr1) ||
		    !amp_read(PIN_A, PIN_B, address, 0x00002018, &pwr2) ||
		    !amp_read(PIN_A, PIN_B, address, 0x00004800, &sp)) {
			printf("amp: %s: no answer for the final read\n", name);
			continue;
		}
		printf("amp: %s: PWR_CTRL1 %#010x (%s), PWR_CTRL2 %#010x (BST_EN %u), SP_ENABLES %#010x (%s)\n", name, pwr1,
		    pwr1 == 0 ? "off, as it must be" : "NOT OFF", pwr2, pwr2 >> 4 & 3, sp,
		    sp == 0 ? "serial port off, silent" : "SERIAL PORT IS OPEN");
	}

	if (steps < 9) {
		goto give_back;
	}

	/*
	 * Step 9, read-only: where the audio's own pins live.
	 *
	 * The amplifiers take their sound from the tertiary TDM interface, whose four pins the device
	 * tree names in pinctrl "tert_tdm_*" (gpio133 clock, 135 sync, 134 data in, 137 data out,
	 * function mi2s2_*). Those are on SM8250's west TLMM tile, which nothing in this kernel has
	 * touched yet: the touch screen proved 8..11 on the north tile and the haptic 60/61 on the
	 * south one, and both agree that a pin's registers are at tile + 0x1000 * n.
	 *
	 * The west tile's own base is not something to guess -- the whole point of the GPU's SMMU
	 * work is that an address assumed and wrong costs more than one measured. So the same pin is
	 * read at every plausible base inside the vendor's 16 MB pinctrl window, and the log shows all
	 * four side by side. The one holding the pin's configuration will have its function, drive and
	 * pull fields set the way a real pad is; the others read whatever lies at that offset.
	 *
	 * Nothing here writes; the pins are read through the north tile's mapping only for 114/119/120,
	 * which are already established.
	 */
	printf("amp: step 9: the audio's TDM pins, and which TLMM tile they are on\n");
	{
		static const struct {
			const char *name;
			uint32_t phys;
		} bases[] = {
			{ "f000000 (the window's start)", 0x0f000000UL },
			{ "f100000 (west, this is the one)", TLMM_WEST_PHYS },
			{ "f500000 (south, known)",       0x0f500000UL },
			{ "f900000 (north, known)",       0x0f900000UL },
		};
		static const unsigned tdm[] = { PIN_TDM_CLK, PIN_TDM_SYNC, PIN_TDM_DIN, PIN_TDM_DOUT };
		static const char *const names[] = { "bit clock", "frame sync", "data in", "data out" };

		for (unsigned b = 0; b < sizeof(bases) / sizeof(bases[0]); b++) {
			vm_offset_t tile = ml_io_map(bases[b].phys, TLMM_SIZE);

			if (tile == 0) {
				printf("amp: tlmm at %s: could not be mapped\n", bases[b].name);
				continue;
			}
			for (unsigned i = 0; i < 4; i++) {
				uint32_t cfg = rd(tile + 0x1000UL * tdm[i]);
				uint32_t io = rd(tile + 0x1000UL * tdm[i] + 4);

				printf("amp: tlmm %s, gpio %u (%s): cfg %#010x (pull %u, function %u, drive %u mA), io %#010x (level %u)\n",
				    bases[b].name, tdm[i], names[i], cfg, cfg & 3, cfg >> 2 & 0xf, (((cfg >> 6) & 7) + 1) * 2, io, io & 1);
			}
		}
		/* The device tree's own answer, for comparison: mi2s2_* is function 1 in those pin groups. */
		printf("amp: (the device tree muxes these four to mi2s2_*; in the pin group's own list that is function 1)\n");
	}

give_back:
	pins_give_back();
	printf("amp: the pins are put back as they were found\n");
	/*
	 * And prove it: with the reset line back at the level it was found at (low -- the bootloader
	 * leaves the amplifiers in reset), they must stop answering. A probe that says "I put it back"
	 * without checking would be the same kind of claim this project has already been burned by, and
	 * this check costs one more read on a bus that is about to be left alone anyway.
	 */
	{
		uint32_t devid = 0;

		spin_us(2000);
		if (amp_read(PIN_A, PIN_B, AMP_RECEIVER, AMP_DEVID, &devid)) {
			printf("amp: AFTER PUTTING IT BACK an amplifier still answers (DEVID %#08x): the reset line is not off\n",
			    devid);
		} else {
			printf("amp: after putting it back, the amplifiers are silent again: the probe left nothing behind\n");
		}
	}
out:
	thread_terminate(current_thread());
}

void
hakios_amp_init(void)
{
	uint32_t steps = 0;
	thread_t thread;

	if (!PE_parse_boot_argn("amp_step", &steps, sizeof(steps)) || steps == 0) {
		return;
	}
	if (kernel_thread_start(amp_thread, (void *)(uintptr_t)steps, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
