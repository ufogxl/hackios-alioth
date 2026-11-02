/*
 * The PCIe root complex of the Redmi K40 (sm8250), from nothing and step by step: the controller the
 * Wi-Fi's QCA6390 hangs off. Every address here is this phone's, and nothing is written unless a
 * step says it writes.
 *
 * The six windows are the device tree's (kona-pcie.dtsi:4, qcom,pcie@1c00000):
 *
 *   parf     0x01c00000  0x3000     the controller's own control and status words
 *   phy      0x01c06000  0x1000     the PHY
 *   dm_core  0x60000000  0xf1d      DBI: the root complex's own configuration space
 *   elbi     0x60000f20  0xa8       the ELBI
 *   iatu     0x60001000  0x1000     the address translation unit
 *   conf     0x60100000  0x100000   ECAM: the endpoints' configuration space
 *
 * and the register offsets inside them are drivers/pci/controller/pci-msm.c of the vendor's kernel
 * for this SoC (compatible "qcom,pci-msm", the same tree, k40-ref): PCIE20_PARF_SYS_CTRL 0x00,
 * PARF_PHY_CTRL 0x40, PARF_MHI_CLOCK_RESET_CTRL 0x174, PARF_AXI_MSTR_WR_ADDR_HALT 0x1a8,
 * PARF_LTSSM 0x1b0 (LTSSM_EN bit 8), PARF_INT_ALL_STATUS 0x224, PARF_STATUS 0x230,
 * PARF_DBI_BASE_ADDR 0x350, PARF_SLV_ADDR_SPACE_SIZE 0x358, PARF_DEVICE_TYPE 0x1000;
 * PCIE20_ELBI_SYS_CTRL 0x04 and ELBI_SYS_STTS 0x08; and in DBI, PCIE20_CAP 0x70 with
 * LINKCTRLSTATUS at CAP + 0x10.
 *
 * Boot-arg pcie_step=N does steps 1 to N and stops; a step says what it is about to read before it
 * reads it. Step 1 reads and writes nothing.
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <mach/vm_param.h>
#include <pexpert/pexpert.h>
#include <sys/systm.h>
#include <vm/memory_types.h>
#include <vm/vm_kern.h>

#include "hakios.rpmh.h"

/* (hakios.display.c declares these the same way: the header that has them is not one the drivers take) */
extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern kern_return_t thread_terminate(thread_t thread);
extern void loader_log_putc(char c);
extern boolean_t ml_get_interrupts_enabled(void);

/*
 * Everything this file says goes through here: a character at a time into the LOADER's log, and to
 * the kernel's own when interrupts are on.
 *
 * Why not printf alone: when one of these experiments stops the machine, the kernel's log stops with
 * it and the loader's poll of it keeps handing back whatever was last flushed -- so the lines that
 * say how far it got are the ones lost. The loader's own log is in memory the loader owns, and it
 * survives. (hakios.cpufreq.c does the same for the same reason.)
 */
static void
tell(const char *format, ...) __printflike(1, 2);

static void
tell(const char *format, ...)
{
	char line[256];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	for (const char *c = line; *c != 0; c++) {
		loader_log_putc(*c);
	}
	if (ml_get_interrupts_enabled()) {
		printf("%s", line);
	}
}

#define PCIE_PARF_PHYS          0x01c00000UL
#define PCIE_PARF_SIZE          0x00003000UL
#define PCIE_PHY_PHYS           0x01c06000UL
#define PCIE_PHY_SIZE           0x00001000UL
#define PCIE_DM_CORE_PHYS       0x60000000UL
#define PCIE_DM_CORE_SIZE       0x00001000UL
#define PCIE_ELBI_PHYS          0x60000f20UL
#define PCIE_ELBI_SIZE          0x00000100UL

/* pci-msm.c: the words that say whether the controller is out of reset and where the link is. */
#define PARF_SYS_CTRL           0x000
#define PARF_PM_CTRL            0x020
#define PARF_PM_STTS            0x024
#define PARF_PHY_CTRL           0x040
#define PARF_MHI_CLOCK_RESET    0x174
#define PARF_AXI_MSTR_HALT      0x1a8
#define PARF_LTSSM              0x1b0           /* LTSSM_EN bit 8; the state itself is the low bits */
#define PARF_INT_ALL_STATUS     0x224
#define PARF_INT_ALL_MASK       0x22c           /* (write 0: everything masked) */
#define PARF_STATUS             0x230           /* FLUSH_COMPLETED bit 8 */
#define PARF_DBI_BASE_ADDR      0x350
#define PARF_SLV_ADDR_SPACE     0x358
#define PARF_DEVICE_TYPE        0x1000

#define ELBI_SYS_CTRL           0x004
#define ELBI_SYS_STTS           0x008

/* DBI, the root complex's own configuration space */
#define DBI_ID                  0x000           /* vendor and device: 0x17cb is Qualcomm */
#define DBI_COMMAND_STATUS      0x004
#define DBI_HEADER_TYPE         0x00c
#define DBI_CAP                 0x070
#define DBI_LINK_CTRL_STATUS    (DBI_CAP + 0x10) /* [3:0] speed, [9:4] width, bit 29 data link active */
#define DBI_LINK_CTRL2_STATUS2  (DBI_CAP + 0x30)

/*
 * The PHY's bring-up table: the device tree's qcom,phy-sequence of qcom,pcie@1c00000, all 99
 * triples of it, in its order (device-tree.dtb, the phone's own copy -- 1,521,101 bytes of dts when
 * decompiled; the property is the one on line 10882 of that dump). Every offset is inside the
 * 0x1000 of the phy window at 0x1c06000 and none of them carries a delay.
 *
 * The vendor's driver writes them one by one before it reads anything of the controller's
 * configuration space (pci-msm.c:3713-3733, pcie_phy_init), and that is what this is for: the PHY
 * supplies the core's PIPE clock, and without it a read of the DBI never completes -- which is
 * exactly what step 7 did, twice, with every PARF register already at the value the vendor leaves.
 */
struct pcie_phy_step {
	uint32_t offset;
	uint32_t value;
	uint32_t delay;         /* (microseconds, always zero in this table) */
};

/* the two the device tree names: qcom,phy-status-offset/-bit and qcom,phy-power-down-offset */
#define PHY_STATUS_OFFSET       0x814
#define PHY_STATUS_BIT          6               /* (set means "not ready": pci-msm.c:3700) */
#define PHY_POWER_DOWN_OFFSET   0x840
#define PHY_INFO_OFFSET         0x8dc           /* (the table's own 0x8dc 0x0d; printed for the record) */

static const struct pcie_phy_step pcie_phy_sequence[] = {
	{ 0x840, 0x03, 0 },
	{ 0x094, 0x08, 0 },
	{ 0x154, 0x34, 0 },
	{ 0x16c, 0x08, 0 },
	{ 0x058, 0x0f, 0 },
	{ 0x0a4, 0x42, 0 },
	{ 0x110, 0x24, 0 },
	{ 0x11c, 0x03, 0 },
	{ 0x118, 0xb4, 0 },
	{ 0x10c, 0x02, 0 },
	{ 0x1bc, 0x11, 0 },
	{ 0x0bc, 0x82, 0 },
	{ 0x0d4, 0x03, 0 },
	{ 0x0d0, 0x55, 0 },
	{ 0x0cc, 0x55, 0 },
	{ 0x0b0, 0x1a, 0 },
	{ 0x0ac, 0x0a, 0 },
	{ 0x0c4, 0x68, 0 },
	{ 0x0e0, 0x02, 0 },
	{ 0x0dc, 0xaa, 0 },
	{ 0x0d8, 0xab, 0 },
	{ 0x0b8, 0x34, 0 },
	{ 0x0b4, 0x14, 0 },
	{ 0x158, 0x01, 0 },
	{ 0x074, 0x06, 0 },
	{ 0x07c, 0x16, 0 },
	{ 0x084, 0x36, 0 },
	{ 0x078, 0x06, 0 },
	{ 0x080, 0x16, 0 },
	{ 0x088, 0x36, 0 },
	{ 0x1b0, 0x1e, 0 },
	{ 0x1ac, 0xca, 0 },
	{ 0x1b8, 0x18, 0 },
	{ 0x1b4, 0xa2, 0 },
	{ 0x050, 0x07, 0 },
	{ 0x010, 0x01, 0 },
	{ 0x01c, 0x31, 0 },
	{ 0x020, 0x01, 0 },
	{ 0x024, 0xde, 0 },
	{ 0x028, 0x07, 0 },
	{ 0x030, 0x4c, 0 },
	{ 0x034, 0x06, 0 },
	{ 0x29c, 0x12, 0 },
	{ 0x284, 0x35, 0 },
	{ 0x23c, 0x11, 0 },
	{ 0x51c, 0x03, 0 },
	{ 0x518, 0x1c, 0 },
	{ 0x524, 0x1e, 0 },
	{ 0x4e8, 0x00, 0 },
	{ 0x4ec, 0x0e, 0 },
	{ 0x4f0, 0x4a, 0 },
	{ 0x4f4, 0x0f, 0 },
	{ 0x5b4, 0x04, 0 },
	{ 0x434, 0x7f, 0 },
	{ 0x444, 0x70, 0 },
	{ 0x510, 0x17, 0 },
	{ 0x4d4, 0x04, 0 },
	{ 0x4d8, 0x07, 0 },
	{ 0x598, 0xd4, 0 },
	{ 0x59c, 0x54, 0 },
	{ 0x5a0, 0xdb, 0 },
	{ 0x5a4, 0x3b, 0 },
	{ 0x5a8, 0x31, 0 },
	{ 0x584, 0x24, 0 },
	{ 0x588, 0xe4, 0 },
	{ 0x58c, 0xec, 0 },
	{ 0x590, 0x3b, 0 },
	{ 0x594, 0x36, 0 },
	{ 0x570, 0x3f, 0 },
	{ 0x574, 0x3f, 0 },
	{ 0x578, 0xff, 0 },
	{ 0x57c, 0x7f, 0 },
	{ 0x580, 0x14, 0 },
	{ 0x4fc, 0x00, 0 },
	{ 0x4f8, 0xc0, 0 },
	{ 0x460, 0x30, 0 },
	{ 0x464, 0x00, 0 },
	{ 0x5bc, 0x0c, 0 },
	{ 0x4dc, 0x1b, 0 },
	{ 0x408, 0x0c, 0 },
	{ 0x414, 0x03, 0 },
	{ 0x5b8, 0x30, 0 },
	{ 0x9a4, 0x01, 0 },
	{ 0xc90, 0x00, 0 },
	{ 0xc40, 0x01, 0 },
	{ 0xc48, 0x01, 0 },
	{ 0xc50, 0x00, 0 },
	{ 0xcb4, 0x33, 0 },
	{ 0xcbc, 0x00, 0 },
	{ 0xce0, 0x58, 0 },
	{ 0xca4, 0x0f, 0 },
	{ 0x048, 0x90, 0 },
	{ 0xc1c, 0xc1, 0 },
	{ 0x988, 0x77, 0 },
	{ 0x998, 0x0b, 0 },
	{ 0x8dc, 0x0d, 0 },
	{ 0x9ec, 0x12, 0 },
	{ 0x800, 0x00, 0 },
	{ 0x844, 0x03, 0 },
};

