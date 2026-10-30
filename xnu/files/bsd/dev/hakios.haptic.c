/*
 * Redmi K40 (alioth) AW8697 haptic driver, hardware probe stages 1 to 4.
 *
 * Alioth differs from redwood's AW86927 in both the chip register layout and
 * the SoC pin map.  All values here come from alioth's own FDT and Xiaomi's
 * AW8697 driver, not from redwood.
 *
 *   I2C       QUPv3 SE11, GPIO60 SDA / GPIO61 SCL, address 0x5a
 *   reset     GPIO130, high = running
 *   interrupt GPIO129, pulled up (polled, not registered)
 *   TLMM      GPIO4..23 use North 0x0f900000; the others use South 0x0f500000
 *
 * haptic=1 reads the pins and attempts an ID read without changing reset.
 * haptic=2 pulses reset low for 5 ms, releases it, waits 8 ms, then reads the
 * ID and registers.  It never writes an AW8697 register and never plays.
 * haptic=3 writes alioth's static configuration and loads the phone's own RAM
 * firmware, then compares every byte and returns to standby with GO=0.
 * haptic=4 first measures f0 with Xiaomi's CONT/bypass sequence, then plays
 * one RAM waveform if and only if calibration completes without a fault.
 * The iOS service is deliberately not enabled in this build.
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <sys/hakios.audio.h>
#include <sys/hakios.input.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern kern_return_t thread_terminate(thread_t thread);
extern void loader_log_putc(char c);

static vm_offset_t tlmm_north, tlmm_south;
static uint32_t level;
static bool broken, released, leaving;

static int say(const char *format, ...) __printflike(1, 2);

static int
say(const char *format, ...)
{
	char line[300];
	va_list list;
	int length;

	va_start(list, format);
	length = vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	for (const char *c = line; *c != 0; c++) {
		loader_log_putc(*c);
	}
	if (!leaving && ml_get_interrupts_enabled()) {
		printf("%s", line);
	}
	return length;
}

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

static void
spin_us(uint32_t us)
{
	uint64_t until;

	nanoseconds_to_absolutetime((uint64_t)us * NSEC_PER_USEC, &until);
	until += mach_absolute_time();
	while (mach_absolute_time() < until) {
	}
}

static void
wait_ms(uint32_t ms)
{
	if (leaving) {
		spin_us(ms * 1000);
	} else {
		delay_for_interval(ms, NSEC_PER_MSEC);
	}
}

/* ---- SM8250 TLMM: North and South are separate windows, not one flat array. */

#define TLMM_NORTH_PHYS         0x0f900000UL
#define TLMM_SOUTH_PHYS         0x0f500000UL
#define TLMM_SIZE               0x100000UL
#define TLMM_BASE(n)            ((n) >= 4 && (n) <= 23 ? tlmm_north : tlmm_south)
#define GPIO_CFG(n)             (TLMM_BASE(n) + 0x1000UL * (n))
#define GPIO_IO(n)              (GPIO_CFG(n) + 4)
#define CFG_PULL_UP             3
#define CFG_OUTPUT              (1u << 9)

#define PIN_SDA                 60
#define PIN_SCL                 61
#define PIN_RESET               130
#define PIN_INT                 129

static void
show_pins(void)
{
	static const struct {
		unsigned pin;
		const char *what;
	} pins[] = {
		{ PIN_SDA, "I2C data" }, { PIN_SCL, "I2C clock" },
		{ PIN_RESET, "reset" }, { PIN_INT, "interrupt" }
	};

	for (unsigned i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) {
		uint32_t cfg = rd(GPIO_CFG(pins[i].pin));
		uint32_t io = rd(GPIO_IO(pins[i].pin));

		say("haptic: gpio%u (%s): cfg %#x, function %u, pull %u, %u mA, %s, reads %u\n",
		    pins[i].pin, pins[i].what, cfg, (cfg >> 2) & 15, cfg & 3,
		    (((cfg >> 6) & 7) + 1) * 2,
		    (cfg & CFG_OUTPUT) ? (io & 2 ? "drives high" : "drives low") : "input", io & 1);
	}
}

static void
reset_pin(bool high)
{
	wr(GPIO_IO(PIN_RESET), high ? 2 : 0);
	wr(GPIO_CFG(PIN_RESET), CFG_OUTPUT);
	released = high;
}

/* ---- PM8150B GPIO5: alioth's vdd_hap_boost fixed regulator. ---- */

#define ARB_CORE_PHYS           0x0c440000UL
#define ARB_CORE_SIZE           0x1100UL
#define ARB_CNFG_PHYS           0x0c40a000UL
#define ARB_CNFG_SIZE           0x1000UL
#define ARB_OBSERVER_PHYS       0x0e600000UL
#define ARB_OBSERVER_SIZE       0x10000UL
#define ARB_CHNLS_PHYS          0x0c600000UL
#define ARB_SYSTEM              0
#define ARB_VERSION             0x000
#define ARB_VERSION_5           0x50000000u
#define ARB_VERSION_7           0x70000000u
#define ARB_FEATURES            0x004
#define ARB_CHANNEL(n)          (0x900 + 4 * (n))
#define ARB_OWNER(n)            (0x700 + 4 * (n))
#define ARB_MAX_CHANNELS        512
#define CHANNEL_COMMAND         0x00
#define CHANNEL_STATUS          0x08
#define CHANNEL_WDATA0          0x10
#define OBSERVER_DATA0          0x18
#define OBSERVER_DATA1          0x1c
#define STATUS_DONE             0x1
#define STATUS_FAILURE          0x2
#define STATUS_DENIED           0x4
#define STATUS_DROPPED          0x8
#define COMMAND_WRITE           (0u << 27)
#define COMMAND_READ            (1u << 27)
#define TRANSFER_TIMEOUT_US     2000
#define DROPPED                 "the transfer was dropped"

#define BOOST_ID                0x2c4   /* PM8150B slave 2, GPIO5 at 0xc400 */
#define PMIC_TYPE               0x04
#define PMIC_RT_STATUS          0x10
#define PMIC_MODE               0x40
#define PMIC_SUPPLY             0x41
#define PMIC_PULL               0x42
#define PMIC_OUT_SOURCE         0x44   /* LV/MV: bit 7 is the output value */
#define PMIC_OUT_CTL            0x45
#define PMIC_ENABLE             0x46

static vm_offset_t arb_core, arb_cnfg, arb_observer, boost_write;
static uint16_t boost_channel;
static bool boost_found, boost_writable, boost_we_enabled;

static const char *
spmi_wait(vm_offset_t channel)
{
	uint64_t start = mach_absolute_time(), interval;
	uint32_t status;

	nanoseconds_to_absolutetime(TRANSFER_TIMEOUT_US * (uint64_t)NSEC_PER_USEC, &interval);
	do {
		status = rd(channel + CHANNEL_STATUS);
	} while (!(status & STATUS_DONE) && mach_absolute_time() - start < interval);
	return !(status & STATUS_DONE) ? "no answer from the arbiter" :
	       (status & STATUS_DENIED) ? "the arbiter refuses" :
	       (status & STATUS_FAILURE) ? "the transfer failed" :
	       (status & STATUS_DROPPED) ? DROPPED : NULL;
}

static const char *
spmi_read_once(uint8_t reg, uint8_t *bytes, uint32_t count)
{
	vm_offset_t channel = arb_observer + 0x10000UL * ARB_SYSTEM + 0x80UL * boost_channel;
	const char *problem;
	uint32_t data[2];

	if (!boost_found || count == 0 || count > 8) {
		return "no channel or an invalid transfer length";
	}
	if (!leaving) {
		hakios_spmi_lock();
	}
	wr(channel + CHANNEL_COMMAND, COMMAND_READ | (uint32_t)reg << 4 | (count - 1));
	problem = spmi_wait(channel);
	data[0] = rd(channel + OBSERVER_DATA0);
	data[1] = count > 4 ? rd(channel + OBSERVER_DATA1) : 0;
	if (!leaving) {
		hakios_spmi_unlock();
	}
	if (problem != NULL) {
		return problem;
	}
	for (uint32_t i = 0; i < count; i++) {
		bytes[i] = (uint8_t)(data[i / 4] >> (8 * (i % 4)));
	}
	return NULL;
}

static const char *
spmi_read(uint8_t reg, uint8_t *bytes, uint32_t count)
{
	const char *problem = NULL;

	for (unsigned tries = 0; tries < 3; tries++) {
		problem = spmi_read_once(reg, bytes, count);
		if (problem == NULL || strcmp(problem, DROPPED) != 0) {
			break;
		}
	}
	return problem;
}

static bool
pmic_get(uint8_t reg, uint8_t *value)
{
	const char *problem = spmi_read(reg, value, 1);

	if (problem != NULL) {
		say("haptic: PM8150B GPIO5 register %#04x: %s\n", reg, problem);
		return false;
	}
	return true;
}

static bool
pmic_put(uint8_t reg, uint8_t value)
{
	const char *problem = NULL;
	uint8_t back = 0xff;

	if (!boost_writable || boost_write == 0 ||
	    ((rd(arb_core + ARB_CHANNEL(boost_channel)) >> 8) & 0xfff) != BOOST_ID) {
		say("haptic: refusing a PMIC write: the GPIO5 channel is absent, not ours, or changed\n");
		return false;
	}
	for (unsigned tries = 0; tries < 3; tries++) {
		if (!leaving) {
			hakios_spmi_lock();
		}
		wr(boost_write + CHANNEL_WDATA0, value);
		wr(boost_write + CHANNEL_COMMAND, COMMAND_WRITE | (uint32_t)reg << 4);
		problem = spmi_wait(boost_write);
		if (!leaving) {
			hakios_spmi_unlock();
		}
		if (problem == NULL || strcmp(problem, DROPPED) != 0) {
			break;
		}
	}
	if (problem != NULL || !pmic_get(reg, &back)) {
		say("haptic: PM8150B GPIO5 %#04x <- %#04x failed%s%s\n", reg, value,
		    problem != NULL ? ": " : "", problem != NULL ? problem : "");
		return false;
	}
	if (back != value) {
		say("haptic: PM8150B GPIO5 %#04x <- %#04x reads back %#04x\n", reg, value, back);
		return false;
	}
	return true;
}

static bool
pmic_masked(uint8_t reg, uint8_t mask, uint8_t value)
{
	uint8_t now;

	if (!pmic_get(reg, &now)) {
		return false;
	}
	return (now & mask) == (value & mask) ||
	       pmic_put(reg, (uint8_t)((now & ~mask) | (value & mask)));
}

static void
find_boost_channel(uint32_t channels)
{
	for (uint32_t n = 0; n < channels; n++) {
		uint32_t map = rd(arb_core + ARB_CHANNEL(n)), owner;

		if (map == 0 || ((map >> 8) & 0xfff) != BOOST_ID) {
			continue;
		}
		owner = rd(arb_cnfg + ARB_OWNER(n)) & 7;
		say("haptic: SPMI channel %u is PM8150B GPIO5 (map %#x), owner %u%s\n",
		    n, map, owner, owner == ARB_SYSTEM ? " (us)" : "");
		if (!boost_found || owner == ARB_SYSTEM) {
			boost_found = true;
			boost_channel = (uint16_t)n;
			boost_writable = owner == ARB_SYSTEM;
		}
	}
}