static vm_offset_t parf, phy, dbi, elbi;
static void step_setup(uint32_t steps);  /* (defined below, used by the runner above it) */
static uint32_t step;

static inline uint32_t
rd(vm_offset_t address)
{
	return *(volatile uint32_t *) address;
}

static inline void
wr(vm_offset_t address, uint32_t value)
{
	*(volatile uint32_t *) address = value;
}

static bool
pcie_map(void)
{
	if (parf != 0) {
		return true;
	}
	tell("pcie: about to map the controller's windows\n");
	parf = ml_io_map(PCIE_PARF_PHYS, PCIE_PARF_SIZE);
	if (parf == 0) {
		tell("pcie: parf (%#lx) could not be mapped\n", (unsigned long) PCIE_PARF_PHYS);
		return false;
	}
	tell("pcie: parf at %#lx\n", parf);
	phy = ml_io_map(PCIE_PHY_PHYS, PCIE_PHY_SIZE);
	tell("pcie: phy at %#lx\n", phy);
	dbi = ml_io_map(PCIE_DM_CORE_PHYS, PCIE_DM_CORE_SIZE);
	tell("pcie: dbi at %#lx\n", dbi);
	elbi = ml_io_map(PCIE_ELBI_PHYS, PCIE_ELBI_SIZE);
	tell("pcie: elbi at %#lx\n", elbi);
	return true;
}

/*
 * Step 1: the controller's power switch, and nothing else.
 *
 * A first attempt read the controller's own registers with no power step, and the machine stopped
 * dead -- 270 bytes of log, not one pcie: line. An unpowered block on this interconnect does not
 * answer: the read never completes. The vendor's driver powers the switch before it touches
 * anything (pci-msm.c:3453, regulator_enable(dev->gdsc)), and so does this.
 *
 * The switch is a GCC GDSC: qcom,gdsc@16b004 in the device tree (kona.dtsi:2567), that is GCC +
 * 0x6b004, and GCC's window is the one hakios.adreno.c already uses (0x00100000, 0x80000). A GDSCR
 * is the register that every GDSC in this SoC has: bit 0 SW_COLLAPSE turns it off by software and
 * bit 31 POWER_ON says it is up (gdsc.c, and the two constants hakios.adreno.c defines for the
 * same job).
 *
 * Nothing about the PCIe controller itself is read here: this step only says whether the switch can
 * be turned on at all. It is left on, with the value it had remembered so that a later step can put
 * it back.
 */
static uint32_t gdsc_was;

static void
step_power(void)
{
	vm_offset_t gcc = ml_io_map(0x00100000UL, 0x00080000UL);
	uint32_t gdscr;

	if (gcc == 0) {
		tell("pcie: 1: GCC could not be mapped\n");
		return;
	}
	gdscr = rd(gcc + 0x6b004);
	gdsc_was = gdscr;
	tell("pcie: 1: pcie_0_gdsc %#x -> %s (power on %u, sw collapse %u, hw ctrl %u)\n", gdscr,
	    (gdscr & (1u << 31)) ? "ON" : "off", (gdscr >> 31) & 1, gdscr & 1, (gdscr >> 16) & 1);
	if ((gdscr & (1u << 31)) == 0) {
		uint64_t give_up;
		uint32_t now = gdscr & ~1u;                     /* SW_COLLAPSE cleared: on, by software */

		wr(gcc + 0x6b004, now);
		nanoseconds_to_absolutetime(100ull * NSEC_PER_MSEC, &give_up);
		give_up += mach_absolute_time();
		do {
			gdscr = rd(gcc + 0x6b004);
		} while ((gdscr & (1u << 31)) == 0 && mach_absolute_time() < give_up);
		tell("pcie: 1: after writing %#x it reads %#x -> %s\n", now, gdscr,
		    (gdscr & (1u << 31)) ? "THE SWITCH IS ON" : "IT DID NOT COME ON");
	}
}

/*
 * Step 2: the clocks, which are what the block answers through.
 *
 * With the power switch on and nothing else, reading PARF_SYS_CTRL did not hang the bus as an
 * unpowered block does -- it came back as an external abort ("Unhandled implementation specific
 * error"), which is a block that exists and will not answer. The vendor's driver enables eleven
 * clocks and two resets before it touches the controller (pci-msm.c:3505, and the list is the
 * device tree's kona-pcie.dtsi:57-84); this is the clocks.
 *
 * Six of them share one register, GCC + 0x52008, a bit each (gcc-kona.c: the clk_branch entries'
 * enable_reg and enable_mask):
 *
 *   bit 0  gcc_pcie_0_slv_axi_clk        bit 3  gcc_pcie_0_aux_clk
 *   bit 1  gcc_pcie_0_mstr_axi_clk       bit 4  gcc_pcie_0_pipe_clk
 *   bit 2  gcc_pcie_0_cfg_ahb_clk        bit 5  gcc_pcie_0_slv_q2a_axi_clk
 *
 * and four have a register of their own (their CBCR, bit 0):
 *
 *   0x6f02c  gcc_pcie0_phy_refgen_clk    0x9000c  gcc_aggre_noc_pcie_tbu_clk
 *   0x8c004  gcc_pcie_wifi_clkref_en     0x8d058  gcc_ddrss_pcie_sf_tbu_clk
 *
 * Each branch also has a halt register that says whether it is actually running (halt_reg, bit 2
 * set means halted); those are read back and printed, because a vote that is written and a clock
 * that runs are not the same thing.
 *
 * What this step does NOT do: the two resets (GCC_PCIE_0_BCR, GCC_PCIE_0_PHY_BCR) and the two
 * rails (pm8150_l9 at 1.8 V, pm8150_l5 at 0.9 V). If the read still fails after the clocks, those
 * are next.
 */
static const struct {
	uint32_t at;                            /* in GCC */
	uint32_t bit;
	const char *name;
} pcie_clock_gates[] = {
	{ 0x6f02c, 0, "pcie0_phy_refgen_clk" },
	{ 0x6f004, 0, "pcie_phy_aux_clk" },    /* the PHY's own AHB clock: not in the device tree's list */
	{ 0x8c004, 0, "pcie_wifi_clkref_en" },
	{ 0x9000c, 0, "aggre_noc_pcie_tbu_clk" },
	{ 0x8d058, 0, "ddrss_pcie_sf_tbu_clk" },
};

static const struct {
	uint32_t at;                            /* the branch's halt register, in GCC */
	uint32_t bit;                           /* its enable bit in 0x52008 */
	const char *name;
} pcie_0_branches[] = {
	{ 0x6b014, 0, "slv_axi_clk" },
	{ 0x6b01c, 1, "mstr_axi_clk" },
	{ 0x6b024, 2, "cfg_ahb_clk" },
	{ 0x6b028, 3, "aux_clk" },
	{ 0x6b02c, 4, "pipe_clk" },
	{ 0x6b010, 5, "slv_q2a_axi_clk" },
};

#define GCC_PCIE_0_VOTE        0x52008

static vm_offset_t gcc_window;

static void
step_clocks(void)
{
	uint32_t vote;

	if (gcc_window == 0) {
		/*
		 * One megabyte, not the eight hundred kilobytes hakios.adreno.c maps: the gates this file
		 * uses run from +0x52008 to +0x9000c, and the first attempt at the tail of that range
		 * faulted inside the last loop -- a data abort at +0x8c004, before the controller was even
		 * looked at.
		 */
		gcc_window = ml_io_map(0x00100000UL, 0x00100000UL);
	}
	if (gcc_window == 0) {
		tell("pcie: 2: GCC could not be mapped\n");
		return;
	}
	vote = rd(gcc_window + GCC_PCIE_0_VOTE);
	tell("pcie: 2: gcc +%#x (the six PCIe 0 branches) reads %#x; setting the low six bits\n",
	    GCC_PCIE_0_VOTE, vote);
	wr(gcc_window + GCC_PCIE_0_VOTE, vote | 0x3f);
	tell("pcie: 2: gcc +%#x now reads %#x\n", GCC_PCIE_0_VOTE, rd(gcc_window + GCC_PCIE_0_VOTE));
	for (unsigned i = 0; i < sizeof(pcie_0_branches) / sizeof(pcie_0_branches[0]); i++) {
		uint32_t halt = rd(gcc_window + pcie_0_branches[i].at);

		tell("pcie: 2:   %-16s halt register %#x: %#x -> %s\n", pcie_0_branches[i].name,
		    pcie_0_branches[i].at, halt, (halt >> 2) & 1 ? "halted" : "running");
	}
	for (unsigned i = 0; i < sizeof(pcie_clock_gates) / sizeof(pcie_clock_gates[0]); i++) {
		uint32_t was = rd(gcc_window + pcie_clock_gates[i].at);

		wr(gcc_window + pcie_clock_gates[i].at, was | (1u << pcie_clock_gates[i].bit));
		tell("pcie: 2:   %-22s %#x: %#x -> %#x\n", pcie_clock_gates[i].name,
		    pcie_clock_gates[i].at, was, rd(gcc_window + pcie_clock_gates[i].at));
	}
}

/*
 * Step 2, read-only: is the controller out of reset, and where is the link?
 *
 * The most telling word is the DBI's own vendor and device identifier: it is a constant the core
 * answers with only when it is powered and its reset is released, so 0x17cb... says the block is
 * alive while 0 or 0xffffffff says it is not -- which is the difference between "the link did not
 * train" and "there was nothing there to train it".
 */
/*
 * Step 3: the two resets, which keep the block in a state that does not answer.
 *
 * With the switch on and the clocks running, reading PARF still faulted (a kernel data abort).
 * What the vendor's driver does next is deassert the pair the device tree names -- pcie_0_core_reset
 * and pcie_0_phy_reset, that is GCC_PCIE_0_BCR and GCC_PCIE_0_PHY_BCR (pci-msm.c:
 * reset_control_deassert, before any register is read).
 *
 * Both are one word each in the GCC reset table (gcc-kona.c:4270, the qcom_reset_map array):
 * GCC_PCIE_0_BCR at +0x6b000 and GCC_PCIE_0_PHY_BCR at +0x6c01c, bit 0 asserted. Their neighbours
 * are printed too, because a link that refuses to train may want them: LINK_DOWN at +0x6c014 and
 * NOCSR_COM_PHY at +0x6c020.
 */
static const struct {
	uint32_t at;
	const char *name;
} pcie_resets[] = {
	{ 0x6b000, "pcie_0_core_reset" },
	{ 0x6c01c, "pcie_0_phy_reset" },
};