/* Linux pinctrl-spmi-gpio.c's LV/MV layout; values from alioth's own FDT. */
static bool
boost_power(bool high)
{
	uint8_t id[2], config[8], status = 0;

	if (spmi_read(PMIC_TYPE, id, 2) != NULL || spmi_read(PMIC_MODE, config, 8) != NULL ||
	    !pmic_get(PMIC_RT_STATUS, &status)) {
		say("haptic: cannot read the haptics boost GPIO; AW8697 is left alone\n");
		return false;
	}
	say("haptic: PM8150B GPIO5 type %#04x/%#04x, cfg 40-47: %02x %02x %02x %02x %02x %02x %02x %02x, RT %#04x\n",
	    id[0], id[1], config[0], config[1], config[2], config[3], config[4], config[5], config[6], config[7], status);
	if (id[0] != 0x10 || id[1] < 0x10 || id[1] > 0x15) {
		say("haptic: GPIO5 is not an LV/MV PMIC GPIO; no supply write is made\n");
		return false;
	}
	if (high && !boost_we_enabled) {
		/* normal, output, VIN1, no pull, CMOS high strength, output high */
		if (!pmic_masked(PMIC_SUPPLY, 0x07, 0x01) ||
		    !pmic_masked(PMIC_PULL, 0x07, 0x05) ||
		    !pmic_masked(PMIC_OUT_CTL, 0x33, 0x03) ||
		    !pmic_masked(PMIC_MODE, 0x03, 0x01) ||
		    !pmic_masked(PMIC_OUT_SOURCE, 0x8f, 0x80) ||
		    !pmic_masked(PMIC_ENABLE, 0x80, 0x80)) {
			say("haptic: vdd_hap_boost could not be enabled; no motor operation follows\n");
			return false;
		}
		boost_we_enabled = true;
	} else if (!high && boost_we_enabled && !pmic_masked(PMIC_OUT_SOURCE, 0x80, 0)) {
		say("haptic: WARNING: vdd_hap_boost could not be switched off\n");
		return false;
	}
	if (!pmic_get(PMIC_OUT_SOURCE, &config[4]) || !pmic_get(PMIC_ENABLE, &config[6]) ||
	    !pmic_get(PMIC_RT_STATUS, &status)) {
		return false;
	}
	say("haptic: vdd_hap_boost %s: output-source %#04x, enable %#04x, pin status %#04x\n",
	    high ? "requested ON" : "requested OFF", config[4], config[6], status);
	if (high) {
		/* RT status is zero while the input buffer is disabled.  Temporarily
		 * select input+output (Linux mode 2), read the physical pin, and put
		 * the DTS-selected output-only mode back before touching the chip. */
		if (!pmic_put(PMIC_MODE, (config[0] & ~0x03) | 0x02)) {
			return false;
		}
		if (!pmic_get(PMIC_RT_STATUS, &status)) {
			(void)pmic_put(PMIC_MODE, config[0]);
			return false;
		}
		say("haptic: diagnostic input+output reads physical vdd_hap_boost pin %s (RT %#04x)\n",
		    status & 1 ? "HIGH" : "LOW", status);
		if (!pmic_put(PMIC_MODE, config[0])) {
			return false;
		}
		if (!(status & 1)) {
			say("haptic: physical boost control is low; refusing chip initialization\n");
			return false;
		}
	}
	return high ? (config[4] & 0x80) != 0 && (config[6] & 0x80) != 0 : (config[4] & 0x80) == 0;
}

static bool
boost_start(void)
{
	uint32_t version, channels;

	arb_core = ml_io_map(ARB_CORE_PHYS, ARB_CORE_SIZE);
	arb_cnfg = ml_io_map(ARB_CNFG_PHYS, ARB_CNFG_SIZE);
	arb_observer = ml_io_map(ARB_OBSERVER_PHYS, ARB_OBSERVER_SIZE);
	version = rd(arb_core + ARB_VERSION);
	channels = rd(arb_core + ARB_FEATURES) & 0x7ff;
	say("haptic: PMIC arbiter version %#x, %u channels\n", version, channels);
	if (version < ARB_VERSION_5 || version >= ARB_VERSION_7) {
		return false;
	}
	if (channels == 0 || channels > ARB_MAX_CHANNELS) {
		channels = ARB_MAX_CHANNELS;
	}
	find_boost_channel(channels);
	if (!boost_found || !boost_writable) {
		say("haptic: no writable PM8150B GPIO5 channel; boost is left unchanged\n");
		return false;
	}
	boost_write = ml_io_map(ARB_CHNLS_PHYS + 0x10000UL * boost_channel, 0x1000);
	if (!boost_power(true)) {
		return false;
	}
	wait_ms(2);                    /* FDT startup-delay-us = 1000 */
	return true;
}

/* ---- 100 kHz open-drain I2C, with the two original pin configurations restored. */

static void
half_bit(void)
{
	spin_us(5);
}

static void
line(unsigned pin, bool high)
{
	wr(GPIO_IO(pin), 0);
	wr(GPIO_CFG(pin), CFG_PULL_UP | (high ? 0 : CFG_OUTPUT));
}

static bool
level_of(unsigned pin)
{
	return rd(GPIO_IO(pin)) & 1;
}

static void
i2c_start(void)
{
	line(PIN_SDA, true);
	line(PIN_SCL, true);
	half_bit();
	line(PIN_SDA, false);
	half_bit();
	line(PIN_SCL, false);
	half_bit();
}

static void
i2c_stop(void)
{
	line(PIN_SDA, false);
	half_bit();
	line(PIN_SCL, true);
	half_bit();
	line(PIN_SDA, true);
	half_bit();
}

static bool
i2c_bit(bool out)
{
	bool in;

	line(PIN_SDA, out);
	half_bit();
	line(PIN_SCL, true);
	for (unsigned stretched = 0; !level_of(PIN_SCL) && stretched < 100; stretched++) {
		half_bit();
	}
	half_bit();
	in = level_of(PIN_SDA);
	line(PIN_SCL, false);
	return in;
}

static bool
i2c_write(uint8_t byte)
{
	for (int bit = 7; bit >= 0; bit--) {
		i2c_bit((byte >> bit) & 1);
	}
	return !i2c_bit(true);
}

static uint8_t
i2c_read(bool more)
{
	uint8_t byte = 0;

	for (int bit = 7; bit >= 0; bit--) {
		byte = (uint8_t)(byte << 1 | i2c_bit(true));
	}
	i2c_bit(!more);
	return byte;
}

static uint32_t taken_cfg[2], taken_io[2];

static bool
bus_take(void)
{
	taken_cfg[0] = rd(GPIO_CFG(PIN_SDA));
	taken_cfg[1] = rd(GPIO_CFG(PIN_SCL));
	taken_io[0] = rd(GPIO_IO(PIN_SDA));
	taken_io[1] = rd(GPIO_IO(PIN_SCL));
	line(PIN_SDA, true);
	line(PIN_SCL, true);
	half_bit();
	if (!level_of(PIN_SDA) || !level_of(PIN_SCL)) {
		bool sda = level_of(PIN_SDA), scl = level_of(PIN_SCL);

		wr(GPIO_IO(PIN_SDA), taken_io[0] & 2);
		wr(GPIO_IO(PIN_SCL), taken_io[1] & 2);
		wr(GPIO_CFG(PIN_SDA), taken_cfg[0]);
		wr(GPIO_CFG(PIN_SCL), taken_cfg[1]);
		say("haptic: I2C is not at rest (SDA %u, SCL %u): not touched\n", sda, scl);
		return false;
	}
	return true;
}

static void
bus_give_back(void)
{
	wr(GPIO_IO(PIN_SDA), taken_io[0] & 2);
	wr(GPIO_IO(PIN_SCL), taken_io[1] & 2);
	wr(GPIO_CFG(PIN_SDA), taken_cfg[0]);
	wr(GPIO_CFG(PIN_SCL), taken_cfg[1]);
}

#define AW_ADDRESS              0x5a
#define AW_REG_ID               0x00
#define AW_CHIP_ID              0x97

static bool
regs_read(uint8_t reg, uint8_t *values, unsigned count)
{
	bool answered;

	if (!bus_take()) {
		return false;
	}
	i2c_start();
	answered = i2c_write(AW_ADDRESS << 1) && i2c_write(reg);
	if (answered) {
		i2c_start();
		answered = i2c_write(AW_ADDRESS << 1 | 1);
	}
	for (unsigned i = 0; answered && i < count; i++) {
		values[i] = i2c_read(i + 1 < count);
	}
	i2c_stop();
	bus_give_back();
	return answered;
}

static bool
chip_answers(void)
{
	uint8_t id = 0;

	if (!regs_read(AW_REG_ID, &id, 1)) {
		say("haptic: nobody acknowledges address %#x on GPIO%u/%u\n", AW_ADDRESS, PIN_SDA, PIN_SCL);
		return false;
	}
	say("haptic: chip ID %#04x%s\n", id, id == AW_CHIP_ID ? ": AW8697, as alioth's FDT says" : ": NOT the expected AW8697 (0x97)");
	return id == AW_CHIP_ID;
}

static void
show_registers(void)
{
	unsigned failed = 0;

	/* RAMDATA is a streaming port, not an ordinary register: leave it alone. */
	for (unsigned base = 0; base < 0x80; base += 16) {
		uint8_t values[16] = { 0 };
		char row[180];
		int used = 0;

		for (unsigned i = 0; i < 16; i++) {
			unsigned reg = base + i;

			if (reg == 0x42) {
				used += snprintf(row + used, sizeof(row) - used, " --");
				continue;
			}
			if (!regs_read((uint8_t)reg, &values[i], 1)) {
				say("haptic: register %#04x NOT READ\n", reg);
				failed++;
				return;
			}
			used += snprintf(row + used, sizeof(row) - used, " %02x", values[i]);
		}
		say("haptic: regs %02x-%02x:%s\n", base, base + 15, row);
	}
	say("haptic: register scan complete (%u failures); no chip register was written\n", failed);
}

/* ---- Levels 3: static configuration and RAM verification, never GO=1. ---- */

#define AW_SYSST                0x01
#define AW_SYSINT               0x02
#define AW_SYSINTM              0x03
#define AW_SYSCTRL              0x04
#define AW_GO                   0x05
#define AW_WAVSEQ1              0x07
#define AW_WAVLOOP1             0x0f
#define AW_BASE_ADDRH           0x21
#define AW_BASE_ADDRL           0x22
#define AW_FIFO_AEH             0x23
#define AW_FIFO_AEL             0x24
#define AW_FIFO_AFH             0x25
#define AW_FIFO_AFL             0x26
#define AW_DATCTRL              0x2b
#define AW_PWMDBG               0x2e
#define AW_BSTDBG1              0x31
#define AW_BSTCFG               0x34
#define AW_ANADBG               0x35
#define AW_ANACTRL              0x36
#define AW_DATDBG               0x39
#define AW_BSTDBG4              0x3a
#define AW_RAMADDRH             0x40
#define AW_RAMADDRL             0x41
#define AW_RAMDATA              0x42
#define AW_GLB_STATE            0x46
#define AW_BST_AUTO             0x47
#define AW_CONT_CTRL            0x48
#define AW_F_PRE_H              0x49
#define AW_F_PRE_L              0x4a
#define AW_TSET                 0x4d
#define AW_TRIM_LRA             0x5b
#define AW_R_SPARE              0x5d
#define AW_ADCTEST              0x66
#define AW_F_LRA_F0_H           0x68
#define AW_F_LRA_F0_L           0x69
#define AW_DRV_LVL              0x7b
#define AW_NUM_F0_1             0x7d
#define AW_NUM_F0_2             0x7e
#define AW_NUM_F0_3             0x7f
#define AW_BEMF_VTHH_H          0x74
#define AW_BEMF_VTHH_L          0x75
#define AW_BEMF_VTHL_H          0x76
#define AW_BEMF_VTHL_L          0x77
#define SYSINT_FAULTS           0xe6