static void
step_resets(void)
{
	if (gcc_window == 0) {
		step_clocks();          /* (maps GCC; harmless when the clocks are already on) */
	}
	if (gcc_window == 0) {
		return;
	}
	for (unsigned i = 0; i < sizeof(pcie_resets) / sizeof(pcie_resets[0]); i++) {
		uint32_t was = rd(gcc_window + pcie_resets[i].at);
		uint32_t now = was & ~1u;

		wr(gcc_window + pcie_resets[i].at, now);
		tell("pcie: 4:   %s %#x: %#x -> %#x (%s)\n", pcie_resets[i].name, pcie_resets[i].at,
		    was, rd(gcc_window + pcie_resets[i].at), (was & 1) ? "was asserted" : "was already out");
	}
	tell("pcie: 4:   link_down %#x, nocsr_com_phy %#x\n", rd(gcc_window + 0x6c014),
	    rd(gcc_window + 0x6c020));
}

static void
step_read(void)
{
	uint32_t v;

	tell("pcie: 4: reading parf sys_ctrl (%#x) -- if nothing follows this line, that read hung\n", PARF_SYS_CTRL);
	v = rd(parf + PARF_SYS_CTRL);
	tell("pcie: 4: parf sys_ctrl %#x\n", v);
	tell("pcie: 4: reading parf pm_ctrl (%#x)\n", PARF_PM_CTRL);
	tell("pcie: 4: parf pm_ctrl %#x, pm_stts %#x\n", rd(parf + PARF_PM_CTRL), rd(parf + PARF_PM_STTS));
	tell("pcie: 4: reading parf ltssm (%#x)\n", PARF_LTSSM);
	v = rd(parf + PARF_LTSSM);
	tell("pcie: 4: parf ltssm %#x -> enabled %u, state %#x\n", v, (v >> 8) & 1, v & 0x3f);
	tell("pcie: 4: phy_ctrl %#x, mhi_clock_reset %#x, axi_mstr_halt %#x, status %#x\n",
	    rd(parf + PARF_PHY_CTRL), rd(parf + PARF_MHI_CLOCK_RESET), rd(parf + PARF_AXI_MSTR_HALT),
	    rd(parf + PARF_STATUS));
	tell("pcie: 4: dbi_base %#x, slv_addr_space %#x, device_type %#x, int_all_status %#x\n",
	    rd(parf + PARF_DBI_BASE_ADDR), rd(parf + PARF_SLV_ADDR_SPACE), rd(parf + PARF_DEVICE_TYPE),
	    rd(parf + PARF_INT_ALL_STATUS));
}

static void
pcie_run(uint32_t steps)
{

	if (steps == 0) {
		return;
	}
	step = 1;
	tell("pcie: step %u of the PCIe root complex\n", steps);
	if (steps >= 1) {
		step_power();
	}
	if (steps >= 2) {
		step_clocks();
	}
	if (steps >= 3) {
		step_resets();
	}
	if (steps < 4) {
		tell("pcie: stopping here (pcie_step=%u)\n", steps);
		return;
	}
	if (parf == 0 && !pcie_map()) {
		return;
	}
	step_read();
	if (steps >= 5) {
		step_setup(steps);              /* (the windows are mapped by now: see above) */
	}
}

/*
 * Boot-arg pcie_at=N: the same steps, N seconds in, on a thread of their own.
 *
 * Everything here otherwise runs inside bsd_init, which the loader sees as the first two seconds of a
 * log that is already a hundred kilobytes: its lines are gone by the time the log is read back (only
 * the last 4 KB of each snapshot is kept). Waiting puts the report where it can be read.
 */
static void
pcie_thread(void * parameter, __unused wait_result_t result)
{
	uint32_t seconds = (uint32_t)(uintptr_t) parameter;
	uint32_t steps = 0;

	delay_for_interval(seconds, NSEC_PER_SEC);
	if (PE_parse_boot_argn("pcie_step", &steps, sizeof(steps)) && steps != 0) {
		pcie_run(steps);
	}
	thread_terminate(current_thread());
}

/*
 * Step 5: the two words the vendor's driver writes before it touches the link, and the DBI window.
 *
 * They are the same two the reference port's step 9 wrote -- PARF_MHI_CLOCK_RESET_CTRL from 3 to
 * 0x13, and the master's write-address halt left alone at its 0x80000014 -- plus the one they never
 * reached: PARF_DBI_BASE_ADDR, which is 0 here. With it zero there is no window in which the root
 * complex's own configuration space can be read, which is the next thing to want (vendor: pci-msm.c,
 * msm_pcie_init_parf / msm_pcie_config_dbi).
 *
 * Written last, dbi: whatever goes wrong after it can be seen in one boot instead of two.
 */
/*
 * Step 8: the PHY (pci-msm.c:3713-3766, pcie_phy_init).
 *
 * The table goes in first, then a millisecond for it to settle, then a poll of one bit. Everything
 * about the poll is the vendor's: an offset of 0x814 and bit 6, both from this phone's device tree;
 * and the bit means the opposite of what the name suggests -- pcie_phy_is_ready() returns false
 * while bit 6 is *set*, so the PHY is up when it reads back clear. Ten tries a millisecond apart is
 * the vendor's PHY_READY_TIMEOUT_COUNT, and a PHY that misses it is as good as absent.
 *
 * The pipe clock is the one thing here that is not written: msm_pcie_pipe_clk_init() enables
 * dev->pipeclk[], and this root complex's single pipe clock is GCC's pcie_0_pipe_clk (+0x52008,
 * bit 4), which step 2 already turned on; its reset is GCC_PCIE_0_PHY_BCR of step 3. They are
 * printed, not rewritten.
 */
static void
step_phy(void)
{
	unsigned i, tries;
	uint32_t v;

	if (phy == 0) {
		tell("pcie: 8: the phy window (%#lx) was not mapped; nothing to write\n",
		    (unsigned long) PCIE_PHY_PHYS);
		return;
	}
	tell("pcie: 8: writing the device tree's %u phy triples into %#lx\n",
	    (unsigned)(sizeof(pcie_phy_sequence) / sizeof(pcie_phy_sequence[0])),
	    (unsigned long) PCIE_PHY_PHYS);
	for (i = 0; i < sizeof(pcie_phy_sequence) / sizeof(pcie_phy_sequence[0]); i++) {
		wr(phy + pcie_phy_sequence[i].offset, pcie_phy_sequence[i].value);
		if (pcie_phy_sequence[i].delay != 0) {
			delay_for_interval(pcie_phy_sequence[i].delay, NSEC_PER_USEC);
		}
	}
	tell("pcie: 8: table written; %#x = %#x, %#x = %#x, %#x = %#x\n",
	    pcie_phy_sequence[0].offset, rd(phy + pcie_phy_sequence[0].offset),
	    PHY_POWER_DOWN_OFFSET, rd(phy + PHY_POWER_DOWN_OFFSET),
	    pcie_phy_sequence[sizeof(pcie_phy_sequence) / sizeof(pcie_phy_sequence[0]) - 1].offset,
	    rd(phy + pcie_phy_sequence[sizeof(pcie_phy_sequence) / sizeof(pcie_phy_sequence[0]) - 1].offset));

	delay_for_interval(1000, NSEC_PER_USEC);        /* (PHY_STABILIZATION_DELAY_US: 995 to 1005) */
	if (gcc_window != 0) {
		tell("pcie: 8: pipe clk vote %#x (bit 4 set in step 2), phy reset %#x (step 3)\n",
		    rd(gcc_window + GCC_PCIE_0_VOTE), rd(gcc_window + 0x6c01c));
	}
	tell("pcie: 8: about to read phy_status (%#x + %#x, bit %u) -- if nothing follows, that read hung\n",
	    (unsigned long) PCIE_PHY_PHYS, PHY_STATUS_OFFSET, PHY_STATUS_BIT);
	for (tries = 1; tries <= 10; tries++) {
		v = rd(phy + PHY_STATUS_OFFSET);
		tell("pcie: 8:   try %u: phy_status %#x, bit %u %s\n", tries, v, PHY_STATUS_BIT,
		    ((v >> PHY_STATUS_BIT) & 1) ? "still set" : "clear: phy is ready");
		if (((v >> PHY_STATUS_BIT) & 1) == 0) {
			break;
		}
		delay_for_interval(1000, NSEC_PER_USEC);
	}
	tell("pcie: 8: phy %s (info %#x), step 9 reads the dbi next\n",
	    (((rd(phy + PHY_STATUS_OFFSET) >> PHY_STATUS_BIT) & 1) == 0) ? "ready" : "NOT ready",
	    rd(phy + PHY_INFO_OFFSET));
}

/*
 * Steps 10 to 12: the link. One thing a step, and in the order that isolates rather than the order
 * the vendor takes them in.
 *
 * Why the order is turned around: the first attempt did the pins and the LTSSM enable together, and
 * the machine reset within forty seconds -- and a reset is worse than a hang, because the loader
 * restarts with it and the log of that run goes with the memory (xl fetched 64 KB of 0x98 bytes).
 * So the LTSSM enable, which touches nothing but the controller's own PARF, goes first; the pins
 * afterwards; the ELBI last, because it is the one window fed by the link itself.
 */