static bool
regs_write(uint8_t reg, const uint8_t *values, unsigned count)
{
	bool answered;

	if (!bus_take()) {
		return false;
	}
	i2c_start();
	answered = i2c_write(AW_ADDRESS << 1) && i2c_write(reg);
	for (unsigned i = 0; answered && i < count; i++) {
		answered = i2c_write(values[i]);
	}
	i2c_stop();
	bus_give_back();
	return answered;
}

static void
give_up(const char *why)
{
	broken = true;
	say("haptic: %s: GPIO%u is driven low and the chip is abandoned for this run\n", why, PIN_RESET);
	if (tlmm_south != 0) {
		reset_pin(false);
	}
}

static bool
get(uint8_t reg, uint8_t *value)
{
	if (!regs_read(reg, value, 1)) {
		say("haptic: register %#04x NOT READ\n", reg);
		return false;
	}
	return true;
}

static bool
put(uint8_t reg, uint8_t value)
{
	uint8_t after = 0;

	if (broken) {
		return false;
	}
	if (!regs_write(reg, &value, 1)) {
		give_up("a write was not acknowledged");
		return false;
	}
	if (!get(reg, &after)) {
		give_up("a register could not be read back");
		return false;
	}
	say("haptic: reg %#04x <- %#04x, reads %#04x%s\n", reg, value, after,
	    after == value ? "" : ": MISMATCH");
	if (after != value) {
		give_up("a register did not read back as written");
		return false;
	}
	return true;
}

static uint32_t measured_vbat;

static bool
healthy(const char *when, bool active)
{
	uint8_t status = 0, flags = 0, faults;

	if (!get(AW_SYSST, &status) || !get(AW_SYSINT, &flags)) {
		give_up("the status or interrupt flags could not be read");
		return false;
	}
	say("haptic: health %s: SYSST %#04x, latched SYSINT %#04x\n", when, status, flags);
	faults = status | flags;
	if (!active && measured_vbat >= 3500 && measured_vbat <= 5000) {
		/* Xiaomi masks UVLO in standby and enables it again in active mode.
		 * Do not hide it during playback: it is only a standby diagnostic here. */
		if (faults & 0x20) {
			say("haptic: standby UVLS/UVLOI is noted with measured VBAT %u mV; active mode will recheck it\n",
			    measured_vbat);
		}
		faults &= ~0x20;
	}
	if (faults & SYSINT_FAULTS) {
		say("haptic: %s: FAULTS %#04x:%s%s%s%s%s\n", when, faults,
		    faults & 0x80 ? " BOOST SHORT" : "", faults & 0x40 ? " BOOST OVER-VOLTAGE" : "",
		    faults & 0x20 ? " UNDER-VOLTAGE" : "", faults & 0x04 ? " OVER-CURRENT" : "",
		    faults & 0x02 ? " OVER-TEMPERATURE" : "");
		give_up("the chip reported a fault");
		return false;
	}
	return true;
}

static uint32_t states_seen;

static bool
wait_standby(unsigned ms)
{
	states_seen = 0;
	for (unsigned waited = 0; waited <= ms; waited += 2) {
		uint8_t state = 0xff;

		if (!get(AW_GLB_STATE, &state)) {
			return false;
		}
		states_seen |= 1u << (state & 0x0f);
		if ((state & 0x0f) == 0) {
			return true;
		}
		wait_ms(2);
	}
	return false;
}

#define AW_DETCTRL              0x5f
#define AW_VBATDET              0x62

static bool measure_vbat(void);

/* Xiaomi runs DIAG_GO during probe with RAM open and GO clear.  This is an
 * internal offset measurement, not LRA playback; retain a bounded poll and
 * restore standby before doing anything else. */
static bool
calibrate_offset(void)
{
	uint8_t control = 0, start = 0x01;
	bool done = false;

	/* DIAG_GO is self-clearing and can already read zero after the I2C write,
	 * so do not use put()'s ordinary read-back equality check for this bit. */
	if (!put(AW_GO, 0) || !put(AW_SYSCTRL, 0x63) || !regs_write(AW_DETCTRL, &start, 1)) {
		give_up("offset calibration could not be started");
		return false;
	}
	for (unsigned i = 0; i < 500; i++) {
		if (!get(AW_DETCTRL, &control)) {
			give_up("offset calibration could not be polled");
			return false;
		}
		if (!(control & 0x01)) {
			done = true;
			break;
		}
		wait_ms(1);
	}
	if (!put(AW_DETCTRL, 0) || !put(AW_SYSCTRL, 0x43)) {
		give_up("the chip could not be restored after offset calibration");
		return false;
	}
	if (!done) {
		give_up("offset calibration timed out");
		return false;
	}
	say("haptic: native DIAG_GO offset calibration completed; GO stayed 0\n");
	return true;
}

static bool
configure(void)
{
	/* Xiaomi's alioth DTS and AW8697 init sequence.  Motor protection is
	 * deliberately left enabled; LRA/f0 calibration is a separate later step. */
	static const uint8_t config[][2] = {
		{ AW_GO, 0x00 },
		{ AW_SYSINTM, 0x7f },       /* interrupts polled, all masked */
		{ AW_SYSCTRL, 0x43 },       /* RAM mode, boost, standby */
		{ AW_PWMDBG, 0x40 },        /* 24 kHz */
		{ AW_BSTDBG1, 0x30 },
		{ AW_BSTDBG1 + 1, 0xeb },
		{ AW_BSTDBG1 + 2, 0xd4 },
		{ AW_TSET, 0x12 },
		{ AW_R_SPARE, 0x68 },
		{ AW_ANADBG, 0x1e },        /* 4.65 A over-current detection */
		{ AW_BSTCFG, 0x05 },        /* 3.5 A boost peak current */
		{ AW_BST_AUTO, 0x01 },      /* manual boost, RAM boost enabled */
		{ AW_BSTDBG4, 0x62 },       /* DTS vib_bst_vol_ram = 0x11 */
		{ AW_DATDBG, 0x80 },        /* Android's default gain */
		{ AW_ADCTEST, 0x4c },       /* hardware VBAT compensation */
		{ AW_BEMF_VTHH_H, 0x00 },
		{ AW_BEMF_VTHH_L, 0x08 },
		{ AW_BEMF_VTHL_H, 0x03 },
		{ AW_BEMF_VTHL_L, 0xf8 },
	};

	say("haptic: writing alioth static configuration; GO stays 0\n");
	for (unsigned i = 0; i < sizeof(config) / sizeof(config[0]); i++) {
		if (!put(config[i][0], config[i][1])) {
			return false;
		}
	}
	if (!calibrate_offset()) {
		return false;
	}
	/* The ADC is diagnostic and cannot start playback.  Measure even when UVLS
	 * is set, then retain the fault as a hard stop rather than hiding it. */
	return measure_vbat() && healthy("after static configuration, offset calibration and VBAT measurement", false);
}

/* Xiaomi's own ADC sequence; this measures VBAT and does not drive the motor. */
static bool
measure_vbat(void)
{
	uint8_t start = 0x02, control = 0, code = 0;
	bool done = false;

	if (!put(AW_GO, 0) || !put(AW_SYSCTRL, 0x63) || !regs_write(AW_DETCTRL, &start, 1)) {
		give_up("the VBAT measurement could not be started");
		return false;
	}
	for (unsigned i = 0; i < 1000; i++) {
		if (!get(AW_DETCTRL, &control)) {
			give_up("the VBAT measurement could not be polled");
			return false;
		}
		if ((control & 0x02) == 0) {
			done = true;
			break;
		}
		wait_ms(1);
	}
	if (!done || !get(AW_VBATDET, &code)) {
		give_up("the VBAT measurement did not complete");
		return false;
	}
	say("haptic: internal VBAT ADC code %#04x = %u mV (Android battery service is a separate measurement)\n",
	    code, (measured_vbat = 6100u * code / 256u));
	if (!put(AW_DETCTRL, 0) || !put(AW_SYSCTRL, 0x43)) {
		give_up("the chip could not be restored after VBAT measurement");
		return false;
	}
	return true;
}

/* Xiaomi's non-CONT_F0_CALI sequence, using alioth's own DTS parameters.
 * This drives the LRA in CONT/bypass mode to measure its resonance.  Unlike
 * offset calibration it can trip the same power faults as RAM playback, so
 * poll both live and latched fault bits every 5 ms and never mask UVLO. */