static void
step_pins(uint32_t steps)
{
	static const struct {
		unsigned pin;
		const char *what;
	} pins[] = {
		{ 84, "what the radio's pinctrl description calls cnss_wlan_en" },
		{ 79, "perst" },                /* dt: qcom,pcie@1c00000, perst-gpio = <&tlmm 79 0> */
		{ 81, "wake" },                 /* dt: the same node, wake-gpio = <&tlmm 81 0> (an interrupt) */
		{ 20, "wlan-en" },              /* dt: qcom,cnss-qca6390@b0000000, wlan-en-gpio = <&tlmm 20 0> */
	};
	vm_offset_t tlmm;
	uint32_t v;
	unsigned i;

	/*
	 * TLMM is not "one page a pin above 0xf100000": it is a 16 MB window at 0xf000000 with three
	 * tiles in it, and a pin's registers are at tile_base + 0x1000 * pin (pinctrl-kona.c:20-24:
	 * NORTH 0x00900000, SOUTH 0x00500000, WEST 0x00100000, all offsets inside that window).
	 *
	 * All four pins here are NORTH (pinctrl-kona.c:1461, 1576, 1580, 1586 -- pin 79's function list
	 * even has pci_e0), so they are at 0x0f900000 + 0x1000 * n. The first attempt at this step used
	 * 0xf100000 + 0x1000 * n, which is the WEST tile -- that is, it read and wrote four completely
	 * different pins, and driving one of those (whatever WEST + 20 is) is the best explanation there
	 * is for a machine that resets within forty seconds leaving no log at all.
	 *
	 * Pin 84 is read first and pin 20 last on purpose: 84 is the one the Wi-Fi driver has already
	 * read on this phone, so if this step does stop the machine, the last line says which pin did it.
	 */
#define TLMM_NORTH_PHYS 0x0f900000UL
#define TLMM_SIZE       0x0007d000UL    /* (pins 0 to 124 of the north tile) */
#define GPIO_CFG(n)     (0x1000 * (n))
#define GPIO_IO(n)      (0x1000 * (n) + 4)
#define GPIO_OE_MASK    0x200u          /* (bit 9: output enable; `on` high means a real output) */
#define GPIO_LEVEL      1               /* (bit 1 of the IO register: the level, when an output) */
#define GPIO_DRIVE(n)   (((unsigned)(n) & 7u) << 6)     /* ([8:6] drive strength: 2 mA times n + 1) */
#define GPIO_PULL_UP    0x3u            /* ([1:0]: 0 none, 1 down, 2 keeper, 3 up) */
#define GPIO_NO_PULL    0x0u

	tlmm = ml_io_map(TLMM_NORTH_PHYS, TLMM_SIZE);
	if (tlmm == 0) {
		tell("pcie: 11: the north tile (%#lx) could not be mapped\n", (unsigned long) TLMM_NORTH_PHYS);
		return;
	}
	tell("pcie: 11: north tile at %#lx\n", tlmm);
	for (i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) {
		uint32_t cfg, io;

		tell("pcie: 11: reading gpio%u (%s)\n", pins[i].pin, pins[i].what);
		cfg = rd(tlmm + GPIO_CFG(pins[i].pin));
		io = rd(tlmm + GPIO_IO(pins[i].pin));
		tell("pcie: 11:   gpio%u: config %#x (function %u, %s), level %u\n", pins[i].pin, cfg,
		    (cfg >> 2) & 15, (cfg & GPIO_OE_MASK) ? ((io & 2) ? "drives high" : "drives low") : "an input", io & 1);
	}

	if (steps < 12) {
		tell("pcie: 11: only reads; step 12 drives perst and wlan-en\n");
		return;
	}

	/*
	 * wlan-en first now, and alone: the reads above say the firmware left every one of these pads
	 * unconfigured (config 0, an input, level 0), so the QCA6390 is off. Raising its enable with no
	 * PERST change tells whether a GPIO write here is safe at all; PERST is the one that lets the
	 * link try to train, and a training attempt against a dead endpoint is the other candidate for
	 * what stopped the machine at 21:58.
	 */
	tell("pcie: 12: driving gpio20 (wlan-en) high -- the device tree's cnss_wlan_en_active: function gpio,\n"
	    "pcie: 12:   16 mA, pull-up, output high (drive 7 = 16 mA, pull 3, output enable, level high)\n");
	wr(tlmm + GPIO_CFG(20), GPIO_DRIVE(7) | GPIO_PULL_UP | GPIO_OE_MASK);
	wr(tlmm + GPIO_IO(20), rd(tlmm + GPIO_IO(20)) | (1u << GPIO_LEVEL));
	tell("pcie: 12: gpio20 (wlan-en) now config %#x, level %u (%s)\n", rd(tlmm + GPIO_CFG(20)),
	    rd(tlmm + GPIO_IO(20)) & 1, (rd(tlmm + GPIO_IO(20)) & 2) ? "high" : "low");
	if (steps < 13) {
		tell("pcie: 12: stopping here; step 13 drives perst\n");
		return;
	}

	/*
	 * perst: asserted, held for the device tree's qcom,ep-latency (10 ms), released -- the order the
	 * vendor keeps (assert at the top of msm_pcie_alloc_resources, release after the PHY is up:
	 * pci-msm.c:4497 and 4581). perst-gpio is <&tlmm 79 0>, active high, so `on` -- asserted -- is
	 * high.
	 */
	tell("pcie: 13: asserting gpio79 (perst)\n");
	wr(tlmm + GPIO_CFG(79), GPIO_DRIVE(0) | GPIO_NO_PULL | GPIO_OE_MASK);
	wr(tlmm + GPIO_IO(79), (rd(tlmm + GPIO_IO(79)) & ~2u) | 2u);
	tell("pcie: 13: gpio79 (perst) asserted (high); ep latency 10 ms, then released\n");
	delay_for_interval(10000, NSEC_PER_USEC);
	wr(tlmm + GPIO_IO(79), rd(tlmm + GPIO_IO(79)) & ~2u);
	tell("pcie: 13: gpio79 (perst) released (low), level %u\n", rd(tlmm + GPIO_IO(79)) & 1);
#undef TLMM_NORTH_PHYS
#undef TLMM_SIZE
#undef GPIO_CFG
#undef GPIO_IO
#undef GPIO_OE_MASK
#undef GPIO_LEVEL
#undef GPIO_DRIVE
#undef GPIO_PULL_UP
#undef GPIO_NO_PULL
}

static void
step_link(uint32_t steps)
{
	uint32_t v;
	unsigned i;

	if (steps >= 10) {
		v = rd(parf + PARF_LTSSM);
		wr(parf + PARF_LTSSM, v | (1u << 8));
		tell("pcie: 10: ltssm %#x -> %#x (enabled %u, state %#x)\n", v, rd(parf + PARF_LTSSM),
		    (rd(parf + PARF_LTSSM) >> 8) & 1, rd(parf + PARF_LTSSM) & 0x3f);
		for (i = 1; i <= 20; i++) {
			tell("pcie: 10:   try %u: parf ltssm %#x (enabled %u, state %#x)\n", i,
			    rd(parf + PARF_LTSSM), (rd(parf + PARF_LTSSM) >> 8) & 1, rd(parf + PARF_LTSSM) & 0x3f);
			delay_for_interval(5000, NSEC_PER_USEC);
		}
	}
	if (steps < 11) {
		tell("pcie: 10: stopping here; step 11 drives the pins\n");
		return;
	}
	step_pins(steps);
	if (steps < 13) {
		tell("pcie: 12: stopping here; step 13 reads the elbi\n");
		return;
	}

	/*
	 * The link's own answer, from the ELBI: bit 10 is XMLH_LINK_UP and bits 17:12 are the LTSSM
	 * state (pci-msm.c:4419). Twenty tries five milliseconds apart is the vendor's 100 ms.
	 */
	if (elbi == 0) {
		tell("pcie: 12: no elbi window; the link state cannot be read\n");
		return;
	}
	for (i = 1; i <= 20; i++) {
		v = rd(elbi + ELBI_SYS_STTS);
		tell("pcie: 12:   try %u: elbi sys_stts %#x: link %s, ltssm state %#x\n", i, v, (v & 0x400) ? "UP" : "down",
		    (v >> 12) & 0x3f);
		if (v & 0x400) {
			break;
		}
		delay_for_interval(5000, NSEC_PER_USEC);
	}
	if (dbi != 0) {
		tell("pcie: 12: dbi link ctrl/status %#x (speed %#x, width %#x, data link active %u)\n",
		    rd(dbi + DBI_LINK_CTRL_STATUS), rd(dbi + DBI_LINK_CTRL_STATUS) & 0xf,
		    (rd(dbi + DBI_LINK_CTRL_STATUS) >> 4) & 0x3f, (rd(dbi + DBI_LINK_CTRL_STATUS) >> 29) & 1);
	}
}

/*
 * Step 14: the command database, read only.
 *
 * The six rails the radio's power manager needs are RPMh resources, and RPMh addresses a resource
 * by the 32-bit command word the database holds -- there is no way to vote one without it. So the
 * question this step answers is the only one that comes before any write: does this board's
 * database know these names, and what addresses does it give them.
 *
 * The names are the device tree's own qcom,resource-name strings (kona-regulators.dtsi, read out of
 * devices/alioth/prebuilt/device-tree.dtb):
 *
 *   smpa6  pm8150_s6   0.60-1.128 V   vdd-wlan-aon   0.95 V
 *   smpf2  pm8009_s2   0.512-1.10 V   vdd-wlan-dig   0.95 V
 *   smpa4  pm8150_s4   1.80-1.92 V    vdd-wlan-io    1.80 V
 *   smpa5  pm8150_s5   1.82-2.04 V    vdd-wlan-rfa1  1.90 V
 *   smpc8  pm8150a_s8  1.20-1.40 V    vdd-wlan-rfa2  1.35 V
 *
 * and the two the PCIe PHY itself uses (ldoa5 = vreg-0p9, ldoa9 = vreg-1p8) are asked about too,
 * because they are the pair the other port tried to vote first.
 *
 * The database is device memory: it is copied out with 32-bit reads before it is parsed, which is
 * what the port that works does as well.
 */
#define CMD_DB_PHYS     0x80860000UL    /* (UniXNU-sheng's board has it at 0x81c60000; this one here) */
#define CMD_DB_SIZE     0x00020000UL
#define RSC_DRV0_PHYS   0x18200000UL    /* (dt: rsc@18200000, reg-names drv-0, drv-1, drv-2) */
#define RSC_DRV2_PHYS   0x18220000UL    /* (qcom,drv-id = 2; qcom,tcs-offset = 0xd00) */
#define RSC_DRV_SIZE    0x00010000UL
#define RSC_DRV_CONFIG  0x00c           /* (k40-ref rpmh-rsc.c:38: TCS count [11:6], commands per TCS [31:27]) */

/*
 * Every resource whose command word says "VRM regulator" in its accelerator field (bits 19:16 = 4):
 * that is the set the radios' rails are in, and printing the sets' own names is how a rail that the
 * device tree names differently from the database is found instead of guessed at.
 */
static void
list_vrm(void *context, const char *name, uint32_t address, unsigned levels)
{
	tell("pcie: 14:   %s: address %#x, accel %u, aux %u levels\n", name, address, (address >> 16) & 0xf, levels);
}

static void
step_cmd_db(void)
{
	static const char *const names[] = { "smpa6", "smpf2", "smpa4", "smpa5", "smpc8", "ldoa5", "ldoa9" };
	static uint8_t db[CMD_DB_SIZE] __attribute__((aligned(8)));
	vm_offset_t window, rsc;
	unsigned i;

	window = ml_io_map(CMD_DB_PHYS, CMD_DB_SIZE);
	if (window == 0) {
		tell("pcie: 14: the command database at %#lx could not be mapped\n", (unsigned long) CMD_DB_PHYS);
		return;
	}
	for (i = 0; i < CMD_DB_SIZE / 4; i++) {
		uint32_t word = ((volatile uint32_t *) window)[i];

		__builtin_memcpy(db + 4 * i, &word, 4);
	}
	tell("pcie: 14: the command database at %#lx: version %u, magic %#x (expected %#x)\n",
	    (unsigned long) CMD_DB_PHYS, ((const uint32_t *) db)[0] , ((const uint32_t *) db)[1], HAKIOS_CMD_DB_MAGIC);

	for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		struct hakios_rpmh_resource r;
		int problem;

		__builtin_memset(&r, 0, sizeof r);
		problem = hakios_cmd_db_find(db, sizeof db, names[i], &r);
		if (problem != hakios_rpmh_ok) {
			tell("pcie: 14: %s: NOT IN THE COMMAND DATABASE (%d)\n", names[i], problem);
			continue;
		}
		tell("pcie: 14: %s: address %#x, aux %u levels\n", names[i], r.address, r.level_count);
	}

	rsc = ml_io_map(RSC_DRV2_PHYS, RSC_DRV_SIZE);
	if (rsc == 0) {
		tell("pcie: 14: the rsc's drv-2 (%#lx) could not be mapped\n", (unsigned long) RSC_DRV2_PHYS);
		return;
	}
	tell("pcie: 14: rsc drv-2 at %#lx: irq_enable %#x, irq_status %#x, config %#x (%u TCS, %u commands each)\n",
	    rsc, rd(rsc + 0x00), rd(rsc + 0x04), rd(rsc + RSC_DRV_CONFIG),
	    (rd(rsc + RSC_DRV_CONFIG) >> 6) & 0x3f, (rd(rsc + RSC_DRV_CONFIG) >> 27) & 0x1f);
	tell("pcie: 14: that layout is %s; nothing has been written\n",
	    "the vendor kernel's (tcs 672, command 20, control 0x14, cmd_enable 0x1c)");
	/*
	 * A raw search for names the parse did not produce: if "smpf" is in these bytes but no entry
	 * came back, the parser is what is wrong; if it is not there at all, the database really does
	 * not know the pm8009, and that is a different conclusion with a different next move.
	 */
	for (unsigned s = 0; s < 8; s++) {
		const uint8_t *slave = db + 8 + 16 * s;

		if (slave[0] == 0 && slave[1] == 0) {
			tell("pcie: 14: slave %u is empty (the records end here)\n", s);
			break;
		}
		tell("pcie: 14: slave %u: id %u, header %#x, data %#x, entries %u, version %u\n", s,
		    slave[0] | slave[1] << 8, slave[2] | slave[3] << 8, slave[4] | slave[5] << 8,
		    slave[6] | slave[7] << 8, slave[8] | slave[9] << 8);
	}
	for (unsigned i = 0; i + 4 < sizeof db; i++) {
		if (__builtin_memcmp(db + i, "smpf", 4) == 0 || __builtin_memcmp(db + i, "ldof", 4) == 0) {
			tell("pcie: 14: the raw bytes hold \"%c%c%c%c\" at offset %#x: %.8s\n", db[i], db[i + 1],
			    db[i + 2], db[i + 3], i, (const char *)(db + i));
		}
	}
	hakios_cmd_db_each(db, sizeof db, list_vrm, NULL);
}

/*
 * Step 15: the four rails, voted through RPMh.
 *
 * The write sequence is not new: it is the one hakios.adreno.c's PAS bandwidth vote already performs
 * on this board (pas_bw_write, "the proved DDR voter"), down to the message id 0x10108 that a
 * response-requested four-byte write carries, the active-mode-then-trigger pair, and the wait on the
 * RSC's interrupt status bit for TCS 0. What is new is only the resource and the value.
 *
 * Each rail takes three requests, one command each, as the port that powers the WCN7850 does it: the
 * mode (+8 = 7, high power mode), the voltage (+0, in millivolts), the enable (+4 = 1). The mode
 * request is not decoration -- without it that port's rails stay off and its link never trains.
 *
 * Gated, because this is the first write this file makes to a power rail:
 *   pcie_rail=1  print the commands and write nothing (the default when the argument is absent)
 *   pcie_rail=2  make the writes
 *
 * pm8009_s2 (vdd-wlan-dig) is deliberately absent: the command database at 0x80860000 does not have
 * it under any name -- step 14 printed all 125 entries, and the SMPS resources there are smpa3..a6
 * and smpc1..c8 only. So that rail is not APPS' to vote on this board, and the voter says so rather
 * than pretending.
 */
struct pcie_rail {
	const char *name;
	uint32_t millivolts;
	const char *supply;
};

static const struct pcie_rail pcie_rails[] = {
	{ "smpa6", 950,  "vdd-wlan-aon (pm8150_s6)" },
	{ "smpa4", 1800, "vdd-wlan-io (pm8150_s4)" },
	{ "smpa5", 1900, "vdd-wlan-rfa1 (pm8150_s5)" },
	{ "smpc8", 1350, "vdd-wlan-rfa2 (pm8150a_s8)" },
};

static bool
rsc_write(vm_offset_t rsc, uint32_t address, uint32_t data, const char *what)
{
	uint32_t irq_enable;
	bool done = false;

#define TCS(reg)        (rsc + 0xd00 + (reg))   /* (dt: qcom,tcs-offset = 0xd00, TCS 0) */
	for (int i = 0; rd(TCS(0x18)) == 0; i++) {
		if (i > 2000) {
			tell("pcie: 15: %s: the APPS TCS stays busy before the write\n", what);
			return false;
		}
		delay_for_interval(100, NSEC_PER_USEC);
	}
	irq_enable = rd(TCS(0x00));
	wr(TCS(0x14), rd(TCS(0x14)) & ~(1u << 24));
	wr(TCS(0x14), rd(TCS(0x14)) & ~(1u << 16));
	wr(TCS(0x08), 1);
	wr(TCS(0x30), 0x10108);                 /* (4-byte payload, response requested, a write) */
	wr(TCS(0x34), address);
	wr(TCS(0x38), data);
	wr(TCS(0x10), 1);
	wr(TCS(0x1c), 1);
	wr(TCS(0x00), irq_enable | 1);
	wr(TCS(0x14), 1u << 16);
	wr(TCS(0x14), 1u << 16 | 1u << 24);
	for (int i = 0; i < 2000 && !done; i++) {
		done = (rd(TCS(0x04)) & 1) != 0;
		if (!done) {
			delay_for_interval(100, NSEC_PER_USEC);
		}
	}
	wr(TCS(0x14), rd(TCS(0x14)) & ~(1u << 24));
	wr(TCS(0x14), rd(TCS(0x14)) & ~(1u << 16));
	wr(TCS(0x1c), 0);
	wr(TCS(0x10), 0);
	wr(TCS(0x08), 1);
	wr(TCS(0x00), irq_enable);
#undef TCS
	tell("pcie: 15: %s: wrote %#x = %u: %s\n", what, address, data, done ? "RPMh answered" : "NO RPMh ANSWER");
	return done;
}

static void
step_rails(uint32_t steps)
{
	static uint8_t db[CMD_DB_SIZE] __attribute__((aligned(8)));
	vm_offset_t window, rsc;
	uint32_t rail_mode = 0;
	unsigned i, voted = 0;

	(void) PE_parse_boot_argn("pcie_rail", &rail_mode, sizeof(rail_mode));
	window = ml_io_map(CMD_DB_PHYS, CMD_DB_SIZE);
	if (window == 0) {
		tell("pcie: 15: the command database could not be mapped\n");
		return;
	}
	for (i = 0; i < CMD_DB_SIZE / 4; i++) {
		uint32_t word = ((volatile uint32_t *) window)[i];

		__builtin_memcpy(db + 4 * i, &word, 4);
	}
	rsc = ml_io_map(RSC_DRV2_PHYS, RSC_DRV_SIZE);
	if (rsc == 0) {
		tell("pcie: 15: the rsc's drv-2 could not be mapped\n");
		return;
	}
	tell("pcie: 15: pcie_rail=%u (%s); rsc drv-2 at %#lx\n", rail_mode,
	    rail_mode == 2 ? "the writes are made" : "dry run, nothing is written", rsc);

	for (i = 0; i < sizeof(pcie_rails) / sizeof(pcie_rails[0]); i++) {
		struct hakios_rpmh_resource r;
		static const struct {
			uint32_t offset;
			const char *what;
		} requests[] = {
			{ 8, "mode 7 (high power)" },
			{ 0, "voltage" },
			{ 4, "enable" },
		};

		__builtin_memset(&r, 0, sizeof r);
		if (hakios_cmd_db_find(db, sizeof db, pcie_rails[i].name, &r) != hakios_rpmh_ok) {
			tell("pcie: 15: %s (%s): not in the command database\n", pcie_rails[i].name,
			    pcie_rails[i].supply);
			continue;
		}
		for (unsigned k = 0; k < sizeof(requests) / sizeof(requests[0]); k++) {
			uint32_t value = requests[k].offset == 8 ? 7 :
			    requests[k].offset == 0 ? pcie_rails[i].millivolts : 1;

			if (rail_mode != 2) {
				tell("pcie: 15: would write %#x = %u (%s, %s)\n", r.address + requests[k].offset,
				    value, pcie_rails[i].name, requests[k].what);
				continue;
			}
			(void) rsc_write(rsc, r.address + requests[k].offset, value, pcie_rails[i].name);
		}
		voted++;
	}
	tell("pcie: 15: %u of %u rails handled; vdd-wlan-dig (pm8009_s2) has no RPMh resource on this board\n",
	    voted, (unsigned)(sizeof(pcie_rails) / sizeof(pcie_rails[0])));
	if (rail_mode != 2) {
		tell("pcie: 15: dry run over; pcie_rail=2 makes these writes\n");
		return;
	}
	if (steps < 16) {
		tell("pcie: 15: stopping here; step 16 re-drives the endpoint with the rails up\n");
		return;
	}
}

/*
 * Step 16: the endpoint again, now that its rails are up.
 *
 * Up to here PERST# was released with the QCA6390 unpowered, which is why the link sat in Detect
 * and the ELBI only ever said 0x1. This is the vendor's order at last -- rails, then wlan-en, then
 * PERST# -- so PERST# is toggled once more and the link is asked for its state until it answers.
 */
static void
step_endpoint(void)
{
	vm_offset_t tlmm;
	unsigned i;

	tlmm = ml_io_map(0x0f900000UL, 0x0007d000UL);   /* (the north tile: see step 11) */
	if (tlmm == 0) {
		tell("pcie: 16: the north tile could not be mapped\n");
		return;
	}
	wr(tlmm + 0x1000 * 79, 0x200);                  /* (perst: an output, 2 mA, no pull) */
	wr(tlmm + 0x1000 * 79 + 4, 2);                  /* asserted */
	delay_for_interval(2000, NSEC_PER_USEC);
	wr(tlmm + 0x1000 * 79 + 4, 0);                  /* released */
	tell("pcie: 16: perst released again, level %u; asking the link\n", rd(tlmm + 0x1000 * 79 + 4) & 1);
	if (elbi == 0) {
		tell("pcie: 16: no elbi window\n");
		return;
	}
	for (i = 1; i <= 50; i++) {
		uint32_t v = rd(elbi + ELBI_SYS_STTS);

		tell("pcie: 16:   try %u: elbi sys_stts %#x: link %s, ltssm state %#x\n", i, v,
		    (v & 0x400) ? "UP" : "down", (v >> 12) & 0x3f);
		if (v & 0x400) {
			break;
		}
		delay_for_interval(5000, NSEC_PER_USEC);
	}
	tell("pcie: 16: parf ltssm %#x (state %#x), dbi link ctrl/status %#x (width %#x, data link active %u)\n",
	    rd(parf + PARF_LTSSM), rd(parf + PARF_LTSSM) & 0x3f, rd(dbi + DBI_LINK_CTRL_STATUS),
	    (rd(dbi + DBI_LINK_CTRL_STATUS) >> 4) & 0x3f, (rd(dbi + DBI_LINK_CTRL_STATUS) >> 29) & 1);
}