static bool
calibrate_f0(void)
{
	uint8_t cont = 0, datctrl = 0, anactrl = 0;
	uint8_t status = 0, flags = 0, state = 0xff, high = 0, low = 0;
	uint32_t f0_reg, f0;
	int step, trim;
	bool started = false, completed = false;

	if (!get(AW_CONT_CTRL, &cont) || !get(AW_DATCTRL, &datctrl) || !get(AW_ANACTRL, &anactrl)) {
		give_up("the f0 calibration registers could not be read");
		return false;
	}
	say("haptic: f0 calibration starts with CONT_CTRL %#04x, DATCTRL %#04x, ANACTRL %#04x\n",
	    cont, datctrl, anactrl);
	if (!put(AW_GO, 0) || !put(AW_TRIM_LRA, 0) ||
	    !put(AW_SYSCTRL, 0x49) || !put(AW_SYSCTRL, 0x48)) {
		give_up("CONT/bypass active mode could not be entered for f0 calibration");
		return false;
	}
	/* Clear transition flags after entering active mode, then unmask UVLO only;
	 * the other fault bits remain visible in SYSST/SYSINT even while masked. */
	if (!get(AW_SYSINT, &flags) || !put(AW_SYSINTM, 0x5f) ||
	    !put(AW_CONT_CTRL, (cont & ~0x08) | 0x04) ||
	    !put(AW_DATCTRL, datctrl | 0x60) || !put(AW_ANACTRL, anactrl | 0x20) ||
	    !put(AW_F_PRE_H, 0x08) || !put(AW_F_PRE_L, 0xd6) ||
	    !put(AW_DRV_LVL, 0x26) || !put(AW_NUM_F0_1, 0x93) ||
	    !put(AW_NUM_F0_2, 0x01) || !put(AW_NUM_F0_3, 0x1f)) {
		give_up("the native f0 calibration parameters could not be configured");
		return false;
	}
	if (!get(AW_SYSINT, &flags) || !put(AW_GO, 1)) {
		give_up("f0 calibration could not be started");
		return false;
	}
	say("haptic: f0 GO=1: preset 170.0 Hz, coeff 260, DRV_LVL 0x26, polling faults for at most 600 ms\n");
	for (unsigned waited = 0; waited <= 600; waited += 5) {
		if (!get(AW_SYSST, &status) || !get(AW_SYSINT, &flags) || !get(AW_GLB_STATE, &state)) {
			give_up("f0 calibration status could not be read");
			return false;
		}
		if ((status | flags) & SYSINT_FAULTS) {
			say("haptic: f0 calibration stopped after %u ms: SYSST %#04x, latched SYSINT %#04x, GLB_STATE %#04x, FAULTS %#04x\n",
			    waited, status, flags, state, (status | flags) & SYSINT_FAULTS);
			give_up("the chip reported a fault during f0 calibration");
			return false;
		}
		if ((state & 0x0f) != 0) {
			started = true;
		} else if (started && waited >= 110) {
			/* Xiaomi waits t_f0_trace_ms (110 ms with alioth's DTS values)
			 * before its first GLB_STATE check.  An earlier standby sample is
			 * only an intermediate phase and must not release a zero result. */
			completed = true;
			say("haptic: f0 calibration returned to standby after %u ms\n", waited);
			break;
		}
		wait_ms(5);
	}
	if (!completed) {
		say("haptic: f0 calibration %s; last GLB_STATE %#04x\n",
		    started ? "did not finish" : "never left standby", state);
		give_up("f0 calibration timed out");
		return false;
	}
	if (!get(AW_F_LRA_F0_H, &high) || !get(AW_F_LRA_F0_L, &low)) {
		give_up("the measured f0 could not be read");
		return false;
	}
	f0_reg = (uint32_t)high << 8 | low;
	if (f0_reg == 0) {
		give_up("f0 calibration returned a zero period");
		return false;
	}
	f0 = 1000000000u / (f0_reg * 260u);
	say("haptic: measured F_LRA_F0 %#06x = %u.%u Hz\n", f0_reg, f0 / 10, f0 % 10);
	if (f0 < 1581 || f0 > 1819) {
		give_up("the measured f0 is outside the native +/-7 percent limit");
		return false;
	}

	/* Preserve Xiaomi's C integer truncation and signed remainder rounding. */
	step = 100000 * ((int)f0 - 1700) / ((int)f0 * 25);
	if (step >= 0) {
		step = step / 10 + (step % 10 >= 5 ? 1 : 0) + 32;
	} else {
		step = 32 + step / 10 - (step % 10 <= -5 ? 1 : 0);
	}
	trim = step > 31 ? step - 32 : step + 32;
	say("haptic: native f0 trim calculation gives step %d, TRIM_LRA %#04x\n", step, trim);
	if (!put(AW_TRIM_LRA, (uint8_t)trim) || !put(AW_GO, 0) ||
	    !put(AW_SYSCTRL, 0x43) || !put(AW_SYSINTM, 0x7f) ||
	    !put(AW_CONT_CTRL, (cont | 0x08) & ~0x04) || !wait_standby(50)) {
		give_up("the chip could not be restored after f0 calibration");
		return false;
	}
	say("haptic: native CONT/bypass f0 calibration PASS: %u.%u Hz, TRIM_LRA %#04x\n",
	    f0 / 10, f0 % 10, trim);
	return healthy("after f0 calibration, back in RAM standby", false);
}

static bool
load_waveforms(void)
{
	uint64_t size = 0;
	const uint8_t *file = hakios_wifi_file("aw8697_haptic.bin", &size);
	uint32_t sum = 0, base, length;
	uint8_t back[32];

	if (file == NULL || size < 8) {
		say("haptic: aw8697_haptic.bin is absent from the small image's /fw\n");
		return false;
	}
	for (uint64_t i = 2; i < size; i++) {
		sum += file[i];
	}
	if ((sum & 0xffff) != (uint32_t)(file[0] << 8 | file[1])) {
		say("haptic: firmware checksum mismatch: actual %#x, header %#x\n", sum & 0xffff,
		    file[0] << 8 | file[1]);
		return false;
	}
	base = (uint32_t)(file[2] << 8 | file[3]);
	length = (uint32_t)size - 4;
	if (base != 0x0800 || length > 0x8000 - base) {
		say("haptic: implausible firmware base/length: %#x/%u\n", base, length);
		return false;
	}
	say("haptic: firmware checksum %#x OK: %u bytes at %#x, %u waveforms\n", sum & 0xffff,
	    length, base, (unsigned)(((file[5] << 8 | file[6]) - base - 1) / 4));
	if (!put(AW_GO, 0) || !wait_standby(250) || !put(AW_SYSCTRL, 0x63)) {
		give_up("the RAM could not be opened in standby");
		return false;
	}
	if (!put(AW_BASE_ADDRH, base >> 8) || !put(AW_BASE_ADDRL, base) ||
	    !put(AW_FIFO_AEH, (base >> 2) >> 8) || !put(AW_FIFO_AEL, base >> 2) ||
	    !put(AW_FIFO_AFH, (base - (base >> 2)) >> 8) || !put(AW_FIFO_AFL, base - (base >> 2)) ||
	    !put(AW_RAMADDRH, base >> 8) || !put(AW_RAMADDRL, base)) {
		give_up("the RAM address or FIFO thresholds could not be set");
		return false;
	}
	for (uint32_t at = 0; at < length; at += 32) {
		unsigned n = length - at < 32 ? length - at : 32;

		if (!regs_write(AW_RAMDATA, file + 4 + at, n)) {
			give_up("the waveform RAM write was not acknowledged");
			return false;
		}
	}
	if (!put(AW_RAMADDRH, base >> 8) || !put(AW_RAMADDRL, base)) {
		give_up("the RAM readback address could not be set");
		return false;
	}
	for (uint32_t at = 0; at < length; at += sizeof(back)) {
		unsigned n = length - at < sizeof(back) ? length - at : sizeof(back);

		if (!regs_read(AW_RAMDATA, back, n)) {
			give_up("the waveform RAM could not be read back");
			return false;
		}
		if (memcmp(back, file + 4 + at, n) != 0) {
			say("haptic: RAM mismatch near %#x\n", base + at);
			give_up("the waveform RAM readback differed");
			return false;
		}
	}
	if (!put(AW_SYSCTRL, 0x43) || !wait_standby(50)) {
		give_up("the chip did not return to standby after RAM loading");
		return false;
	}
	say("haptic: all %u RAM bytes read back identically; GLB state is standby, GO is 0\n", length);
	return measure_vbat() && healthy("after waveform loading and VBAT measurement", false);
}