/*
 * Step 17: PM8009's SMPS2, read only, over SPMI.
 *
 * The four rails came up through RPMh; the fifth -- vdd-wlan-dig, pm8009_s2, the radio's digital
 * core -- has no resource in the command database at all (step 14 printed all 125 entries). So the
 * question is not "which name" any more but "who does control it, and is it on". This asks the PMIC
 * itself: the SPMI arbiter's channel table says which (slave, peripheral) pairs exist, and a
 * regulator peripheral answers 0x40/0x41 (voltage, millivolts) and 0x46 (enable, bit 7).
 *
 * The arbiter windows, the channel map layout and the transfer handshake are hakios.haptic.c's
 * (ARB_CORE 0x0c440000, channel map at 0x900 + 4n with the slave in bits 11:8 and the peripheral in
 * 7:0, observer window 0xe600000 with 0x80 a channel). Nothing is written here.
 */
#define ARB_CORE_PHYS           0x0c440000UL
#define ARB_CORE_SIZE           0x00001100UL
#define ARB_CNFG_PHYS           0x0c40a000UL
#define ARB_CNFG_SIZE           0x00001000UL
#define ARB_OBSERVER_PHYS       0x0e600000UL
#define ARB_OBSERVER_SIZE       0x00010000UL
#define ARB_SYSTEM              0
#define ARB_OWNER(n)            (0x700 + 4 * (n))

static vm_offset_t arb_core, arb_observer;

static bool
spmi_read_channel(uint16_t channel, uint8_t reg, uint8_t *bytes, unsigned count)
{
	vm_offset_t at = arb_observer + 0x10000UL * ARB_SYSTEM + 0x80UL * channel;
	uint32_t data[2], status = 0;
	unsigned spins;

	if (count == 0 || count > 8) {
		return false;
	}
	wr(at + 0x00, (1u << 27) | (uint32_t)reg << 4 | (count - 1));    /* (a read: bit 27) */
	for (spins = 0; spins < 100000; spins++) {
		status = rd(at + 0x08);
		if (status & 1) {                                       /* (done) */
			break;
		}
	}
	data[0] = rd(at + 0x18);
	data[1] = count > 4 ? rd(at + 0x1c) : 0;
	if ((status & 1) == 0 || (status & (2 | 4)) != 0) {
		return false;
	}
	for (unsigned i = 0; i < count; i++) {
		bytes[i] = (uint8_t)(data[i / 4] >> (8 * (i % 4)));
	}
	return true;
}

static void
step_rails_read(void)
{
	uint32_t channels, version;
	unsigned found_15 = 0, found_14 = 0;
	uint16_t channel_15 = 0xffff, channel_14 = 0xffff;

	arb_core = ml_io_map(ARB_CORE_PHYS, ARB_CORE_SIZE);
	arb_observer = ml_io_map(ARB_OBSERVER_PHYS, ARB_OBSERVER_SIZE);
	if (arb_core == 0 || arb_observer == 0) {
		tell("pcie: 17: the spmi arbiter could not be mapped\n");
		return;
	}
	version = rd(arb_core + 0x000);
	channels = rd(arb_core + 0x004) & 0x7ff;
	if (channels == 0 || channels > 512) {
		channels = 512;
	}
	tell("pcie: 17: spmi arbiter version %#x, %u channels\n", version, channels);
	for (uint32_t n = 0; n < channels; n++) {
		uint32_t map = rd(arb_core + 0x900 + 4 * n);
		uint32_t slave, usid;

		if (map == 0) {
			continue;
		}
		slave = (map >> 8) & 0xfff;
		usid = map & 0xff;
		if (usid < 0x13 || usid > 0x17) {
			continue;               /* (only the smps peripherals are interesting here) */
		}
		tell("pcie: 17:   channel %u: slave %#x, peripheral %#x, owner %u\n", n, slave, usid,
		    rd(arb_core + 0x700 + 4 * n) & 7);
		if (usid == 0x15 && found_15++ == 0) {
			channel_15 = (uint16_t) n;
		}
		if (usid == 0x14 && found_14++ == 0) {
			channel_14 = (uint16_t) n;
		}
	}
	if (found_15 == 0 && found_14 == 0) {
		tell("pcie: 17: no smps channel at all; the pmics' regulators are not on this arbiter\n");
		return;
	}
	for (unsigned k = 0; k < 2; k++) {
		uint16_t channel = k == 0 ? channel_14 : channel_15;
		uint8_t volt[2] = { 0 }, mode = 0, enable = 0, type[2] = { 0 };

		if (k == 0 && found_14 == 0) {
			continue;
		}
		if (k == 1 && found_15 == 0) {
			continue;
		}
		if (!spmi_read_channel(channel, 0x04, type, 2)) {
			tell("pcie: 17: smps%s: no answer\n", k == 0 ? "1" : "2");
			continue;
		}
		(void) spmi_read_channel(channel, 0x40, volt, 2);
		(void) spmi_read_channel(channel, 0x45, &mode, 1);
		(void) spmi_read_channel(channel, 0x46, &enable, 1);
		tell("pcie: 17: smps%s (channel %u): type %#04x/%#04x, %u mV, mode %#x, enable %#x (%s)\n",
		    k == 0 ? "1" : "2", channel, type[0], type[1], (unsigned)(volt[0] | volt[1] << 8), mode, enable,
		    (enable & 0x80) ? "ON" : "OFF");
	}
}

/*
 * Step 18: the vendor's seven dm_core writes, and a restart of the link training.
 *
 * pci-msm.c:4379-4409 (msm_pcie_link_train) touches the core's own configuration space before it
 * throws the LTSSM switch, and the port that tried this before could never make the first of those
 * accesses return (its dm_core was unreachable). Ours is reachable, so the same eight
 * read-modify-writes are made here, followed by LTSSM bit 8 cleared and set again -- a restart, so
 * that the state machine begins from Detect with the endpoint powered and out of reset.
 *
 * The values are the vendor's: gen3_gen2_ctrl's [12:8] = 1, eq_control's bit 5 and gen3_related's
 * bit 0 cleared, misc_control's bit 0 around the preset (0x55555555, the vendor's default when the
 * device tree has no qcom,core-preset), and device_control_status's [14:12] = 5 for the largest
 * read request. The address fields are read back after each write, so a field that did not take
 * says so instead of being assumed.
 */
static void
step_train(void)
{
	static const struct {
		uint32_t offset;
		uint32_t clear;
		uint32_t set;
		const char *what;
	} writes[] = {
		{ 0x80c, 0x1f00, 1u << 8, "gen3_gen2_ctrl [12:8] = 1" },
		{ 0x8a8, 1u << 5, 0, "gen3_eq_control bit 5 cleared" },
		{ 0x890, 1u << 0, 0, "gen3_related bit 0 cleared" },
		{ 0x8bc, 0, 1u << 0, "gen3_misc_control bit 0 set (the preset may be written)" },
		{ 0x154, 0xffffffffu, 0x55555555u, "gen3_spcie_cap = 0x55555555 (the vendor's core preset)" },
		{ 0x8bc, 1u << 0, 0, "gen3_misc_control bit 0 cleared again" },
		{ 0x78, 0x7000u, 5u << 12, "device_control_status [14:12] = 5 (max read request)" },
	};
	uint32_t v;
	unsigned i;

	if (dbi == 0 || parf == 0 || elbi == 0) {
		tell("pcie: 18: a window is missing; nothing to write\n");
		return;
	}
	for (i = 0; i < sizeof(writes) / sizeof(writes[0]); i++) {
		v = rd(dbi + writes[i].offset);
		wr(dbi + writes[i].offset, (v & ~writes[i].clear) | writes[i].set);
		tell("pcie: 18: %s: %#x -> %#x -> %#x\n", writes[i].what, v, (v & ~writes[i].clear) | writes[i].set,
		    rd(dbi + writes[i].offset));
	}

	tell("pcie: 18: restarting training: ltssm %#x\n", rd(parf + PARF_LTSSM));
	wr(parf + PARF_LTSSM, rd(parf + PARF_LTSSM) & ~(1u << 8));
	delay_for_interval(1000, NSEC_PER_USEC);
	wr(parf + PARF_LTSSM, rd(parf + PARF_LTSSM) | (1u << 8));
	tell("pcie: 18: ltssm %#x (enabled %u, state %#x)\n", rd(parf + PARF_LTSSM),
	    (rd(parf + PARF_LTSSM) >> 8) & 1, rd(parf + PARF_LTSSM) & 0x3f);
	for (i = 1; i <= 50; i++) {
		v = rd(elbi + ELBI_SYS_STTS);
		if ((i % 5) == 0 || (v & 0x400) != 0) {
			tell("pcie: 18:   try %u: elbi %#x: link %s, ltssm state %#x\n", i, v,
			    (v & 0x400) ? "UP" : "down", (v >> 12) & 0x3f);
		}
		if (v & 0x400) {
			break;
		}
		delay_for_interval(5000, NSEC_PER_USEC);
	}
	tell("pcie: 18: end: parf ltssm state %#x, dbi link ctrl/status %#x (speed %#x, width %#x, active %u)\n",
	    rd(parf + PARF_LTSSM) & 0x3f, rd(dbi + DBI_LINK_CTRL_STATUS), rd(dbi + DBI_LINK_CTRL_STATUS) & 0xf,
	    (rd(dbi + DBI_LINK_CTRL_STATUS) >> 4) & 0x3f, (rd(dbi + DBI_LINK_CTRL_STATUS) >> 29) & 1);
}

/*
 * Step 19: the endpoint's power-up sequence in the vendor's order, with the one thing every earlier
 * step got wrong.
 *
 * The device tree settles a question this file asked twice: alioth has no fifth rail. Its cnss node
 * gives vdd-wlan-aon and vdd-wlan-dig the *same* supply (phandle 0xa9, pm8150_s6 = smpa6, both at
 * 0xe7ef0 = 950 mV), and io/rfa1/rfa2 the three that were voted. So all four rails were up by step
 * 15, and "the dig rail is missing" was a reading of the generic kona.dtsi, not of this board.
 *
 * What was wrong instead is the order of the two pads. Every earlier run drove WLAN_EN high *before*
 * the rails existed (step 12 runs before step 15), so the chip was enabled while unpowered and then
 * powered with its enable already high -- there is no rising edge for it to see. Here the sequence is
 * the vendor's: power (already voted), then WLAN_EN low and high again, then PERST#.
 *
 * Both PERST# polarities are tried, and the reason is honest: perst-gpio is <&tlmm 79 0>, active
 * high, so "asserted" is high and "released" is low -- but no earlier run ever produced a link, so
 * the other polarity gets its turn in the same boot rather than in the next one. The first attempt
 * that brings the link up ends the step.
 */
static void
step_power_sequence(void)
{
	static const struct {
		bool release_high;
		const char *what;
	} polarities[] = {
		{ false, "perst released low (the device tree's active-high reading)" },
		{ true,  "perst released high (the other reading)" },
	};
	vm_offset_t tlmm;

	tlmm = ml_io_map(0x0f900000UL, 0x0007d000UL);
	if (tlmm == 0 || elbi == 0) {
		tell("pcie: 19: a window is missing\n");
		return;
	}
#define PIN_CFG(n)      (tlmm + 0x1000 * (n))
#define PIN_IO(n)       (tlmm + 0x1000 * (n) + 4)
	/* wlan-en, the device tree's cnss_wlan_en_active: function gpio, 16 mA, pull-up, output. */
	wr(PIN_CFG(20), 0x3c3);
	tell("pcie: 19: wlan-en low (the rails are already up), then high again\n");
	wr(PIN_IO(20), rd(PIN_IO(20)) & ~2u);
	delay_for_interval(5000, NSEC_PER_USEC);
	wr(PIN_IO(20), rd(PIN_IO(20)) | 2u);
	delay_for_interval(10000, NSEC_PER_USEC);

	for (unsigned p = 0; p < sizeof(polarities) / sizeof(polarities[0]); p++) {
		unsigned tries;

		wr(PIN_CFG(79), 0x200);                 /* (perst: an output, 2 mA, no pull) */
		wr(PIN_IO(79), polarities[p].release_high ? 0u : 2u);   /* asserted */
		delay_for_interval(2000, NSEC_PER_USEC);
		wr(PIN_IO(79), polarities[p].release_high ? 2u : 0u);   /* released */
		tell("pcie: 19: %s: level now %u\n", polarities[p].what, rd(PIN_IO(79)) & 1);
		for (tries = 1; tries <= 20; tries++) {
			uint32_t v = rd(elbi + ELBI_SYS_STTS);

			if ((tries % 5) == 0 || (v & 0x400) != 0) {
				tell("pcie: 19:   try %u: elbi %#x: link %s, ltssm state %#x\n", tries, v,
				    (v & 0x400) ? "UP" : "down", (v >> 12) & 0x3f);
			}
			if (v & 0x400) {
				tell("pcie: 19: THE LINK IS UP: dbi link ctrl/status %#x (speed %#x, width %#x, active %u)\n",
				    rd(dbi + DBI_LINK_CTRL_STATUS), rd(dbi + DBI_LINK_CTRL_STATUS) & 0xf,
				    (rd(dbi + DBI_LINK_CTRL_STATUS) >> 4) & 0x3f,
				    (rd(dbi + DBI_LINK_CTRL_STATUS) >> 29) & 1);
				goto out;
			}
			delay_for_interval(5000, NSEC_PER_USEC);
		}
	}
	tell("pcie: 19: neither polarity trained; parf ltssm state %#x\n", rd(parf + PARF_LTSSM) & 0x3f);
out:
#undef PIN_CFG
#undef PIN_IO
	;
}

/*
 * Step 20: the PHY's power-down word, cleared, and the link tried again.
 *
 * The LTSSM sits in 0x5, which k40-ref calls MSM_PCIE_LTSSM_PRE_DETECT_QUIET (pci-msm.c:416) -- the
 * state before detect, where the controller is waiting for its PHY's PIPE to be out of power down
 * rather than looking for a receiver. The device tree gives that register a name of its own
 * (qcom,phy-power-down-offset = <0x840>) and the sequence's first write is 0x840 = 3: both PWRDN
 * bits set. Nothing in this file ever cleared them, and the vendor does clear that word -- outside
 * the sequence, in msm_pcie_disable and on its failure paths (pci-msm.c:4644, 4683), which is where
 * "0" is the value that means "not powered down".
 *
 * So: clear it, ask the PHY whether it is still ready, and restart the training. If the PIPE was the
 * missing half, the state moves and this step says so.
 */
static void
step_phy_powerdown(void)
{
	uint32_t was, now;
	unsigned i;

	if (phy == 0 || parf == 0 || elbi == 0) {
		tell("pcie: 20: a window is missing\n");
		return;
	}
	was = rd(phy + PHY_POWER_DOWN_OFFSET);
	wr(phy + PHY_POWER_DOWN_OFFSET, was & ~3u);
	tell("pcie: 20: phy power_down %#x -> %#x (both PWRDN bits cleared)\n", was,
	    rd(phy + PHY_POWER_DOWN_OFFSET));
	for (i = 1; i <= 10; i++) {
		now = rd(phy + PHY_STATUS_OFFSET);
		if (((now >> PHY_STATUS_BIT) & 1) == 0) {
			tell("pcie: 20: phy still ready after %u tries (status %#x)\n", i, now);
			break;
		}
		delay_for_interval(1000, NSEC_PER_USEC);
		if (i == 10) {
			tell("pcie: 20: phy NOT ready after clearing power-down (status %#x)\n", now);
			return;
		}
	}

	tell("pcie: 20: restarting training with the phy out of power down\n");
	wr(parf + PARF_LTSSM, rd(parf + PARF_LTSSM) & ~(1u << 8));
	delay_for_interval(1000, NSEC_PER_USEC);
	wr(parf + PARF_LTSSM, rd(parf + PARF_LTSSM) | (1u << 8));
	for (i = 1; i <= 40; i++) {
		uint32_t v = rd(elbi + ELBI_SYS_STTS);

		if ((i % 5) == 0 || (v & 0x400) != 0) {
			tell("pcie: 20:   try %u: elbi %#x: link %s, ltssm state %#x\n", i, v,
			    (v & 0x400) ? "UP" : "down", (v >> 12) & 0x3f);
		}
		if (v & 0x400) {
			tell("pcie: 20: THE LINK IS UP: dbi link ctrl/status %#x (speed %#x, width %#x, active %u)\n",
			    rd(dbi + DBI_LINK_CTRL_STATUS), rd(dbi + DBI_LINK_CTRL_STATUS) & 0xf,
			    (rd(dbi + DBI_LINK_CTRL_STATUS) >> 4) & 0x3f, (rd(dbi + DBI_LINK_CTRL_STATUS) >> 29) & 1);
			return;
		}
		delay_for_interval(5000, NSEC_PER_USEC);
	}
	tell("pcie: 20: still down; parf ltssm %#x (state %#x)\n", rd(parf + PARF_LTSSM), rd(parf + PARF_LTSSM) & 0x3f);
}

/*
 * Step 21: the two bits the controller set for itself, and what the state does without them.
 *
 * Step 20 cleared the PHY's power-down word and the machine stopped answering the controller's own
 * registers -- PARF included, which had answered everything since step 4. So 0x840 = 3 is the
 * resting value on this board and the vendor's zero is a teardown value, not a bring-up one. That
 * is now known by measurement rather than by reading.
 *
 * What the last good reading did show is this: PARF_LTSSM read 0x905 -- state 0x5 (which k40-ref
 * calls MSM_PCIE_LTSSM_PRE_DETECT_QUIET) with bit 8 (LTSSM_EN, written by step 10) and bit 11 (the
 * register's FLUSH_MODE) both set. Nothing in this file writes bit 11, so the controller set it,
 * and a link held in flush mode never sends a detect pulse: the state before it can never leave
 * 0x5, which is what every attempt has shown.
 *
 * So: clear bit 11 (and bit 10, SW_CLR_FLUSH_MODE, with it) and let the state machine run. PARF is
 * the only register read here; the ELBI is not touched.
 */
static void
step_flush_clear(void)
{
	uint32_t was, now;
	unsigned i;

	if (parf == 0 || dbi == 0) {
		tell("pcie: 21: a window is missing\n");
		return;
	}
	was = rd(parf + PARF_LTSSM);
	tell("pcie: 21: parf ltssm %#x (state %#x, enable %u, sw_clr_flush %u, flush_mode %u)\n", was, was & 0x3f,
	    (was >> 8) & 1, (was >> 10) & 1, (was >> 11) & 1);
	wr(parf + PARF_LTSSM, (was & ~(1u << 11) & ~(1u << 10)) | (1u << 8));
	delay_for_interval(1000, NSEC_PER_USEC);
	now = rd(parf + PARF_LTSSM);
	tell("pcie: 21: after clearing flush mode: parf ltssm %#x (state %#x, flush_mode %u)\n", now, now & 0x3f,
	    (now >> 11) & 1);
	for (i = 1; i <= 20; i++) {
		now = rd(parf + PARF_LTSSM);
		tell("pcie: 21:   try %u: parf ltssm %#x (state %#x), dbi link %#x\n", i, now, now & 0x3f,
		    rd(dbi + DBI_LINK_CTRL_STATUS));
		delay_for_interval(5000, NSEC_PER_USEC);
	}
	tell("pcie: 21: end: state %#x, flush_mode %u, dbi link ctrl/status %#x (width %#x, active %u)\n",
	    rd(parf + PARF_LTSSM) & 0x3f, (rd(parf + PARF_LTSSM) >> 11) & 1, rd(dbi + DBI_LINK_CTRL_STATUS),
	    (rd(dbi + DBI_LINK_CTRL_STATUS) >> 4) & 0x3f, (rd(dbi + DBI_LINK_CTRL_STATUS) >> 29) & 1);
}

/*
 * Step 22: the aux clock's frequency register, and the state again.
 *
 * pci-msm.c:3823-3826 is two lines that this file has never had:
 *
 *	if (dev->use_19p2mhz_aux_clk)
 *		msm_pcie_write_reg(dev->dm_core, PCIE20_AUX_CLK_FREQ_REG, 0x14);
 *	else
 *		msm_pcie_write_reg(dev->dm_core, PCIE20_AUX_CLK_FREQ_REG, 0x01);
 *
 * and this board sets qcom,use-19p2mhz-aux-clk, so the core is told what its AUX clock is *before*
 * it is asked to train. Every timestamped thing in the core -- the 12 ms Detect.Quiet timer among
 * them -- counts against that clock, and a core that has not been told its frequency does not leave
 * Detect.Quiet. That is the state every attempt here has ended in.
 *
 * The controller's own dump register (PCIE20_PORT_LINK_CTRL_REG 0x710, read in the vendor's
 * diagnostics at pci-msm.c:2103) is printed alongside, and the flush bit from step 21 is cleared
 * again in the same breath, because a restart sets it.
 */
#define DBI_AUX_CLK_FREQ        0x0b40
#define DBI_PORT_LINK_CTRL      0x0710