/* Level 4: one waveform, once, only after native f0 calibration passed.
 * Active-mode UVLO is a hard stop, and every normal exit stops GO and returns
 * to standby.  A register-level PASS still is not a physical feel test. */
static bool
play_once(void)
{
	uint8_t flags = 0, state = 0xff;
	bool started = false, completed = false;

	if (!get(AW_SYSINT, &flags)) {
		give_up("the old interrupt flags could not be cleared before playback");
		return false;
	}
	say("haptic: cleared latched SYSINT %#04x before entering active mode\n", flags);
	if (!put(AW_SYSCTRL, 0x42)) {
		return false;
	}
	wait_ms(2);
	/* Xiaomi's active() clears after changing WORK_MODE, not before it: the
	 * standby-to-active transition itself can leave a stale UVLOI behind. */
	if (!get(AW_SYSINT, &flags)) {
		give_up("the transition interrupt flags could not be cleared in active mode");
		return false;
	}
	say("haptic: active-mode clear removed latched SYSINT %#04x; checking fresh flags\n", flags);
	if (!healthy("active, before GO", true)) {
		return false;
	}
	if (!put(AW_SYSINTM, 0x5f) || !put(AW_WAVSEQ1, 1) || !put(0x08, 0) ||
	    !put(AW_WAVLOOP1, 0) || !put(AW_DATDBG, 0x1e) || !put(AW_GO, 1)) {
		give_up("the single playback could not be started safely");
		return false;
	}
	say("haptic: GO=1: waveform 1, no loop, minimum Android effect gain 0x1e, watching faults for at most 1000 ms\n");
	for (unsigned waited = 0; waited <= 1000; waited += 5) {
		uint8_t status = 0;

		if (!get(AW_SYSST, &status) || !get(AW_SYSINT, &flags) || !get(AW_GLB_STATE, &state)) {
			give_up("playback status could not be read");
			return false;
		}
		if ((status | flags) & SYSINT_FAULTS) {
			say("haptic: playback stopped after %u ms by a fault\n", waited);
			(void)healthy("during playback", true);
			return false;
		}
		if ((state & 0x0f) != 0) {
			started = true;
		} else if (started) {
			completed = true;
			say("haptic: playback returned to standby after %u ms, SYSST %#04x\n", waited, status);
			break;
		}
		wait_ms(5);
	}
	if (!put(AW_GO, 0) || !put(AW_SYSCTRL, 0x43) || !put(AW_SYSINTM, 0x7f)) {
		give_up("the chip could not be stopped after playback");
		return false;
	}
	if (!completed || !wait_standby(50)) {
		give_up("playback timed out or did not return to standby");
		return false;
	}
	return healthy("after playback, back in standby", false);
}

static void
haptic_thread(__unused void *parameter, __unused wait_result_t result)
{
	tlmm_north = ml_io_map(TLMM_NORTH_PHYS, TLMM_SIZE);
	tlmm_south = ml_io_map(TLMM_SOUTH_PHYS, TLMM_SIZE);
	say("haptic: alioth AW8697 level %u (%s)\n", level,
	    level == 1 ? "read-only, reset unchanged" : level == 2 ? "hardware reset, then read-only register scan" :
	    level == 3 ? "static configuration and full RAM readback, nothing played" :
	    "native f0 calibration, then one waveform once with active fault checks");
	show_pins();
	if (level >= 2 && !boost_start()) {
		goto out;
	}
	if (level >= 2) {
		say("haptic: reset GPIO%u low for 5 ms, then high\n", PIN_RESET);
		reset_pin(false);
		wait_ms(5);
		reset_pin(true);
		wait_ms(8);
	}
	if (!chip_answers()) {
		say("haptic: reset %s\n", (rd(GPIO_CFG(PIN_RESET)) & CFG_OUTPUT) &&
		    (rd(GPIO_IO(PIN_RESET)) & 2) ? "is high; check power/bus" : "is not high; chip may be held in reset");
	} else {
		show_registers();
		if (level >= 3 && configure() &&
		    (level < 4 || calibrate_f0()) && load_waveforms()) {
			say("haptic: level 3 PASS: configured and verified without playing\n");
			if (level >= 4 && play_once()) {
				say("haptic: level 4 PASS: one waveform completed with no active fault\n");
			}
		}
	}
out:
	say("haptic: done; the iOS haptic service is not enabled in this build\n");
	thread_terminate(current_thread());
}

void
hakios_haptic_init(void)
{
	thread_t thread;

	if (!PE_parse_boot_argn("haptic", &level, sizeof(level)) || level == 0) {
		return;
	}
	if (level > 4) {
		level = 4;
	}
	if (kernel_thread_start(haptic_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}

void
hakios_haptic_quiesce(void)
{
	if (released && tlmm_south != 0) {
		leaving = true;
		reset_pin(false);
		say("haptic: run end: AW8697 reset GPIO%u is held low\n", PIN_RESET);
	}
	if (boost_we_enabled) {
		leaving = true;
		if (boost_power(false)) {
			boost_we_enabled = false;
			say("haptic: run end: PM8150B GPIO5 / vdd_hap_boost is low\n");
		}
	}
}

/* Kept for hakios.mic.c's optional microphone/haptic correlation test. */
void
hakios_haptic_pretend(__unused uint32_t peak)
{
}