static void
step_aux_clock(void)
{
	uint32_t before, now;
	unsigned i;

	if (dbi == 0 || parf == 0) {
		tell("pcie: 22: a window is missing\n");
		return;
	}
	before = rd(dbi + DBI_AUX_CLK_FREQ);
	wr(dbi + DBI_AUX_CLK_FREQ, 0x14);               /* (19.2 MHz: the vendor's value for this board) */
	tell("pcie: 22: aux_clk_freq %#x -> %#x (was it 0x14 already? %s), port link ctrl %#x\n", before,
	    rd(dbi + DBI_AUX_CLK_FREQ), before == 0x14 ? "yes" : "no", rd(dbi + DBI_PORT_LINK_CTRL));

	now = rd(parf + PARF_LTSSM);
	wr(parf + PARF_LTSSM, (now & ~(1u << 11) & ~(1u << 10)) | (1u << 8));
	delay_for_interval(1000, NSEC_PER_USEC);
	for (i = 1; i <= 30; i++) {
		now = rd(parf + PARF_LTSSM);
		tell("pcie: 22:   try %u: parf ltssm %#x (state %#x, flush %u), dbi link %#x\n", i, now, now & 0x3f,
		    (now >> 11) & 1, rd(dbi + DBI_LINK_CTRL_STATUS));
		if ((now & 0x3f) >= 0x11 || ((rd(dbi + DBI_LINK_CTRL_STATUS) >> 29) & 1) != 0) {
			tell("pcie: 22: THE LINK IS UP: dbi link ctrl/status %#x (speed %#x, width %#x, active %u)\n",
			    rd(dbi + DBI_LINK_CTRL_STATUS), rd(dbi + DBI_LINK_CTRL_STATUS) & 0xf,
			    (rd(dbi + DBI_LINK_CTRL_STATUS) >> 4) & 0x3f, (rd(dbi + DBI_LINK_CTRL_STATUS) >> 29) & 1);
			return;
		}
		delay_for_interval(5000, NSEC_PER_USEC);
	}
	tell("pcie: 22: end: state %#x, flush %u, dbi link ctrl/status %#x (width %#x, active %u)\n",
	    rd(parf + PARF_LTSSM) & 0x3f, (rd(parf + PARF_LTSSM) >> 11) & 1, rd(dbi + DBI_LINK_CTRL_STATUS),
	    (rd(dbi + DBI_LINK_CTRL_STATUS) >> 4) & 0x3f, (rd(dbi + DBI_LINK_CTRL_STATUS) >> 29) & 1);
}

/*
 * Step 23: the configuration space, which is what all of this was for.
 *
 * The device tree's third window is ECAM: conf = <0x60100000 0x100000> is bus 0, and the ranges map
 * bus 1 to 0x60200000 and bus 2 to 0x60300000. So the root complex's own type-1 header is at
 * 0x60100000 and the endpoint the link just trained to is at 0x60200000 -- and the vendor and
 * device ids there say whether the thing on the other side of the link is the QCA6390 (17cb:1101)
 * or nothing at all.
 */
#define ECAM_BUS0_PHYS  0x60100000UL
#define ECAM_BUS1_PHYS  0x60200000UL
#define ECAM_SIZE       0x00010000UL

static void
step_config(void)
{
	vm_offset_t bus0, bus1;
	uint32_t id, header, bars;

	if (dbi == 0) {
		tell("pcie: 23: no dbi window\n");
		return;
	}
	bus0 = ml_io_map(ECAM_BUS0_PHYS, ECAM_SIZE);
	bus1 = ml_io_map(ECAM_BUS1_PHYS, ECAM_SIZE);
	tell("pcie: 23: ecam bus0 at %#lx, bus1 at %#lx\n", bus0, bus1);
	if (bus0 != 0) {
		id = rd(bus0);
		header = rd(bus0 + 0x0c);
		tell("pcie: 23: bus 0 dev 0 fn 0: id %#x (vendor %#x, device %#x), header type %#x\n", id, id & 0xffff,
		    id >> 16, (header >> 16) & 0xff);
		tell("pcie: 23:   the root complex's bus numbers: primary %u, secondary %u, subordinate %u\n",
		    (rd(bus0 + 0x18) >> 8) & 0xff, (rd(bus0 + 0x18) >> 16) & 0xff, (rd(bus0 + 0x18) >> 24) & 0xff);
	}
	if (bus1 == 0) {
		tell("pcie: 23: bus 1 could not be mapped\n");
		return;
	}
	for (unsigned fn = 0; fn < 2; fn++) {
		id = rd(bus1 + 0x1000 * fn);
		if (id == 0 || id == 0xffffffffu) {
			tell("pcie: 23: bus 1 dev 0 fn %u: nothing (%#x)\n", fn, id);
			continue;
		}
		header = rd(bus1 + 0x1000 * fn + 0x0c);
		tell("pcie: 23: bus 1 dev 0 fn %u: id %#x -- vendor %#x, device %#x (QCA6390 is 17cb:1101), header %#x\n",
		    fn, id, id & 0xffff, id >> 16, (header >> 16) & 0xff);
		tell("pcie: 23:   command/status %#x, revision/class %#x\n", rd(bus1 + 0x1000 * fn + 0x04),
		    rd(bus1 + 0x1000 * fn + 0x08));
		for (unsigned b = 0; b < 3; b++) {
			bars = rd(bus1 + 0x1000 * fn + 0x10 + 4 * b);
			tell("pcie: 23:   bar%u %#x\n", b, bars);
		}
	}
}

static void
step_setup(uint32_t steps)
{
	uint32_t was;

	if (parf == 0) {
		return;
	}
	/*
	 * One register a step. Writing them together hung the machine with nothing in the log at all
	 * (step 5, 21:44: that boot's last line was a launchd one forty seconds in, and the same run
	 * with pcie_step=4 prints everything), so whichever of them is the one cannot be told apart
	 * from the others that way -- and each takes a boot to try.
	 */
	if (steps >= 5) {
		tell("pcie: 5: about to write mhi_clock_reset (%#x = %#x); if nothing follows, that write hung\n",
		    PARF_MHI_CLOCK_RESET, 0x13u);
		was = rd(parf + PARF_MHI_CLOCK_RESET);
		wr(parf + PARF_MHI_CLOCK_RESET, (was & ~0x30u) | 0x10u);        /* (bit 4: what 0x13 sets) */
		tell("pcie: 5: mhi_clock_reset %#x -> %#x\n", was, rd(parf + PARF_MHI_CLOCK_RESET));
	}
	if (steps >= 6) {
		/*
		 * The rest of what the vendor's driver writes before it touches the configuration space
		 * (pci-msm.c:4510-4565, msm_pcie_hw_init), in its order:
		 *
		 *   PARF_DEVICE_TYPE = 4        "configure PCIe to RC mode" -- this was 0, which is not a
		 *                               root complex at all, and is the best explanation there is
		 *                               for the configuration space not answering
		 *   PARF_PM_CTRL     bit 5 clear   (L1 enabled: REQ_NOT_ENTR_L1)
		 *   PARF_PHY_CTRL    bit 0 clear
		 *   PARF_DBI_BASE    = 0         an offset, not an address: the vendor writes zero, and the
		 *                               window at 0x60000000 is the one the device tree describes
		 *   PARF_SYS_CTRL    = 0x365E
		 *   PARF_MHI_CLOCK_RESET_CTRL  bit 4 set  (done in step 5, left here for the order's sake)
		 *   PARF_INT_ALL_MASK: 0, then the bits the driver wants
		 *   PARF_SLV_ADDR_SPACE_SIZE = 64 MB  (the device tree's qcom,slv-addr-space-size, and
		 *                               the one value here that the first attempt had wrong: 16 MB)
		 *   PARF_AXI_MSTR_WR_ADDR_HALT: bit 31 with what is already there
		 *
		 * Writes have been safe all evening; it was reading the configuration space that stopped
		 * the machine, and that is step 7.
		 */
		was = rd(parf + PARF_DEVICE_TYPE);
		wr(parf + PARF_DEVICE_TYPE, 4);
		tell("pcie: 6: device_type %#x -> %#x (root complex mode)\n", was, rd(parf + PARF_DEVICE_TYPE));

		was = rd(parf + PARF_PM_CTRL);
		wr(parf + PARF_PM_CTRL, was & ~(1u << 5));
		tell("pcie: 6: pm_ctrl %#x -> %#x (L1 allowed)\n", was, rd(parf + PARF_PM_CTRL));

		was = rd(parf + PARF_PHY_CTRL);
		wr(parf + PARF_PHY_CTRL, was & ~1u);
		tell("pcie: 6: phy_ctrl %#x -> %#x\n", was, rd(parf + PARF_PHY_CTRL));

		wr(parf + PARF_DBI_BASE_ADDR, 0);
		tell("pcie: 6: dbi_base = %#x (an offset; the window is %#lx)\n",
		    rd(parf + PARF_DBI_BASE_ADDR), (unsigned long) PCIE_DM_CORE_PHYS);

		was = rd(parf + PARF_SYS_CTRL);
		wr(parf + PARF_SYS_CTRL, 0x365e);
		tell("pcie: 6: sys_ctrl %#x -> %#x\n", was, rd(parf + PARF_SYS_CTRL));

		wr(parf + PARF_INT_ALL_MASK, 0);
		tell("pcie: 6: int_all_mask %#x (everything masked for now)\n", rd(parf + PARF_INT_ALL_MASK));

		wr(parf + PARF_SLV_ADDR_SPACE, 0x04000000);
		tell("pcie: 6: slv_addr_space %#x (the device tree's qcom,slv-addr-space-size)\n",
		    rd(parf + PARF_SLV_ADDR_SPACE));

		was = rd(parf + PARF_AXI_MSTR_HALT);
		wr(parf + PARF_AXI_MSTR_HALT, was | (1u << 31));
		tell("pcie: 6: axi_mstr_halt %#x -> %#x\n", was, rd(parf + PARF_AXI_MSTR_HALT));
	}
	if (steps >= 7) {
		tell("pcie: 7: parf ltssm %#x, status %#x, pm_stts %#x\n", rd(parf + PARF_LTSSM),
		    rd(parf + PARF_STATUS), rd(parf + PARF_PM_STTS));
	}
	if (steps >= 8) {
		step_phy();
	}
	if (steps >= 9) {
		if (dbi != 0) {
			tell("pcie: 9: reading the dbi's id (%#x at %#lx) -- if nothing follows, that read hung\n",
			    DBI_ID, dbi);
			tell("pcie: 9: dbi id %#x (Qualcomm is 0x17cb), header type %#x, cap %#x\n",
			    rd(dbi + DBI_ID), rd(dbi + DBI_HEADER_TYPE), rd(dbi + DBI_CAP));
		}
	}
	if (steps >= 10) {
		step_link(steps);
	}
	if (steps >= 14) {
		step_cmd_db();
	}
	if (steps >= 15) {
		step_rails(steps);
	}
	if (steps >= 16) {
		step_endpoint();
	}
	if (steps >= 17) {
		step_rails_read();
	}
	if (steps >= 18) {
		step_train();
	}
	if (steps >= 19) {
		step_power_sequence();
	}
	if (steps >= 21) {
		step_flush_clear();
	}
	if (steps >= 22) {
		step_aux_clock();
	}
	if (steps >= 23) {
		step_config();
	}
}

void
hakios_pcie_init(void)
{
	uint32_t steps = 0, at = 0;

	if (!PE_parse_boot_argn("pcie_step", &steps, sizeof(steps)) || steps == 0) {
		return;
	}
	if (PE_parse_boot_argn("pcie_at", &at, sizeof(at)) && at >= 1 && at <= 600) {
		thread_t thread;

		if (kernel_thread_start(pcie_thread, (void *)(uintptr_t) at, &thread) == KERN_SUCCESS) {
			thread_deallocate(thread);
			return;
		}
	}
	pcie_run(steps);
}
