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
static uint32_t bhi_base = 0x1e0e200u;   /* (what BAR + 0x28 says; see step 44) */
static void step_setup(uint32_t steps);  /* (defined below, used by the runner above it) */
/* (the read-only steps the reuse path calls before their definitions) */
static void step_endpoint_config(void);
static void step_bhi(void);
static void step_capabilities(void);
static void step_smmu_map(void);
static void step_bhi_download(void);
static void step_after_download(void);
static void step_bhi_retry(void);
static void step_iatu_map(void);
static void step_bdf_to_sid(void);
static void step_sid_table(void);
static void step_stream_read(void);
static void step_iatu_inbound(void);
static void step_bhi_base(void);
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

		/*
		 * Released, not pulsed. Mainline's qcom_pcie_init_2_7_0 asserts and releases, and that
		 * was tried here: with the pulse, a clean boot reached L0 and then the core's slave
		 * aperture answered 0xffffffff for the rest of the run. The runs that ended with an
		 * endpoint that answered had no pulse. pcie_reset_pulse=1 puts it back for the next
		 * experiment; the default follows the evidence.
		 */
		{
			uint32_t pulse = 0;

			PE_parse_boot_argn("pcie_reset_pulse", &pulse, sizeof(pulse));
			if (pulse != 0) {
				wr(gcc_window + pcie_resets[i].at, was | 1u);
				delay_for_interval(1000, NSEC_PER_USEC);
			}
		}
		wr(gcc_window + pcie_resets[i].at, now);
		delay_for_interval(1000, NSEC_PER_USEC);
		tell("pcie: 4:   %s %#x: %#x -> %#x (%s)\n", pcie_resets[i].name, pcie_resets[i].at, was,
		    rd(gcc_window + pcie_resets[i].at), (was & 1) ? "was asserted" : "was already out");
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
	/*
	 * pcie_reuse=1: if the endpoint is already at L0, do not initialise anything.
	 *
	 * A run that ends with the link up leaves the controller initialised, and every step that
	 * brings it up again -- the reset release, the PARF writes, the PERST# sequence -- is one that
	 * has been measured to break a working link. So a repeated experiment can start from "the link
	 * is already there" and only read, which is what a debug loop wants: the alternative is a cold
	 * boot between every attempt (five minutes each) because a warm one cannot re-initialise.
	 */
	{
		uint32_t reuse = 0;

		PE_parse_boot_argn("pcie_reuse", &reuse, sizeof(reuse));
		if (reuse != 0 && (rd(parf + PARF_LTSSM) & 0x3f) == 0x11) {
			tell("pcie: reuse: the link is already at L0 (ltssm %#x); nothing is initialised\n",
			    rd(parf + PARF_LTSSM));
			if (steps >= 28) {
				step_endpoint_config();
			}
			if (steps >= 30) {
				step_bhi();
			}
			if (steps >= 31) {
				step_capabilities();
			}
			if (steps >= 35) {
				step_smmu_map();
			}
			if (steps >= 36) {
				step_bhi_download();
			}
			if (steps >= 37) {
				step_after_download();
			}
			if (steps >= 38) {
				step_bhi_retry();
			}
			if (steps >= 39) {
				step_iatu_map();
			}
			if (steps >= 40) {
				step_bdf_to_sid();
			}
			if (steps >= 41) {
				step_sid_table();
			}
			if (steps >= 42) {
				step_stream_read();
			}
			if (steps >= 43) {
				step_iatu_inbound();
			}
			if (steps >= 44) {
				step_bhi_base();
			}
			return;
		}
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

/*
 * Step 24: the endpoint brought up once, in one place, in the order the vendor keeps -- with the
 * delays it actually needs rather than the shortest that compiled.
 *
 * The one boot that reached L0 did it on the first try, and the boots after it did not, which is
 * the signature of a sequence that is right but has no margin: 5 ms between the rails and WLAN_EN,
 * 10 ms between WLAN_EN and PERST#. The vendor's own driver waits 1 ms for PERST# propagation, 10
 * ms for the endpoint's latency (qcom,ep-latency), and the chip's own spec sheet wants tens of
 * milliseconds after its enable. So: rails, 100 ms, WLAN_EN low then high, 50 ms, PERST# asserted
 * for 20 ms and released, 20 ms, and only then the LTSSM.
 *
 * The flush bit is the other half. The state machine sets it again every time it is asked to start
 * and the endpoint does not answer in time, and a flushed link sends no detect pulse at all -- so
 * the trigger here clears it, enables, and if the state has not moved by the next round, clears it
 * again rather than waiting for a state machine that is not running.
 */
#define ECAM_EP_PHYS    0x60200000UL    /* (dt: ranges map bus 1 here) */

static void
step_link_final(void)
{
	vm_offset_t tlmm;
	uint32_t now;
	unsigned i;

	if (parf == 0 || dbi == 0 || elbi == 0) {
		tell("pcie: 24: a window is missing\n");
		return;
	}
	tlmm = ml_io_map(0x0f900000UL, 0x0007d000UL);
	if (tlmm == 0) {
		tell("pcie: 24: the north tile could not be mapped\n");
		return;
	}
#define PIN_CFG(n)      (tlmm + 0x1000 * (n))
#define PIN_IO(n)       (tlmm + 0x1000 * (n) + 4)
	tell("pcie: 24: waiting 100 ms for the rails before touching the endpoint's pads\n");
	delay_for_interval(100000, NSEC_PER_USEC);
	wr(PIN_CFG(20), 0x3c3);                         /* (wlan-en: gpio, 16 mA, pull-up, output) */
	wr(PIN_IO(20), rd(PIN_IO(20)) & ~2u);
	delay_for_interval(20000, NSEC_PER_USEC);
	wr(PIN_IO(20), rd(PIN_IO(20)) | 2u);
	tell("pcie: 24: wlan-en low 20 ms, then high (level %u); 50 ms for the chip\n", rd(PIN_IO(20)) & 1);
	delay_for_interval(50000, NSEC_PER_USEC);
	wr(PIN_CFG(79), 0x200);                         /* (perst: an output, 2 mA, no pull) */
	wr(PIN_IO(79), 2);
	delay_for_interval(20000, NSEC_PER_USEC);
	wr(PIN_IO(79), 0);
	tell("pcie: 24: perst asserted 20 ms and released (level %u); 20 ms, then the ltssm\n",
	    rd(PIN_IO(79)) & 1);
	delay_for_interval(20000, NSEC_PER_USEC);

	for (i = 1; i <= 40; i++) {
		now = rd(parf + PARF_LTSSM);
		if ((now & 0x3f) == 0x11) {
			break;
		}
		if ((now & (1u << 11)) != 0) {
			wr(parf + PARF_LTSSM, (now & ~(1u << 11) & ~(1u << 10)) | (1u << 8));
		}
		delay_for_interval(20000, NSEC_PER_USEC);
		now = rd(parf + PARF_LTSSM);
		tell("pcie: 24:   try %u: parf ltssm %#x (state %#x, flush %u), dbi link %#x\n", i, now, now & 0x3f,
		    (now >> 11) & 1, rd(dbi + DBI_LINK_CTRL_STATUS));
	}
	now = rd(parf + PARF_LTSSM);
	if ((now & 0x3f) != 0x11 && ((rd(dbi + DBI_LINK_CTRL_STATUS) >> 29) & 1) == 0) {
		tell("pcie: 24: still down after 30 rounds: state %#x, flush %u, dbi link %#x\n", now & 0x3f,
		    (now >> 11) & 1, rd(dbi + DBI_LINK_CTRL_STATUS));
		return;
	}
	tell("pcie: 24: LINK UP: parf ltssm %#x (state %#x), dbi link ctrl/status %#x (speed %#x, width %#x, active %u)\n",
	    now, now & 0x3f, rd(dbi + DBI_LINK_CTRL_STATUS), rd(dbi + DBI_LINK_CTRL_STATUS) & 0xf,
	    (rd(dbi + DBI_LINK_CTRL_STATUS) >> 4) & 0x3f, (rd(dbi + DBI_LINK_CTRL_STATUS) >> 29) & 1);
	{
		vm_offset_t ep = ml_io_map(ECAM_EP_PHYS, 0x00010000UL);

		if (ep == 0) {
			tell("pcie: 24: the endpoint's configuration window could not be mapped\n");
			return;
		}
		tell("pcie: 24: endpoint 0x60200000: id %#x (vendor %#x, device %#x), class %#x\n", rd(ep), rd(ep) & 0xffff,
		    rd(ep) >> 16, rd(ep + 0x08));
		tell("pcie: 24:   command/status %#x, header %#x, bar0 %#x, bar1 %#x\n", rd(ep + 0x04), rd(ep + 0x0c),
		    rd(ep + 0x10), rd(ep + 0x14));
	}
#undef PIN_CFG
#undef PIN_IO
}

/*
 * Step 25: the flush, cleared the way the vendor clears it.
 *
 * Every step since 21 has been writing `(ltssm & ~BIT(11)) & ~BIT(10)` and expecting the state
 * machine to run. pci-msm.c:5463-5472 shows that this is backwards:
 *
 *	msm_pcie_write_mask(dev->parf + PCIE20_PARF_LTSSM, 0, SW_CLR_FLUSH_MODE);
 *	... wait for FLUSH_MODE to clear ...
 *
 * write_mask(addr, mask, val) is `(read & ~mask) | val`, so that call *sets* bit 10 -- SW_CLR_FLUSH_MODE
 * is a pulse that asks the hardware to clear bit 11, it is not a bit to clear. Writing zero into it
 * asks for nothing, which is why the state kept reading 0x905 a millisecond later.
 *
 * The vendor also waits first: the flush has to have finished (PARF_STATUS bit 8, FLUSH_COMPLETED)
 * before it is cleared. All three of those are done here, in that order, then LTSSM is enabled and
 * the state is read until it moves.
 */
static void
step_flush_proper(void)
{
	uint32_t status, now;
	unsigned i, done;

	status = 0;
	for (i = 0; i < 200; i++) {
		status = rd(parf + PARF_STATUS);
		if (status & (1u << 8)) {                       /* (FLUSH_COMPLETED) */
			break;
		}
		delay_for_interval(10, NSEC_PER_USEC);
	}
	tell("pcie: 25: parf status %#x, flush completed %s (after %u tries)\n", status, (status & (1u << 8)) ? "yes" : "NOT",
	    i);
	now = rd(parf + PARF_LTSSM);
	tell("pcie: 25: ltssm before %#x (state %#x, flush %u)\n", now, now & 0x3f, (now >> 11) & 1);
	wr(parf + PARF_LTSSM, (now & ~(1u << 11)) | (1u << 10) | (1u << 8));   /* SW_CLR_FLUSH_MODE, then LTSSM_EN */
	for (done = 0, i = 0; i < 200; i++) {
		now = rd(parf + PARF_LTSSM);
		if ((now & (1u << 11)) == 0) {
			done = 1;
			break;
		}
		delay_for_interval(10, NSEC_PER_USEC);
	}
	tell("pcie: 25: ltssm after %#x (state %#x, flush %u): flush %s after %u tries\n", now, now & 0x3f,
	    (now >> 11) & 1, done ? "cleared" : "STILL SET", i);
	if (!done) {
		return;
	}
	for (i = 1; i <= 30; i++) {
		now = rd(parf + PARF_LTSSM);
		if ((now & 0x3f) == 0x11 || ((rd(dbi + DBI_LINK_CTRL_STATUS) >> 29) & 1) != 0) {
			tell("pcie: 25: LINK UP after %u tries: parf ltssm %#x (state %#x), dbi link %#x (active %u)\n", i, now,
			    now & 0x3f, rd(dbi + DBI_LINK_CTRL_STATUS), (rd(dbi + DBI_LINK_CTRL_STATUS) >> 29) & 1);
			{
				vm_offset_t ep = ml_io_map(ECAM_EP_PHYS, 0x00010000UL);

				if (ep != 0) {
					tell("pcie: 25: endpoint: id %#x (vendor %#x, device %#x), class %#x, bar0 %#x\n", rd(ep),
					    rd(ep) & 0xffff, rd(ep) >> 16, rd(ep + 0x08), rd(ep + 0x10));
				}
			}
			return;
		}
		if ((i % 5) == 0) {
			tell("pcie: 25:   try %u: parf ltssm %#x (state %#x, flush %u), dbi link %#x\n", i, now, now & 0x3f,
			    (now >> 11) & 1, rd(dbi + DBI_LINK_CTRL_STATUS));
		}
		delay_for_interval(5000, NSEC_PER_USEC);
	}
	tell("pcie: 25: end: state %#x, flush %u, dbi link ctrl/status %#x\n", rd(parf + PARF_LTSSM) & 0x3f,
	    (rd(parf + PARF_LTSSM) >> 11) & 1, rd(dbi + DBI_LINK_CTRL_STATUS));
}

/*
 * Step 26: the endpoint tried three times in one boot, and the link state judged honestly.
 *
 * Step 25 cleared the flush the vendor's way -- a pulse into SW_CLR_FLUSH_MODE -- and the bit did
 * clear, but the state stayed at 5. Step 21 cleared bit 11 directly and the state went to 0. Both
 * are true, so this step does the direct clear that moved it, and judges the result on a register
 * that cannot lie: 0xffffffff has bit 29 set, so "active" from the DBI's link word means nothing
 * unless the read itself succeeded.
 *
 * The three attempts exist because the one boot that reached L0 did it on the first try and the
 * next six did not: whatever the endpoint is waiting for is marginal, and a sequence that is right
 * but has no margin shows up exactly like this. Each attempt leaves the rails alone, pulses WLAN_EN,
 * resets the endpoint, and only then touches the LTSSM; the delay after WLAN_EN grows with each
 * attempt, because a chip that needs longer is the obvious candidate.
 */
#define ECAM_EP_PHYS    0x60200000UL

static bool
link_is_up(void)
{
	uint32_t link = rd(dbi + DBI_LINK_CTRL_STATUS);

	return (rd(parf + PARF_LTSSM) & 0x3f) == 0x11 || (link != 0xffffffffu && ((link >> 29) & 1) != 0);
}

static void
step_endpoint_attempts(void)
{
	vm_offset_t tlmm;
	unsigned attempt, i;

	if (parf == 0 || dbi == 0) {
		tell("pcie: 26: a window is missing\n");
		return;
	}
	tlmm = ml_io_map(0x0f900000UL, 0x0007d000UL);
	if (tlmm == 0) {
		tell("pcie: 26: the north tile could not be mapped\n");
		return;
	}
#define PIN_CFG(n)      (tlmm + 0x1000 * (n))
#define PIN_IO(n)       (tlmm + 0x1000 * (n) + 4)
	wr(PIN_CFG(20), 0x3c3);
	wr(PIN_CFG(79), 0x200);
	for (attempt = 1; attempt <= 3; attempt++) {
		unsigned settle = 50 * attempt;         /* (50, 100, 150 ms after WLAN_EN) */

		tell("pcie: 26: attempt %u: wlan-en low, %u ms settle, perst, then the ltssm\n", attempt, settle);
		wr(PIN_IO(20), rd(PIN_IO(20)) & ~2u);
		delay_for_interval(20000, NSEC_PER_USEC);
		wr(PIN_IO(20), rd(PIN_IO(20)) | 2u);
		delay_for_interval((uint64_t)settle * 1000, NSEC_PER_USEC);
		wr(PIN_IO(79), 2);
		delay_for_interval(20000, NSEC_PER_USEC);
		wr(PIN_IO(79), 0);
		delay_for_interval(20000, NSEC_PER_USEC);

		{
			uint32_t now = rd(parf + PARF_LTSSM);

			wr(parf + PARF_LTSSM, (now & ~(1u << 11) & ~(1u << 10)) | (1u << 8));
		}
		for (i = 1; i <= 20; i++) {
			delay_for_interval(20000, NSEC_PER_USEC);
			if (link_is_up()) {
				break;
			}
		}
		tell("pcie: 26: attempt %u: parf ltssm %#x (state %#x, flush %u), dbi link %#x after %u tries\n", attempt,
		    rd(parf + PARF_LTSSM), rd(parf + PARF_LTSSM) & 0x3f, (rd(parf + PARF_LTSSM) >> 11) & 1,
		    rd(dbi + DBI_LINK_CTRL_STATUS), i);
		if (link_is_up()) {
			vm_offset_t ep = ml_io_map(ECAM_EP_PHYS, 0x00010000UL);

			tell("pcie: 26: LINK UP on attempt %u: dbi link ctrl/status %#x (speed %#x, width %#x, active %u)\n",
			    attempt, rd(dbi + DBI_LINK_CTRL_STATUS), rd(dbi + DBI_LINK_CTRL_STATUS) & 0xf,
			    (rd(dbi + DBI_LINK_CTRL_STATUS) >> 4) & 0x3f, (rd(dbi + DBI_LINK_CTRL_STATUS) >> 29) & 1);
			if (ep != 0) {
				tell("pcie: 26: endpoint at 0x60200000: id %#x (vendor %#x, device %#x), class %#x, bar0 %#x\n",
				    rd(ep), rd(ep) & 0xffff, rd(ep) >> 16, rd(ep + 0x08), rd(ep + 0x10));
			}
			goto out;
		}
	}
	tell("pcie: 26: none of the three attempts trained the link\n");
out:
#undef PIN_CFG
#undef PIN_IO
	;
}

/*
 * Step 27: the endpoint brought out of reset, which is what every step before this one got wrong.
 *
 * Two corrections, both from other alioth ports rather than from this file's own reasoning:
 *
 * 1. PERST# is active low. The vendor's GPIO table (pci-msm.c:939) has "on" = 1 for perst, and its
 *    assert is `gpio_set_value(num, on)`, so *asserted is a low level and released is a high one* --
 *    MrMoxing/ios-alioth's alioth_pcie.c:126-132 says it in as many words, and the PCIe reset line is
 *    named PERST# for that reason. Every step here before now drove the line high to "assert" and low
 *    to "release", which held the QCA6390 in reset while the controller was told to train. The one
 *    boot that reached L0 is the one where step 19's second polarity attempt left the line high.
 *
 * 2. ELBI_SYS_CTRL bit 0 is ELBI_SYS_CTRL_LT_ENABLE -- "enable link training" -- and nothing in this
 *    file ever wrote it (pcie-qcom.c:287-293, qcom_pcie_2_1_0_ltssm_enable).
 *
 * The order is the vendor's: hold the endpoint in reset while everything comes up, then power (done),
 * then WLAN_EN, then release the reset, then enable the link, then look at the core's port debug
 * registers -- 0x72c bit 4 is LINK_UP and bit 5 is LINK_IN_TRAINING, which is what mainline's
 * dw_pcie_link_up() actually reads.
 */
#define DBI_PORT_DEBUG0         0x0728
#define DBI_PORT_DEBUG1         0x072c
#define DBI_PORT_DEBUG1_LINK_UP (1u << 4)
#define DBI_PORT_DEBUG1_IN_TRAIN (1u << 5)

static bool
link_up_now(void)
{
	uint32_t debug1 = rd(dbi + DBI_PORT_DEBUG1);
	uint32_t link = rd(dbi + DBI_LINK_CTRL_STATUS);

	if ((rd(parf + PARF_LTSSM) & 0x3f) == 0x11) {
		return true;
	}
	/*
	 * 0xffffffff is what this interconnect returns when the slave did not answer, and its bit 4
	 * (debug1's LINK_UP) and bit 29 (the link word's data-link-active) are both set in it. An
	 * error response is not a link, and reading it as one made step 27 announce "LINK IS UP" on a
	 * window that was answering nothing at all.
	 */
	if (debug1 == 0xffffffffu || link == 0xffffffffu) {
		return false;
	}
	return (debug1 & DBI_PORT_DEBUG1_LINK_UP) != 0 || ((link >> 29) & 1) != 0;
}

/*
 * ltssm_restart: the vendor's link-down recovery followed by a real start edge.
 *
 * Everything here is measured: after a boot that already trained the link, the next boot's PERST#
 * assert drops it, the controller goes into flush mode with the state at 0x5, and simply writing
 * LTSSM_EN again does nothing -- the bit was already set, and a level is not an edge. So: wait for
 * the flush to have finished (PARF_STATUS bit 8), pulse SW_CLR_FLUSH_MODE (bit 10, a request, not a
 * bit to clear), wait for FLUSH_MODE (bit 11) to fall, then drop LTSSM_EN and raise it again.
 */
static void
ltssm_restart(void)
{
	uint32_t now;
	unsigned i;

	for (i = 0; i < 200; i++) {
		if ((rd(parf + PARF_STATUS) & (1u << 8)) != 0) {
			break;
		}
		delay_for_interval(10, NSEC_PER_USEC);
	}
	now = rd(parf + PARF_LTSSM);
	wr(parf + PARF_LTSSM, (now & ~(1u << 11)) | (1u << 10) | (1u << 8));
	for (i = 0; i < 200; i++) {
		now = rd(parf + PARF_LTSSM);
		if ((now & (1u << 11)) == 0) {
			break;
		}
		delay_for_interval(10, NSEC_PER_USEC);
	}
	wr(parf + PARF_LTSSM, now & ~(1u << 8));                /* (stop: the edge needs a low) */
	delay_for_interval(2000, NSEC_PER_USEC);
	wr(parf + PARF_LTSSM, (now & ~(1u << 8)) | (1u << 8));  /* (and start again) */
	now = rd(parf + PARF_LTSSM);
	tell("pcie: 27: ltssm restarted: %#x (state %#x, flush %u)\n", now, now & 0x3f, (now >> 11) & 1);
}

static void
step_endpoint_polarity(void)
{
	vm_offset_t tlmm;
	uint32_t now;
	unsigned i;

	if (parf == 0 || dbi == 0 || elbi == 0) {
		tell("pcie: 27: a window is missing\n");
		return;
	}
	/*
	 * A measured rule: if the link is already up, do not touch PERST# at all.
	 *
	 * Step 18 ends with a link that is up (its own log says elbi 0x5401, bit 10 set), and the
	 * steps after it used to assert PERST# anyway -- and from that point the wrapper's whole slave
	 * aperture (dm_core 0x60000000 and the ELBI with it) answers 0xffffffff while PARF keeps
	 * answering, clocks running and both resets released. Taking a trained link down is what
	 * precedes it, so an intact link is left intact.
	 */
	if (link_up_now()) {
		tell("pcie: 27: the link is already up (ltssm %#x, elbi %#x); PERST# is left alone\n",
		    rd(parf + PARF_LTSSM), rd(elbi + ELBI_SYS_STTS));
		return;
	}
	tlmm = ml_io_map(0x0f900000UL, 0x0007d000UL);
	if (tlmm == 0) {
		tell("pcie: 27: the north tile could not be mapped\n");
		return;
	}
#define PIN_CFG(n)      (tlmm + 0x1000 * (n))
#define PIN_IO(n)       (tlmm + 0x1000 * (n) + 4)
	/* perst: the level first, then the pin made an output -- it must never drive the wrong one. */
	wr(PIN_IO(79), 0);                               /* (asserted: low) */
	wr(PIN_CFG(79), 0x200);                          /* (output, 2 mA, no pull) */
	wr(PIN_CFG(20), 0x3c3);                          /* (wlan-en: gpio, 16 mA, pull-up, output) */
	tell("pcie: 27: perst asserted (low, level %u) -- the endpoint is held in reset\n", rd(PIN_IO(79)) & 1);
	delay_for_interval(10000, NSEC_PER_USEC);

	wr(PIN_IO(20), rd(PIN_IO(20)) & ~2u);
	delay_for_interval(20000, NSEC_PER_USEC);
	wr(PIN_IO(20), rd(PIN_IO(20)) | 2u);
	tell("pcie: 27: wlan-en low 20 ms then high (level %u); 50 ms for the chip's own PMU\n",
	    rd(PIN_IO(20)) & 1);
	delay_for_interval(50000, NSEC_PER_USEC);

	wr(PIN_IO(79), 2);
	tell("pcie: 27: perst RELEASED (high, level %u); 1 ms, then the link\n", rd(PIN_IO(79)) & 1);
	delay_for_interval(1000, NSEC_PER_USEC);

	now = rd(elbi + ELBI_SYS_CTRL);
	wr(elbi + ELBI_SYS_CTRL, now | 1u);
	tell("pcie: 27: elbi sys_ctrl %#x -> %#x (LT_ENABLE), port debug1 %#x\n", now, rd(elbi + ELBI_SYS_CTRL),
	    rd(dbi + DBI_PORT_DEBUG1));
	/*
	 * The plain enable, as it was in the runs that ended with an endpoint that answered: clear the
	 * two flush bits and raise LTSSM_EN in one write. The helper below (a vendor-style pulse plus a
	 * low-high edge on LTSSM_EN) is kept for the retry path, because the boots that worked did not
	 * use it here and the boots that broke the slave aperture did.
	 */
	now = rd(parf + PARF_LTSSM);
	wr(parf + PARF_LTSSM, (now & ~(1u << 11) & ~(1u << 10)) | (1u << 8));
	tell("pcie: 27: parf ltssm %#x -> %#x (plain enable)\n", now, rd(parf + PARF_LTSSM));

	for (i = 1; i <= 40; i++) {
		delay_for_interval(20000, NSEC_PER_USEC);
		now = rd(parf + PARF_LTSSM);
		if (link_up_now()) {
			break;
		}
		if ((i % 5) == 0) {
			tell("pcie: 27:   try %u: ltssm %#x (state %#x, flush %u), debug0 %#x, debug1 %#x, dbi link %#x\n",
			    i, now, now & 0x3f, (now >> 11) & 1, rd(dbi + DBI_PORT_DEBUG0), rd(dbi + DBI_PORT_DEBUG1),
			    rd(dbi + DBI_LINK_CTRL_STATUS));
			if ((now & 0x3f) == 0x5 && (i % 10) == 0) {
				ltssm_restart();        /* (a state that has not moved gets a fresh edge) */
			}
		}
	}
	now = rd(parf + PARF_LTSSM);
	tell("pcie: 27: after %u tries: ltssm %#x (state %#x, flush %u), debug0 %#x, debug1 %#x, dbi link %#x\n", i, now,
	    now & 0x3f, (now >> 11) & 1, rd(dbi + DBI_PORT_DEBUG0), rd(dbi + DBI_PORT_DEBUG1),
	    rd(dbi + DBI_LINK_CTRL_STATUS));
	if (link_up_now()) {
		vm_offset_t ep = ml_io_map(ECAM_EP_PHYS, 0x00010000UL);

		tell("pcie: 27: LINK IS UP (state %#x)\n", now & 0x3f);
		if (ep != 0) {
			tell("pcie: 27: endpoint 0x60200000: id %#x (vendor %#x, device %#x), class %#x, bar0 %#x\n",
			    rd(ep), rd(ep) & 0xffff, rd(ep) >> 16, rd(ep + 0x08), rd(ep + 0x10));
		}
	}
#undef PIN_CFG
#undef PIN_IO
}

/*
 * Step 28: the endpoint's configuration space, reached the way this controller reaches it.
 *
 * The link is at L0 (step 27), and 0x60200000 still read 0xffffffff -- because on this IP the
 * configuration window is not an ECAM: a read has to go through the address translation unit's
 * region 0, which is what msm_pcie_cfg_bdf() programs before every access
 * (pci-msm.c:3060-3085):
 *
 *	msm_pcie_iatu_config(dev, 0, bus == 1 ? CFG0 : CFG1,
 *	                     conf->start, conf->start + 4K - 1, BDF_OFFSET(bus, devfn));
 *	... and then the register is read at conf->start + (offset & 0xfff)
 *
 * with BDF_OFFSET(bus, devfn) = (bus << 24) | (devfn << 16), the region's ctrl1 = 4 for a bus-1
 * target (5 for anything further), and ctrl2's bit 31 enabling the region after it is written.
 *
 * So: region 0 is aimed at bus 1, device 0, function 0; the window at 0x60100000 (4 KB of it) is
 * then the endpoint's configuration space, and the vendor and device ids at its offset 0 are the
 * answer to whether the QCA6390 (17cb:1101) is on the other end of this link.
 */
#define IATU_PHYS       0x60001000UL
#define IATU_SIZE       0x00001000UL
#define CONF_PHYS       0x60100000UL
#define CONF_SIZE       0x00001000UL
#define BDF(bus, devfn) (((uint32_t)(bus) << 24) | ((uint32_t)(devfn) << 16))

static void
iatu_region0(vm_offset_t iatu, uint32_t type, uint32_t host, uint32_t host_end, uint32_t target)
{
	wr(iatu + 0x04, 0);                     /* (the region is switched off before it changes) */
	wr(iatu + 0x00, type);
	wr(iatu + 0x08, host);
	wr(iatu + 0x0c, 0);
	wr(iatu + 0x10, host_end);
	wr(iatu + 0x14, target);
	wr(iatu + 0x18, 0);
	wr(iatu + 0x04, 1u << 31);              /* (and on again) */
}

static void
step_endpoint_config(void)
{
	vm_offset_t iatu, conf;
	uint32_t id;

	if (parf == 0) {
		tell("pcie: 28: no parf window\n");
		return;
	}
	if ((rd(parf + PARF_LTSSM) & 0x3f) != 0x11) {
		tell("pcie: 28: the link is not in L0 (state %#x); the configuration space will not answer\n",
		    rd(parf + PARF_LTSSM) & 0x3f);
	}
	iatu = ml_io_map(IATU_PHYS, IATU_SIZE);
	conf = ml_io_map(CONF_PHYS, CONF_SIZE);
	if (iatu == 0 || conf == 0) {
		tell("pcie: 28: the iatu (%#lx) or the conf window (%#lx) could not be mapped\n",
		    (unsigned long) IATU_PHYS, (unsigned long) CONF_PHYS);
		return;
	}
	iatu_region0(iatu, 0x04, CONF_PHYS, CONF_PHYS + 0xfff, BDF(1, 0));    /* (type 4: a bus-1 target) */
	tell("pcie: 28: iatu region 0 for bus 1 dev 0: ctrl1 %#x, lbar %#x, lar %#x, ltar %#x, ctrl2 %#x\n",
	    rd(iatu + 0x00), rd(iatu + 0x08), rd(iatu + 0x10), rd(iatu + 0x14), rd(iatu + 0x04));

	id = rd(conf + 0x00);
	tell("pcie: 28: endpoint 01:00.0: id %#x (vendor %#x, device %#x)%s\n", id, id & 0xffff, id >> 16,
	    (id == 0xffffffffu) ? " -- nothing answered" : "");
	if (id == 0xffffffffu) {
		return;
	}
	tell("pcie: 28:   command/status %#x, revision/class %#x, header %#x, cache/latency/type/bist %#x\n",
	    rd(conf + 0x04), rd(conf + 0x08), rd(conf + 0x0c), rd(conf + 0x0c));
	for (unsigned b = 0; b < 6; b++) {
		tell("pcie: 28:   bar%u = %#x\n", b, rd(conf + 0x10 + 4 * b));
	}
	tell("pcie: 28:   subsystem id %#x, cap pointer %#x\n", rd(conf + 0x2c), rd(conf + 0x34));
}

/*
 * Step 29: the endpoint's BAR, its memory window, and the two command bits that make them live.
 *
 * The configuration space answers (step 28), so the endpoint can be asked what it wants and given
 * it. Three pieces, in the order a host does them:
 *
 * 1. size bar0 by writing all ones into it (and into bar1, because bar0's low bits say it is a
 *    64-bit memory BAR) and reading the mask back, then restore what was there;
 * 2. give it an address out of the controller's memory window (dt: 0x60300000 + 0x3d00000), and
 *    tell the root bridge about that window in its own type-1 header (memory base/limit, 0x20);
 * 3. point iATU region 1 at that window (type 0, memory, identity) and set the endpoint's command
 *    bits 1 and 2 -- memory space and bus master.
 *
 * Each write says what it is about to do before it does it: the first access through a window that
 * is not configured is the one that hangs, and a log that stops after "about to" names the culprit.
 */
#define EP_BAR_ADDRESS  0x60400000UL    /* (inside the dt's memory window, 1 MB aligned) */
#define EP_BAR_SIZE     0x00100000UL

static vm_offset_t endpoint_bar(void);   /* (defined with step 30: one mapping, reused) */

static void
step_endpoint_bar(void)
{
	vm_offset_t iatu, conf, bars;
	uint32_t save0, save1, mask0, mask1;

	if (parf == 0 || dbi == 0) {
		tell("pcie: 29: a window is missing\n");
		return;
	}
	iatu = ml_io_map(IATU_PHYS, IATU_SIZE);
	conf = ml_io_map(CONF_PHYS, 0x1000);
	if (iatu == 0 || conf == 0) {
		tell("pcie: 29: the iatu or the conf window could not be mapped\n");
		return;
	}
	iatu_region0(iatu, 0x04, CONF_PHYS, CONF_PHYS + 0xfff, BDF(1, 0));
	if (rd(conf + 0x00) != 0x110117cb) {
		tell("pcie: 29: the endpoint does not answer (%#x); nothing else is written\n", rd(conf + 0x00));
		return;
	}

	/* 1. how big is bar0 */
	save0 = rd(conf + 0x10);
	save1 = rd(conf + 0x14);
	tell("pcie: 29: bar0 %#x bar1 %#x before sizing\n", save0, save1);
	wr(conf + 0x10, 0xffffffffu);
	wr(conf + 0x14, 0xffffffffu);
	mask0 = rd(conf + 0x10);
	mask1 = rd(conf + 0x14);
	wr(conf + 0x10, save0);
	wr(conf + 0x14, save1);
	tell("pcie: 29: bar0 mask %#x, bar1 mask %#x -> %s, size %#x\n", mask0, mask1,
	    (mask0 & 0x6) == 0x4 ? "a 64-bit memory bar" : "not a 64-bit memory bar",
	    ~(uint32_t)(mask0 & ~0xfu) + 1);

	/* 2. an address, and the root bridge's window over it */
	wr(conf + 0x10, (uint32_t) EP_BAR_ADDRESS);
	wr(conf + 0x14, (uint32_t)((uint64_t) EP_BAR_ADDRESS >> 32));
	tell("pcie: 29: bar0 = %#x, bar1 = %#x\n", rd(conf + 0x10), rd(conf + 0x14));
	{
		uint32_t base = ((uint32_t) EP_BAR_ADDRESS >> 16) & 0xfff0u;
		uint32_t limit = ((uint32_t)(EP_BAR_ADDRESS + EP_BAR_SIZE - 1) >> 16) & 0xfff0u;
		uint32_t was = rd(dbi + 0x20);

		wr(dbi + 0x20, (limit << 16) | base);
		tell("pcie: 29: root bridge memory base/limit %#x -> %#x (base %#x, limit %#x)\n", was,
		    rd(dbi + 0x20), base << 16, limit << 16);
		wr(dbi + 0x24, 0);                      /* (the upper 32 bits of both: inside 4 GB) */
	}

	/* 3. iATU region 1: an identity window for memory, then the command bits */
	iatu_region0(iatu + 0x200, 0x00, EP_BAR_ADDRESS, EP_BAR_ADDRESS + EP_BAR_SIZE - 1, EP_BAR_ADDRESS);
	tell("pcie: 29: iatu region 1: ctrl1 %#x, lbar %#x, lar %#x, ltar %#x, ctrl2 %#x\n", rd(iatu + 0x200),
	    rd(iatu + 0x208), rd(iatu + 0x210), rd(iatu + 0x214), rd(iatu + 0x204));
	{
		uint32_t command = rd(conf + 0x04);

		wr(conf + 0x04, (command & 0xffff0000u) | 0x0006u);   /* (mem space, bus master) */
		tell("pcie: 29: command %#x -> %#x\n", command, rd(conf + 0x04));
	}

	bars = endpoint_bar();
	if (bars == 0) {
		tell("pcie: 29: bar0's window could not be mapped\n");
		return;
	}
	tell("pcie: 29: about to read the endpoint's registers through bar0 (%#lx) -- if nothing follows, that read hung\n",
	    bars);
	tell("pcie: 29: bar0[0x00] %#x, [0x04] %#x, [0x08] %#x, [0x0c] %#x\n", rd(bars), rd(bars + 0x04),
	    rd(bars + 0x08), rd(bars + 0x0c));
}

/*
 * Step 30: the chip's register window, and the BHI block behind it.
 *
 * The BAR is 1 MB, and the MHI/BHI registers live at 0x1e0e000 and up -- outside it. QCA6390 does
 * what several Qualcomm parts do: the upper half of the BAR is a window, and which 0x80000-byte
 * slice it shows is chosen by writing to a control register (cnss2/reg.h:66-71, cnss2/pci.c:427-472):
 *
 *	window = (offset >> 19) & 0x3f;
 *	write BAR + 0x310c = 0x40000000 | window;        (REMAP_BAR_CTRL)
 *	read  BAR + 0x80000 + (offset & 0x7ffff);
 *
 * So the BHI block at bhi_base + 0x00 is window 0x3c, in-window offset 0xe200. The registers are the ones
 * mhi_internal.h names (k40-ref, drivers/bus/mhi/core/mhi_internal.h:169-198): version minor/major
 * at 0x0/0x4, exec environment at 0x28, status at 0x2c, error code at 0x30, the three debug words
 * after it. A block that answers with a version is a block the host can talk to.
 */
#define REMAP_BAR_CTRL  0x310c          /* (cnss2 reg.h:7) */
#define WINDOW_BIT      0x40000000u
#define WINDOW_START    0x00080000u
#define WINDOW_MASK     0x0007ffffu

static uint32_t
windowed_read(vm_offset_t bar, uint32_t offset)
{
	uint32_t window = (offset >> 19) & 0x3f;

	wr(bar + REMAP_BAR_CTRL, WINDOW_BIT | window);
	return rd(bar + WINDOW_START + (offset & WINDOW_MASK));
}

static vm_offset_t
endpoint_bar(void)
{
	static vm_offset_t cached;

	if (cached == 0) {
		cached = ml_io_map(EP_BAR_ADDRESS, 0x100000);
	}
	return cached;
}

static void
step_bhi(void)
{
	vm_offset_t bar;
	static const struct {
		uint32_t at;
		const char *what;
	} bhi[] = {
		{ 0x00, "BHI version minor" },
		{ 0x04, "BHI version major" },
		{ 0x08, "BHI image address low" },
		{ 0x10, "BHI image size" },
		{ 0x28, "BHI exec environment" },
		{ 0x2c, "BHI status" },
		{ 0x30, "BHI error code" },
		{ 0x34, "BHI error debug 1" },
		{ 0x38, "BHI error debug 2" },
		{ 0x3c, "BHI error debug 3" },
		{ 0x40, "BHI serial number" },
		{ 0x44, "BHI SBL anti-rollback version" },
		{ 0x48, "BHI number of segments" },
	};

	if (parf == 0) {
		tell("pcie: 30: no parf window\n");
		return;
	}
	if ((rd(parf + PARF_LTSSM) & 0x3f) != 0x11) {
		tell("pcie: 30: the link is not at L0 (state %#x)\n", rd(parf + PARF_LTSSM) & 0x3f);
	}
	bar = endpoint_bar();                           /* (mapped once: 1 MB of kernel map is not free to waste) */
	if (bar == 0) {
		tell("pcie: 30: the bar could not be mapped\n");
		return;
	}
	tell("pcie: 30: bar at %#lx; the unwindowed half first: [0x00] %#x, [0x04] %#x, [0x08] %#x, [0x0c] %#x\n",
	    bar, rd(bar), rd(bar + 0x04), rd(bar + 0x08), rd(bar + 0x0c));
	{
		uint32_t was = rd(bar + REMAP_BAR_CTRL);

		tell("pcie: 30: remap bar ctrl reads %#x before any window is selected\n", was);
	}
	for (unsigned i = 0; i < sizeof(bhi) / sizeof(bhi[0]); i++) {
		uint32_t at = bhi_base + bhi[i].at;
		uint32_t value = windowed_read(bar, at);

		tell("pcie: 30:   %#x %s: %#x\n", at, bhi[i].what, value);
	}
	tell("pcie: 30: the whole BHI page, base %#x (window %#x, in-window %#x):\n", bhi_base,
	    (bhi_base >> 19) & 0x3f, bhi_base & 0x7ffff);
	for (uint32_t off = 0; off <= 0x80; off += 16) {
		uint32_t at = bhi_base + off;

		tell("pcie: 30:   %#x: %#x %#x %#x %#x\n", at, windowed_read(bar, at), windowed_read(bar, at + 4),
		    windowed_read(bar, at + 8), windowed_read(bar, at + 12));
	}
}

/*
 * Step 31: the endpoint's capabilities, and the MSI one especially.
 *
 * Everything the MHI layer will need arrives as a message: the firmware's doorbells, the event rings'
 * updates, the completion of a download. That is the endpoint's MSI capability, and before it can be
 * programmed the host has to read what the endpoint asks for: how many vectors, whether the address
 * is 64-bit, and whether it has the per-vector mask and pending registers.
 *
 * The capability list starts at the header's 0x34 (its low byte, two low bits clear) and is a chain of
 * {id, next} byte pairs. MSI is id 0x05, MSI-X is 0x11; each entry's layout is the one in PCI 3.0:
 * for MSI, control at +2 (bits 2:0 vectors - 1 when 64-bit addressing, bit 7 64-bit capable, bit 8
 * per-vector masking) and the address at +4 / +8, then data; for MSI-X, control at +2 (bits 10:0
 * table size - 1), the table's offset and BIR at +4, the PBA's at +8.
 *
 * Device memory is read a word at a time and the bytes taken from the word: byte accesses to this
 * window are not what anything that works on this board does.
 */
static uint32_t
conf_word(vm_offset_t conf, uint32_t at)
{
	return rd(conf + (at & ~3u));
}

static uint8_t
conf_byte(vm_offset_t conf, uint32_t at)
{
	uint32_t word = conf_word(conf, at);

	return (uint8_t)(word >> (8 * (at & 3)));
}

static void
step_capabilities(void)
{
	vm_offset_t iatu, conf;
	uint32_t at;
	unsigned seen = 0;

	if (parf == 0) {
		tell("pcie: 31: no parf window\n");
		return;
	}
	if ((rd(parf + PARF_LTSSM) & 0x3f) != 0x11) {
		tell("pcie: 31: the link is not at L0 (state %#x)\n", rd(parf + PARF_LTSSM) & 0x3f);
	}
	iatu = ml_io_map(IATU_PHYS, IATU_SIZE);
	conf = ml_io_map(CONF_PHYS, 0x1000);
	if (iatu == 0 || conf == 0) {
		tell("pcie: 31: the iatu or the conf window could not be mapped\n");
		return;
	}
	iatu_region0(iatu, 0x04, CONF_PHYS, CONF_PHYS + 0xfff, BDF(1, 0));
	if (rd(conf + 0x00) != 0x110117cb) {
		tell("pcie: 31: the endpoint does not answer (%#x)\n", rd(conf + 0x00));
		return;
	}
	at = conf_byte(conf, 0x34) & 0xfc;
	tell("pcie: 31: status %#x, the capability list starts at %#x\n", conf_word(conf, 0x04) >> 16, at);
	while (at != 0 && seen++ < 16) {
		uint8_t id = conf_byte(conf, at);
		uint8_t next = conf_byte(conf, at + 1) & 0xfc;

		if (id == 0x05) {
			uint32_t control = conf_word(conf, at + 2) >> 16;
			unsigned vectors = (control & 0x7) + 1;
			uint32_t address_low = conf_word(conf, at + 4);
			uint32_t address_high = (control & 0x80) ? conf_word(conf, at + 8) : 0;
			unsigned data_at = (control & 0x80) ? 12 : 8;

			tell("pcie: 31:   MSI at %#x: control %#x -- %u vectors, 64-bit %s, per-vector mask %s\n", at, control,
			    vectors, (control & 0x80) ? "yes" : "no", (control & 0x100) ? "yes" : "no");
			tell("pcie: 31:     message address %#x%08x, data %#x, enable %u\n", address_high, address_low,
			    conf_word(conf, at + data_at) & 0xffff, control & 1);
			if (control & 0x100) {
				tell("pcie: 31:     mask %#x, pending %#x\n", conf_word(conf, at + data_at + 4),
				    conf_word(conf, at + data_at + 8));
			}
		} else if (id == 0x11) {
			uint32_t control = conf_word(conf, at + 2) >> 16;

			tell("pcie: 31:   MSI-X at %#x: control %#x -- %u vectors, table at %#x (bir %u), pba at %#x (bir %u), enable %u\n",
			    at, control, (control & 0x7ff) + 1, conf_word(conf, at + 4) & ~0x7u, conf_word(conf, at + 4) & 0x7,
			    conf_word(conf, at + 8) & ~0x7u, conf_word(conf, at + 8) & 0x7, (control >> 15) & 1);
		} else {
			tell("pcie: 31:   capability %#x at %#x\n", id, at);
		}
		if (next == at) {
			break;
		}
		at = next;
	}
	tell("pcie: 31: %u capabilities walked\n", seen);
}

/*
 * Step 32: what the SMMU in front of this endpoint already says, read only.
 *
 * The device tree maps the endpoint's RID to the apps SMMU (qcom,smmu-sid-base = 0x1c00; iommu-map
 * sends RID 0x0 to 0x1c00 and RID 0x100 -- bus 1 device 0, which is where the QCA6390 enumerated --
 * to 0x1c01). That SMMU is qcom,skip-init: nothing in this tree has ever programmed it, so its state
 * is whatever the boot loader left, and the question this step answers is the one that decides the
 * next piece of work: is this stream already bypassed (then the firmware download can use physical
 * addresses and no page tables are needed), already translating (then someone has tables for it),
 * or unmatched (then the global SCR0 decides, and we must add a match register ourselves).
 *
 * The register layout is hakios.camera.c's (SMMU_PHYS 0x15000000, SMR at 0x800 + 4n with the stream
 * in [14:0] and the mask in [30:16], S2CR at 0xc00 + 4n with the type in [17:16] and the context in
 * [7:0], GR1 one page block up, context n at (pages + n) * page size). Nothing is written.
 */
#define SMMU_PHYS       0x15000000UL
#define SMMU_SIZE       0x00100000UL
#define SMMU_SCR0       0x000
#define SMMU_IDR0       0x020
#define SMMU_IDR1       0x024
#define SMMU_SMR(n)     (0x800 + 4 * (n))
#define SMMU_S2CR(n)    (0xc00 + 4 * (n))

static void
step_smmu_look(void)
{
	static const uint32_t ours[] = { 0x1c00, 0x1c01 };
	vm_offset_t smmu;
	uint32_t idr0, idr1, scr0, page_size, pages;

	smmu = ml_io_map(SMMU_PHYS, 0x1000);            /* (GR0: scr0, idr, smr, s2cr all live here) */
	if (smmu == 0) {
		tell("pcie: 32: the apps smmu (%#lx) could not be mapped\n", (unsigned long) SMMU_PHYS);
		return;
	}
	scr0 = rd(smmu + SMMU_SCR0);
	idr0 = rd(smmu + SMMU_IDR0);
	idr1 = rd(smmu + SMMU_IDR1);
	page_size = (idr1 >> 31) ? 0x10000 : 0x1000;
	pages = 2u << ((idr1 >> 28) & 7);
	tell("pcie: 32: apps smmu: scr0 %#x (an unmatched stream %s), smrs %u, contexts %u, page size %#x, %u page blocks\n",
	    scr0, (scr0 & 0x400) ? "faults" : "passes", idr0 & 0xff, idr1 & 0xff, page_size, pages);
	if (2 * pages * page_size != SMMU_SIZE) {
		tell("pcie: 32: that layout is not the known one (2 * %u * %#x != %#x)\n", pages, page_size,
		    (unsigned) SMMU_SIZE);
	}
	{
		unsigned matched = 0, free_matches = 0;
		uint32_t first_free = ~0u;

		for (uint32_t n = 0; n < (idr0 & 0xff) && SMMU_SMR(n) < 0xc00; n++) {
			uint32_t smr = rd(smmu + SMMU_SMR(n)), s2cr = rd(smmu + SMMU_S2CR(n));
			uint32_t stream = smr & 0x7fff, mask = (smr >> 16) & 0x7fff;

			if ((smr >> 31) == 0) {
				free_matches++;
				if (first_free == ~0u) {
					first_free = n;
				}
				continue;
			}
			for (unsigned k = 0; k < sizeof(ours) / sizeof(ours[0]); k++) {
				if (((ours[k] ^ stream) & ~mask) != 0) {
					continue;
				}
				matched++;
				tell("pcie: 32:   smr %u: stream %#x mask %#x covers %#x; s2cr %#x = type %u (%s), context %u\n", n,
				    stream, mask, ours[k], s2cr, (s2cr >> 16) & 3,
				    ((s2cr >> 16) & 3) == 0 ? "translate" : ((s2cr >> 16) & 3) == 1 ? "bypass" :
				    ((s2cr >> 16) & 3) == 2 ? "fault" : "reserved", s2cr & 0xff);
			}
		}
		tell("pcie: 32: %u match registers free (first %u), %u of them cover our streams\n", free_matches, first_free,
		    matched);
		if (matched == 0) {
			tell("pcie: 32: nothing matches 0x1c00/0x1c01 -- scr0 decides, and scr0 %s\n",
			    (scr0 & 0x400) ? "faults" : "passes");
		}
	}
}

/*
 * Step 33: the endpoint's streams, bypassed in the apps SMMU.
 *
 * Step 32 showed why something has to be done: 87 match registers, 80 of them free, and not one
 * covers 0x1c00 or 0x1c01 -- and SCR0 says an unmatched stream faults. So the first DMA the chip
 * makes would be killed by the SMMU in front of it, with nothing in the log to say so.
 *
 * A bypass match is the smallest useful entry, and it is what the boot loader gives most streams:
 * S2CR's type 1 means "do not translate", and the address the chip is handed is then the physical
 * one. That is enough for the firmware download (the host says "read this physical buffer"), and it
 * is reversible: the previous SMR and S2CR values are printed before they are overwritten.
 *
 * The layout is hakios.camera.c's: SMR = bit31 valid | mask << 16 | stream, S2CR = type << 16 |
 * context, and the type is 0 translate, 1 bypass, 2 fault.
 */
#define SMMU_SCR0       0x000
#define SMMU_SMR(n)     (0x800 + 4 * (n))
#define SMMU_S2CR(n)    (0xc00 + 4 * (n))
#define S2CR_TYPE_BYPASS (1u << 16)

static void
step_smmu_bypass(void)
{
	static const uint32_t streams[] = { 0x1c01, 0x1c00 };   /* (bus1 dev0 first: that is our endpoint) */
	vm_offset_t smmu;
	uint32_t idr0;
	unsigned free_smr[2] = { ~0u, ~0u }, used = 0;

	smmu = ml_io_map(SMMU_PHYS, 0x1000);
	if (smmu == 0) {
		tell("pcie: 33: the apps smmu could not be mapped\n");
		return;
	}
	idr0 = rd(smmu + SMMU_IDR0);
	for (uint32_t n = 0; n < (idr0 & 0xff) && SMMU_SMR(n) < 0xc00 && used < 2; n++) {
		if ((rd(smmu + SMMU_SMR(n)) >> 31) == 0) {
			free_smr[used++] = n;
		}
	}
	if (used < 2) {
		tell("pcie: 33: only %u free match registers; nothing is written\n", used);
		return;
	}
	for (unsigned k = 0; k < 2; k++) {
		unsigned n = free_smr[k];
		uint32_t old_smr = rd(smmu + SMMU_SMR(n)), old_s2cr = rd(smmu + SMMU_S2CR(n));

		tell("pcie: 33: match register %u was smr %#x s2cr %#x; giving it stream %#x as bypass\n", n, old_smr,
		    old_s2cr, streams[k]);
		/*
		 * The type field is 2 bits at [17:16]; what the hardware does with a write is a reading,
		 * not an assumption, so all three values go in and are read back before the real one is
		 * left in place.
		 */
		for (uint32_t type = 0; type < 3; type++) {
			wr(smmu + SMMU_S2CR(n), type << 16);
			tell("pcie: 33:   s2cr: wrote type %u (%#x), reads %#x\n", type, type << 16, rd(smmu + SMMU_S2CR(n)));
		}
		wr(smmu + SMMU_S2CR(n), S2CR_TYPE_BYPASS);
		wr(smmu + SMMU_SMR(n), (1u << 31) | streams[k]);
		tell("pcie: 33:   now smr %#x s2cr %#x (type %u)\n", rd(smmu + SMMU_SMR(n)), rd(smmu + SMMU_S2CR(n)),
		    (rd(smmu + SMMU_S2CR(n)) >> 16) & 3);
	}
	tell("pcie: 33: 0x1c01 and 0x1c00 are bypassed; scr0 %#x, so nothing else changes behaviour\n",
	    rd(smmu + SMMU_SCR0));
}

/*
 * Step 34: why the core cannot be reached, asked at the moment it cannot.
 *
 * The symptom this answers is the one that keeps coming back: PARF_LTSSM says 0x11 (L0, the link
 * trained) while every read of the core's own window -- dm_core 0x60000000, and through it the DBI --
 * returns 0xffffffff. The wrapper and the core disagree, so one of them is lying about the other.
 *
 * 0xffffffff from a device read on this interconnect is an error response, not data: the slave did
 * not answer. The candidates, in the order they are cheap to exclude:
 *
 *   1. the core's clocks are gated (gcc 0x6b014 slv_axi, 0x6b01c mstr_axi, 0x6b024 cfg_ahb and
 *      0x6b02c pipe all have a halt bit: bit 2 of their branch register set means stopped);
 *   2. the core is held in reset (gcc 0x6b000 bit 0 asserted);
 *   3. the wrapper's own status says the slave space is out of range or the master is halted
 *      (PARF_STATUS, PARF_SLV_ADDR_SPACE, PARF_AXI_MSTR_HALT);
 *   4. the link really did fall over and PARF_LTSSM's low bits are stale.
 *
 * So: read all of those again, now, and read one dm_core word with the same breath. Whatever moved
 * since step 2 is the answer.
 */
static void
step_core_why(void)
{
	static const struct {
		uint32_t at;
		const char *what;
	} branches[] = {
		{ 0x6b014, "slv_axi_clk" },
		{ 0x6b01c, "mstr_axi_clk" },
		{ 0x6b024, "cfg_ahb_clk" },
		{ 0x6b028, "aux_clk" },
		{ 0x6b02c, "pipe_clk" },
		{ 0x6b010, "slv_q2a_axi_clk" },
	};

	if (gcc_window == 0 || parf == 0 || dbi == 0) {
		tell("pcie: 34: a window is missing\n");
		return;
	}
	tell("pcie: 34: the controller's clocks now (bit 2 set in a branch register means it is not running):\n");
	for (unsigned i = 0; i < sizeof(branches) / sizeof(branches[0]); i++) {
		uint32_t value = rd(gcc_window + branches[i].at);

		tell("pcie: 34:   %#x %s: %#x%s\n", branches[i].at, branches[i].what, value,
		    (value & 0x4) ? " -- STOPPED" : " (running)");
	}
	tell("pcie: 34: gcc 0x52008 reads %#x (six branches), 0x6b000 core reset %#x, 0x6c01c phy reset %#x\n",
	    rd(gcc_window + 0x52008), rd(gcc_window + 0x6b000), rd(gcc_window + 0x6c01c));
	tell("pcie: 34: parf ltssm %#x (state %#x, flush %u, enable %u), status %#x, slv_addr_space %#x, axi_mstr_halt %#x\n",
	    rd(parf + PARF_LTSSM), rd(parf + PARF_LTSSM) & 0x3f, (rd(parf + PARF_LTSSM) >> 11) & 1,
	    (rd(parf + PARF_LTSSM) >> 8) & 1, rd(parf + PARF_STATUS), rd(parf + PARF_SLV_ADDR_SPACE),
	    rd(parf + PARF_AXI_MSTR_HALT));
	tell("pcie: 34: about to read dm_core (%#lx) once -- id should be 0x10b17cb if the core answers\n", dbi);
	tell("pcie: 34: dm_core[0] %#x, elbi sys_stts %#x\n", rd(dbi), (elbi != 0) ? rd(elbi + ELBI_SYS_STTS) : 0xffffffffu);
	/*
	 * Two candidate repairs, each followed by the same read, because both registers were changed
	 * from what the firmware left: the slave space size was 16 MB and this file raised it to 64 MB
	 * (the device tree's value), and the system control word was 0x2000361c while this file writes
	 * 0x365e (the vendor driver's value). If the wrapper's decoder is what stopped answering, one
	 * of these brings it back, and the read says which.
	 */
	wr(parf + PARF_SLV_ADDR_SPACE, 0x01000000);
	tell("pcie: 34: slv_addr_space back to 16 MB: dm_core[0] %#x, elbi %#x\n", rd(dbi), rd(elbi + ELBI_SYS_STTS));
	wr(parf + PARF_SYS_CTRL, 0x2000361c);
	tell("pcie: 34: sys_ctrl back to 0x2000361c: dm_core[0] %#x, elbi %#x\n", rd(dbi), rd(elbi + ELBI_SYS_STTS));
	wr(parf + PARF_SYS_CTRL, 0x365e);
	tell("pcie: 34: sys_ctrl back to the vendor's 0x365e: dm_core[0] %#x\n", rd(dbi));
}

/*
 * Step 35: a translation context for the endpoint's streams, and a buffer behind it.
 *
 * Step 33 measured that this SMMU refuses stage-2 bypass: a bypass written into S2CR reads back as
 * fault. So the firmware download's buffer has to be reachable through real tables. That is what
 * hakios.camera.c already does for the camera's IFE streams (smmu_find/smmu_up, and this step is
 * its shape with a different stream), and its register values are used here unchanged:
 *
 *   CBAR   = later stage 1, S1 BPSHCFG NSH, MEMATTR WB     SMMU_CBAR_STAGE_1
 *   CBA2R  = 1 (TTBR0/1 written as 64-bit)                 SMMU_CBA2R(n)
 *   TCR2   = 1 | 1<<4 | 7<<15                              SMMU_TCR2   (IPS 36-bit)
 *   TCR    = 25 | 1<<8 | 1<<10 | 3<<12 | 1<<23             SMMU_TCR_39_BITS
 *   MAIR0  = 0xf404ff44                                    SMMU_MAIR0
 *   SCTLR  = 1|1<<1|1<<2|1<<5|1<<8|1<<12                   SMMU_SCTLR_ON
 *   leaf   = phys | 3 | 1<<6 | 2<<8 | 1<<10 | 1<<11        CAM_PTE
 *
 * The buffer is 8 MB of contiguous physical memory (amss20.bin is 4.3 MB; the second half is room
 * for the BHIE stage), and the IOVA is the physical address itself -- an identity mapping, which is
 * what makes the value the chip is given equal to the value this kernel handed out.
 *
 * Gated by pcie_smmu_map=1: it writes a context and switches it on, and that is the kind of thing
 * that should be asked for by name the first time.
 */
#define SMMU_SCR0               0x000
#define SMMU_SMR(n)             (0x800 + 4 * (n))
#define SMMU_S2CR(n)            (0xc00 + 4 * (n))
#define SMMU_CBAR(n)            (4 * (n))
#define SMMU_CBA2R(n)           (0x800 + 4 * (n))
#define SMMU_CB_SCTLR           0x00
#define SMMU_CB_TCR2            0x10
#define SMMU_CB_TTBR0           0x20
#define SMMU_CB_TTBR1           0x28
#define SMMU_CB_TCR             0x30
#define SMMU_CB_MAIR0           0x38
#define SMMU_CB_MAIR1           0x3c
#define SMMU_CB_FSR             0x58
#define SMMU_CB_FAR             0x60
#define SMMU_CB_FSYNR0          0x68
#define SMMU_CB_TLBIASID        0x610
#define SMMU_CB_TLBSYNC         0x7f0
#define SMMU_CB_TLBSTATUS       0x7f4
#define SMMU_CBAR_STAGE_1       (1u << 16 | 3u << 8 | 0xfu << 12)
#define SMMU_TCR_39_BITS        (25u | 1u << 8 | 1u << 10 | 3u << 12 | 1u << 23)
#define SMMU_TCR2               (1u | 1u << 4 | 7u << 15)
#define SMMU_MAIR0              0xf404ff44u
#define SMMU_SCTLR_ON           (1u | 1u << 1 | 1u << 2 | 1u << 5 | 1u << 8 | 1u << 12)
#define SMMU_FSR_ALL            0xc00001feu
#define SMMU_PTE                (3ull | 1ull << 6 | 2ull << 8 | 1ull << 10 | 1ull << 11)
#define WIFI_BUFFER_BYTES       0x00800000ull           /* (8 MB: amss20.bin is 4.3 MB) */
#define WIFI_IOVA               0x80000000ull           /* (2 GB: under 4 GB, so BHI can name it; not the phys) */

extern uint64_t kvtophys(vm_offset_t va);   /* (hakios.camera.c declares it the same way) */

static vm_offset_t wifi_buffer;                 /* (physical == virtual here: identity IOVA) */
static uint64_t wifi_iova;
static bool wifi_mapped;
static uint32_t wifi_smmu_context = ~0u;

static const uint8_t *
wifi_firmware(uint64_t *size)
{
	extern const uint8_t *hakios_image_file(const char *dir, const char *name, uint64_t *size, int *disk_out);

	return hakios_image_file("fw/qca6390", "amss20.bin", size, NULL);
}

static void
step_smmu_map(void)
{
	vm_offset_t smmu, gr1, ctx, tables;
	uint32_t idr0, idr1, page_size, pages, free_smr = ~0u, context = ~0u;
	bool used[256] = { false };
	uint64_t *root, *second, *third;

	smmu = ml_io_map(SMMU_PHYS, 0x1000);
	if (smmu == 0) {
		tell("pcie: 35: the apps smmu could not be mapped\n");
		return;
	}
	idr0 = rd(smmu + SMMU_IDR0);
	idr1 = rd(smmu + SMMU_IDR1);
	page_size = (idr1 >> 31) ? 0x10000 : 0x1000;
	pages = 2u << ((idr1 >> 28) & 7);
	if (2 * pages * page_size != SMMU_SIZE) {
		tell("pcie: 35: the smmu layout is not the known one\n");
		return;
	}
	gr1 = ml_io_map(SMMU_PHYS + page_size, page_size);
	if (gr1 == 0) {
		tell("pcie: 35: the smmu's context registers could not be mapped\n");
		return;
	}
	for (uint32_t n = 0; n < (idr0 & 0xff); n++) {
		uint32_t smr = rd(smmu + SMMU_SMR(n));

		if (smr >> 31) {
			used[rd(smmu + SMMU_S2CR(n)) & 0xff] = true;
		} else if (free_smr == ~0u) {
			free_smr = n;
		}
	}
	for (context = 1; context < (idr1 & 0xff); context++) {   /* (0 is the one step 33 may have taken) */
		if (!used[context]) {
			break;
		}
	}
	if (free_smr == ~0u || context >= (idr1 & 0xff)) {
		tell("pcie: 35: no free match register or context\n");
		return;
	}
	ctx = ml_io_map(SMMU_PHYS + (uint64_t)(pages + context) * page_size, page_size);
	if (ctx == 0) {
		tell("pcie: 35: context %u could not be mapped\n", context);
		return;
	}
	/* the buffer, and three levels of tables for it (2 MB leaves: one second-level entry each) */
	{
		vm_offset_t memory = 0;

		/*
		 * Below 4 GB on purpose: BHI hands the endpoint a 32-bit IMGADDR_LOW, so a buffer at
		 * 0x1_93058000 (which is what the first attempt got) is one the endpoint cannot be told
		 * about. 0xfff00000 is the highest page-aligned start under 4 GB.
		 */
		if (kmem_alloc_contig(kernel_map, &memory, (vm_size_t) WIFI_BUFFER_BYTES, PAGE_MASK, 0x0fffff, 0,    /* (max_pnum: the highest page under 4 GB) */
		    KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
			tell("pcie: 35: no %llu KB of contiguous memory for the firmware buffer\n", WIFI_BUFFER_BYTES >> 10);
			return;
		}
		wifi_buffer = memory;
	}
	if (kmem_alloc(kernel_map, &tables, 3 * 4096, KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		tell("pcie: 35: no memory for the page tables\n");
		return;
	}
	root = (uint64_t *) tables;
	second = (uint64_t *)(tables + 4096);
	third = (uint64_t *)(tables + 8192);
	{
		/*
		 * The leaves are physical (kvtophys of the buffer) and the IOVA is a fixed low address,
		 * not the physical one. Two reasons: a table built from the virtual address would describe
		 * memory the endpoint cannot reach while looking perfectly programmed (that was the first
		 * bug here), and BHI hands the endpoint a 32-bit IMGADDR_LOW, so the IOVA has to be under
		 * 4 GB while the buffer itself need not be -- kmem_alloc_contig ignored the page-number
		 * limit asked for and kept returning memory above 6 GB.
		 */
		uint64_t base = kvtophys(wifi_buffer);

		root[(WIFI_IOVA >> 30) & 0x1ff] = kvtophys((vm_offset_t) second) | 3;
		second[(WIFI_IOVA >> 21) & 0x1ff] = kvtophys((vm_offset_t) third) | 3;
		for (unsigned i = 0; i < (WIFI_BUFFER_BYTES >> 21); i++) {
			third[((WIFI_IOVA >> 21) & 0x1ff) + i] = (base + (i << 21)) | SMMU_PTE;
		}
	}
	for (vm_offset_t line = tables; line < tables + 3 * 4096; line += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (line));
	}
	__asm__ volatile ("dsb sy");

	tell("pcie: 35: buffer at va %#llx = phys %#llx (%llu KB), tables at %#llx, context %u, match register %u\n",
	    (unsigned long long) wifi_buffer, (unsigned long long) kvtophys(wifi_buffer), WIFI_BUFFER_BYTES >> 10,
	    (unsigned long long) kvtophys(tables), context, free_smr);
	wr(ctx + SMMU_CB_SCTLR, 0);
	wr(gr1 + SMMU_CBA2R(context), 1);
	wr(gr1 + SMMU_CBAR(context), SMMU_CBAR_STAGE_1);
	wr(ctx + SMMU_CB_TCR2, SMMU_TCR2);
	wr(ctx + SMMU_CB_TCR, SMMU_TCR_39_BITS);
	{
		uint64_t ttbr = kvtophys(tables) | ((uint64_t) context << 48);

		wr(ctx + SMMU_CB_TTBR0, (uint32_t) ttbr);
		wr(ctx + SMMU_CB_TTBR0 + 4, (uint32_t)(ttbr >> 32));
	}
	wr(ctx + SMMU_CB_TTBR1, 0);
	wr(ctx + SMMU_CB_TTBR1 + 4, 0);
	wr(ctx + SMMU_CB_MAIR0, SMMU_MAIR0);
	wr(ctx + SMMU_CB_MAIR1, 0);
	wr(ctx + SMMU_CB_FSR, SMMU_FSR_ALL);
	wr(ctx + SMMU_CB_TLBIASID, context);
	wr(ctx + SMMU_CB_TLBSYNC, 1);
	for (unsigned waited = 0; rd(ctx + SMMU_CB_TLBSTATUS) & 1; waited++) {
		if (waited > 1000) {
			tell("pcie: 35: the smmu does not finish forgetting\n");
			return;
		}
	}
	tell("pcie: 35: context %u: cbar %#x, tcr %#x, ttbr0 %#x%08x, sctlr %#x\n", context, rd(gr1 + SMMU_CBAR(context)),
	    rd(ctx + SMMU_CB_TCR), rd(ctx + SMMU_CB_TTBR0 + 4), rd(ctx + SMMU_CB_TTBR0), rd(ctx + SMMU_CB_SCTLR));
	wr(ctx + SMMU_CB_SCTLR, SMMU_SCTLR_ON);
	wifi_smmu_context = context;
	/* both of the endpoint's streams, through that context */
	for (unsigned k = 0; k < 2; k++) {
		uint32_t stream = k == 0 ? 0x1c01 : 0x1c00;
		unsigned smr = free_smr + k;

		wr(smmu + SMMU_S2CR(smr), context);
		wr(smmu + SMMU_SMR(smr), (1u << 31) | stream);
		tell("pcie: 35: smr %u: stream %#x -> context %u (s2cr %#x, smr %#x)\n", smr, stream, context,
		    rd(smmu + SMMU_S2CR(smr)), rd(smmu + SMMU_SMR(smr)));
	}
	tell("pcie: 35: sctlr now %#x (bit 0 on = the context is live)\n", rd(ctx + SMMU_CB_SCTLR));
	wifi_iova = WIFI_IOVA;
	wifi_mapped = true;
	tell("pcie: 35: the endpoint's iova is %#llx (%s), the buffer's physical is %#llx\n",
	    (unsigned long long) wifi_iova, wifi_iova < 0x100000000ull ? "under 4 GB, which BHI needs" : "ABOVE 4 GB",
	    (unsigned long long) kvtophys(wifi_buffer));
	{
		uint64_t size = 0;
		const uint8_t *image = wifi_firmware(&size);

		tell("pcie: 35: the firmware in the image: fw/qca6390/amss20.bin %s (%llu bytes)\n",
		    image != NULL ? "found" : "NOT FOUND", (unsigned long long) size);
	}
}

/*
 * Step 36: the first 512 KB of amss20.bin, through BHI.
 *
 * The sequence is mhi_boot.c's (BHI: mhi_boot.c:384-395, 597-612) and the research note's table:
 *
 *   1. BHI_EXECENV must read 0 -- the endpoint's own secondary boot loader has not run yet;
 *   2. BHI_STATUS <- 0, so the bits this waits on mean this transfer;
 *   3. the first min(512 KB, size) bytes of the image go into the DMA buffer, and the endpoint is
 *      told where it is (IMGADDR_LOW/HIGH) and how long (IMGSIZE) -- 32-bit, hence the buffer had
 *      to be allocated under 4 GB;
 *   4. BHI_IMGTXDB <- a non-zero sequence number is the doorbell;
 *   5. BHI_STATUS's bits[31:30] become 2 when the transfer is done and 3 when it failed, and a
 *      failure names itself in ERRCODE and the three debug words;
 *   6. BHI_EXECENV becoming 1 is the signal that the SBL is running -- and the signal to switch to
 *      BHIE for the rest of the file. That is the next step; this one stops at the doorbell.
 */
static void
windowed_write(vm_offset_t bar, uint32_t offset, uint32_t value)
{
	uint32_t window = (offset >> 19) & 0x3f;

	wr(bar + REMAP_BAR_CTRL, WINDOW_BIT | window);
	wr(bar + WINDOW_START + (offset & WINDOW_MASK), value);
}

static void
step_bhi_download(void)
{
	vm_offset_t bar;
	const uint8_t *image;
	uint64_t size = 0;
	uint32_t chunk, status, execenv, seq = 0x0051a7b3u;
	unsigned i;

	if (!wifi_mapped) {
		tell("pcie: 36: the dma buffer is not mapped yet (pcie_smmu_map=1 runs step 35 first)\n");
		return;
	}
	if (wifi_iova >= 0x100000000ull) {
		tell("pcie: 36: the buffer is at %#llx, above 4 GB; BHI_IMGADDR_LOW cannot name it\n",
		    (unsigned long long) wifi_iova);
		return;
	}
	image = wifi_firmware(&size);
	if (image == 0) {
		tell("pcie: 36: fw/qca6390/amss20.bin is not in the image\n");
		return;
	}
	if ((rd(parf + PARF_LTSSM) & 0x3f) != 0x11) {
		tell("pcie: 36: the link is not at L0 (state %#x); the endpoint will not take a download\n",
		    rd(parf + PARF_LTSSM) & 0x3f);
		return;
	}
	bar = endpoint_bar();
	if (bar == 0) {
		tell("pcie: 36: the bar could not be mapped\n");
		return;
	}
	execenv = windowed_read(bar, bhi_base + 0x28);
	status = windowed_read(bar, bhi_base + 0x2c);
	tell("pcie: 36: before: BHI exec env %u, status %#x (bits 31:30 = %u), image %llu bytes\n", execenv, status,
	    status >> 30, (unsigned long long) size);
	if (execenv != 0) {
		tell("pcie: 36: the endpoint is not in PBL any more (exec env %u); nothing is written\n", execenv);
		return;
	}

	chunk = (size > 0x80000u) ? 0x80000u : (uint32_t) size;
	__builtin_memcpy((void *) wifi_buffer, image, chunk);
	tell("pcie: 36: copied %#x bytes of the image into %#llx; telling the endpoint\n", chunk,
	    (unsigned long long) wifi_iova);
	windowed_write(bar, bhi_base + 0x2c, 0);                              /* (STATUS: clear the last verdict) */
	windowed_write(bar, bhi_base + 0x0c, 0);                              /* (IMGADDR_HIGH) */
	windowed_write(bar, bhi_base + 0x08, (uint32_t) wifi_iova);           /* (IMGADDR_LOW) */
	windowed_write(bar, bhi_base + 0x10, chunk);                          /* (IMGSIZE) */
	tell("pcie: 36: read back: addr %#x%08x, size %#x\n", windowed_read(bar, bhi_base + 0x0c),
	    windowed_read(bar, bhi_base + 0x08), windowed_read(bar, bhi_base + 0x10));
	windowed_write(bar, bhi_base + 0x18, seq);                            /* (the doorbell) */
	tell("pcie: 36: rang the doorbell with %#x (reads %#x); waiting for the transfer\n", seq,
	    windowed_read(bar, bhi_base + 0x18));

	for (i = 1; i <= 400; i++) {
		status = windowed_read(bar, bhi_base + 0x2c);
		if ((status >> 30) != 0) {
			break;
		}
		delay_for_interval(5000, NSEC_PER_USEC);
	}
	tell("pcie: 36: after %u polls: BHI status %#x -> %s\n", i, status,
	    (status >> 30) == 2 ? "SUCCESS" : (status >> 30) == 3 ? "FAILED" : "still transferring");
	if ((status >> 30) == 3 || status == 0xffffffffu) {
		tell("pcie: 36: error code %#x, debug %#x %#x %#x\n", windowed_read(bar, bhi_base + 0x30),
		    windowed_read(bar, bhi_base + 0x34), windowed_read(bar, bhi_base + 0x38), windowed_read(bar, bhi_base + 0x3c));
		/*
		 * 0xffffffff is an error response, not a verdict: the window stopped answering after the
		 * doorbell. Two readings settle whether that is the endpoint or the memory it was told to
		 * read from -- the SMMU's fault registers for our context, and whether the window comes
		 * back at all.
		 */
		if (wifi_smmu_context != ~0u) {
			uint32_t page_size = (rd(ml_io_map(SMMU_PHYS, 0x1000) + SMMU_IDR1) >> 31) ? 0x10000 : 0x1000;
			uint32_t pages = 2u << ((rd(ml_io_map(SMMU_PHYS, 0x1000) + SMMU_IDR1) >> 28) & 7);
			vm_offset_t ctx = ml_io_map(SMMU_PHYS + (uint64_t)(pages + wifi_smmu_context) * page_size, page_size);
			vm_offset_t smmu = ml_io_map(SMMU_PHYS, 0x1000);

			if (ctx != 0) {
				tell("pcie: 36: smmu context %u: fsr %#x, far %#x, fsynr0 %#x (a non-zero fsr is the endpoint's read faulting)\n",
				    wifi_smmu_context, rd(ctx + SMMU_CB_FSR), rd(ctx + SMMU_CB_FAR), rd(ctx + SMMU_CB_FSYNR0));
			}
			if (smmu != 0) {
				tell("pcie: 36: smmu gr0: scr0 %#x, our smr/s2cr entries are still there\n", rd(smmu + SMMU_SCR0));
			}
		}
		for (unsigned again = 0; again < 3; again++) {
			delay_for_interval(5000, NSEC_PER_USEC);
			tell("pcie: 36:   re-read %u: status %#x, exec env %#x, addr %#x%08x\n", again,
			    windowed_read(bar, bhi_base + 0x2c), windowed_read(bar, bhi_base + 0x28),
			    windowed_read(bar, bhi_base + 0x0c), windowed_read(bar, bhi_base + 0x08));
		}
		return;
	}
	for (i = 1; i <= 200; i++) {
		execenv = windowed_read(bar, bhi_base + 0x28);
		if (execenv != 0) {
			break;
		}
		delay_for_interval(5000, NSEC_PER_USEC);
	}
	tell("pcie: 36: exec env %u after %u polls (%s)\n", execenv, i,
	    execenv == 1 ? "the SBL is running: BHIE is next" : execenv == 0 ? "still PBL" : "something else is running");
}

/*
 * Step 37: after the doorbell, who is still there.
 *
 * Step 36 rang BHI's doorbell and the window the endpoint keeps its registers in started answering
 * 0xffffffff -- an error response, so the question is what actually happened to the endpoint. Three
 * readings answer it without writing anything:
 *
 *   1. the endpoint's own configuration space, which the root complex reaches through the iATU and
 *      not through the endpoint's BARs: 17cb:1101 still there means the device is alive and only
 *      the BAR aperture changed meaning (a secondary boot loader taking the chip over would do
 *      that), while 0xffffffff there means the device is gone;
 *   2. the link: PARF_LTSSM and the two port debug registers;
 *   3. the SMMU's fault registers, cleared first so that what is read afterwards is this step's and
 *      not step 36's leftovers.
 *
 * The BHIE aperture is read too (BHI_EOFF at BAR + 0x2c, in the un-windowed half): it is the
 * address the secondary boot loader's vector table lives at, and the next stage needs it.
 */
static void
step_after_download(void)
{
	vm_offset_t iatu, conf, bar;
	uint32_t bhie_off;

	if (parf == 0 || dbi == 0) {
		tell("pcie: 37: a window is missing\n");
		return;
	}
	iatu = ml_io_map(IATU_PHYS, IATU_SIZE);
	conf = ml_io_map(CONF_PHYS, 0x1000);
	if (iatu != 0 && conf != 0) {
		iatu_region0(iatu, 0x04, CONF_PHYS, CONF_PHYS + 0xfff, BDF(1, 0));
		tell("pcie: 37: the endpoint's configuration space now reads id %#x (17cb:1101 means it is alive), command %#x\n",
		    rd(conf + 0x00), rd(conf + 0x04));
	} else {
		tell("pcie: 37: the conf window could not be mapped\n");
	}
	tell("pcie: 37: the link: parf ltssm %#x (state %#x, flush %u), debug0 %#x, debug1 %#x, dbi link %#x\n",
	    rd(parf + PARF_LTSSM), rd(parf + PARF_LTSSM) & 0x3f, (rd(parf + PARF_LTSSM) >> 11) & 1,
	    rd(dbi + DBI_PORT_DEBUG0), rd(dbi + DBI_PORT_DEBUG1), rd(dbi + DBI_LINK_CTRL_STATUS));
	if (wifi_smmu_context != ~0u) {
		vm_offset_t smmu = ml_io_map(SMMU_PHYS, 0x1000);
		uint32_t idr1 = (smmu != 0) ? rd(smmu + SMMU_IDR1) : 0;
		uint32_t page_size = (idr1 >> 31) ? 0x10000 : 0x1000;
		uint32_t pages = 2u << ((idr1 >> 28) & 7);
		vm_offset_t ctx = ml_io_map(SMMU_PHYS + (uint64_t)(pages + wifi_smmu_context) * page_size, page_size);

		if (ctx != 0) {
			wr(ctx + SMMU_CB_FSR, SMMU_FSR_ALL);            /* (a clean slate for what follows) */
			delay_for_interval(10000, NSEC_PER_USEC);
			tell("pcie: 37: smmu context %u: fsr %#x, far %#x, fsynr0 %#x (cleared 10 ms ago)\n",
			    wifi_smmu_context, rd(ctx + SMMU_CB_FSR), rd(ctx + SMMU_CB_FAR), rd(ctx + SMMU_CB_FSYNR0));
		}
	}
	bar = endpoint_bar();
	if (bar != 0) {
		tell("pcie: 37: bar unwindowed: [0x00] %#x, [0x08] %#x, [0x1c] %#x, remap ctrl %#x\n", rd(bar),
		    rd(bar + 0x08), rd(bar + 0x1c), rd(bar + REMAP_BAR_CTRL));
		bhie_off = rd(bar + 0x2c);
		tell("pcie: 37: BHI_EOFF (bar + 0x2c) %#x -> the BHIE aperture would be bar + %#x%s\n", bhie_off, bhie_off,
		    (bhie_off == 0 || bhie_off > 0x100000u) ? " (not a plausible offset)" : "");
		tell("pcie: 37: window again: exec env %#x, status %#x, image size %#x, addr %#x\n",
		    windowed_read(bar, bhi_base + 0x28), windowed_read(bar, bhi_base + 0x2c), windowed_read(bar, bhi_base + 0x10),
		    windowed_read(bar, bhi_base + 0x08));
	}
}

/*
 * Step 38: BHI, tried three ways in one boot.
 *
 * Step 36 filled the registers correctly (they read back, and they were still there in step 37) and
 * rang the doorbell once, and BHI_STATUS's bits 31:30 stayed 0 while BHI_EXECENV stayed 0 -- the
 * secondary boot loader never started. The endpoint's own configuration space answered throughout,
 * so the device is alive: either the transfer was never issued, or it was refused.
 *
 * What the vendor's stack does around the doorbell that this file does not is small enough to try
 * all of it here, and each attempt ends with the same question -- did EXECENV leave 0:
 *
 *   A. the same request again with a different sequence number, because the doorbell's low bits are
 *      a sequence number and a repeated value may be taken as "already rung";
 *   B. the whole image (4.3 MB in an 8 MB buffer) instead of the first 512 KB, in case this part's
 *      BHI wants the file entire and the 512 KB split is cnss2's choice for its own reasons;
 *   C. the doorbell written as 0 and then as the sequence number, an edge rather than a level.
 *
 * BHI_INTVEC and BHI_NUMSEG are read first: an interrupt vector of 0 is what the vendor writes, and
 * a segment count that is not 1 would say the image is meant to arrive in pieces.
 */
static uint32_t
bhi_status_of(vm_offset_t bar)
{
	return windowed_read(bar, bhi_base + 0x2c);
}

static bool
bhi_wait(vm_offset_t bar, unsigned polls, const char *what)
{
	uint32_t status = 0;
	unsigned i;

	for (i = 1; i <= polls; i++) {
		status = bhi_status_of(bar);
		if ((status >> 30) != 0) {
			break;
		}
		delay_for_interval(5000, NSEC_PER_USEC);
	}
	tell("pcie: 38: %s: status %#x (bits 31:30 = %u) after %u polls, exec env %#x\n", what, status, status >> 30,
	    i, windowed_read(bar, bhi_base + 0x28));
	return (status >> 30) != 0;
}

/*
 * iatu_inbound: an inbound region, which is what the endpoint's own reads are translated by.
 *
 * Everything this file programmed in the iATU until now was an outbound region -- host to the bus,
 * which is how the processor reaches the endpoint. The endpoint reading host memory is the other
 * direction, and it needs an *inbound* region: base = the bus address the endpoint was told to read
 * (our IOVA), target = the physical address behind it. Without one, the read has nowhere to go.
 *
 * The region layout is the same as the outbound one (view 0 for outbound, 0x80000000 | n for
 * inbound, then ctrl1/ctrl2/base/limit/target at 0x200 a region), which is why this is six writes
 * more than iatu_region0 and not a new mechanism.
 */
static void
iatu_inbound(vm_offset_t iatu, unsigned region, uint32_t bus, uint32_t bus_end, uint32_t target)
{
	wr(iatu + 0x00, (1u << 31) | region);           /* (the viewport: bit 31 selects inbound) */
	wr(iatu + 0x04, 0);
	wr(iatu + 0x00, (1u << 31) | region);
	wr(iatu + 0x04, 0x00);                          /* (ctrl1: a memory region) */
	wr(iatu + 0x08, bus);                           /* (lower base: the bus address) */
	wr(iatu + 0x0c, 0);
	wr(iatu + 0x10, bus_end);
	wr(iatu + 0x14, target);                        /* (lower target: where host memory is) */
	wr(iatu + 0x18, 0);
	wr(iatu + 0x04, 1u << 31);                      /* (and enable it last) */
}

static void
step_bhi_retry(void)
{
	vm_offset_t bar;
	const uint8_t *image;
	uint64_t size = 0;
	uint32_t chunk, seq = 0x0051a7b4u;

	if (!wifi_mapped) {
		tell("pcie: 38: the dma buffer is not mapped (pcie_smmu_map=1)\n");
		return;
	}
	image = wifi_firmware(&size);
	if (image == 0) {
		tell("pcie: 38: fw/qca6390/amss20.bin is not in the image\n");
		return;
	}
	bar = endpoint_bar();
	if (bar == 0) {
		tell("pcie: 38: the bar could not be mapped\n");
		return;
	}
	{
		/*
		 * Read the iatu window before writing anything else into it.
		 *
		 * An attempt to program an inbound region wrote 0x80000000 into offsets 0 and 4, which in
		 * this window are outbound region 0's ctrl1 and ctrl2 -- there is no separate viewport
		 * register here, regions sit at 0x200 * n and region 0 starts at 0. That write would have
		 * broken the configuration space path, so this step only looks: the first 0x200 bytes (the
		 * outbound region the config reads use) and the blocks that are not ours, to find where an
		 * inbound region is meant to live.
		 */
		vm_offset_t iatu = ml_io_map(IATU_PHYS, IATU_SIZE);

		if (iatu != 0) {
			tell("pcie: 38: iatu 0x000 (outbound region 0): %#x %#x %#x %#x %#x %#x %#x\n", rd(iatu + 0x00),
			    rd(iatu + 0x04), rd(iatu + 0x08), rd(iatu + 0x0c), rd(iatu + 0x10), rd(iatu + 0x14),
			    rd(iatu + 0x18));
			tell("pcie: 38: iatu 0x200 (outbound region 1): %#x %#x %#x %#x %#x %#x %#x\n", rd(iatu + 0x200),
			    rd(iatu + 0x204), rd(iatu + 0x208), rd(iatu + 0x20c), rd(iatu + 0x210), rd(iatu + 0x214),
			    rd(iatu + 0x218));
			tell("pcie: 38: iatu 0x800..0x81c: %#x %#x %#x %#x %#x %#x %#x %#x\n", rd(iatu + 0x800),
			    rd(iatu + 0x804), rd(iatu + 0x808), rd(iatu + 0x80c), rd(iatu + 0x810), rd(iatu + 0x814),
			    rd(iatu + 0x818), rd(iatu + 0x81c));
			tell("pcie: 38: iatu 0x900..0x91c (the designware viewport block): %#x %#x %#x %#x %#x %#x %#x %#x\n",
			    rd(iatu + 0x900), rd(iatu + 0x904), rd(iatu + 0x908), rd(iatu + 0x90c), rd(iatu + 0x910),
			    rd(iatu + 0x914), rd(iatu + 0x918), rd(iatu + 0x91c));
		} else {
			tell("pcie: 38: the iatu could not be mapped\n");
		}
	}
	tell("pcie: 38: BHI_INTVEC %#x, BHI_EOFF %#x, NUMSEG %#x, SERIALNU %#x, ANTIROLLVER %#x\n",
	    windowed_read(bar, bhi_base + 0x20), rd(bar + 0x2c), windowed_read(bar, bhi_base + 0x48), windowed_read(bar, bhi_base + 0x40),
	    windowed_read(bar, bhi_base + 0x44));
	chunk = (size > 0x80000u) ? 0x80000u : (uint32_t) size;
	if (size > WIFI_BUFFER_BYTES) {
		tell("pcie: 38: the image (%llu) does not fit the buffer (%llu); only the first part is used\n",
		    (unsigned long long) size, (unsigned long long) WIFI_BUFFER_BYTES);
	}
	__builtin_memcpy((void *) wifi_buffer, image, (size_t)((size > WIFI_BUFFER_BYTES) ? WIFI_BUFFER_BYTES : size));

	/* A: the same request, a different sequence number */
	windowed_write(bar, bhi_base + 0x2c, 0);
	windowed_write(bar, bhi_base + 0x0c, 0);
	windowed_write(bar, bhi_base + 0x08, (uint32_t) wifi_iova);
	windowed_write(bar, bhi_base + 0x10, chunk);
	windowed_write(bar, bhi_base + 0x18, seq);
	(void) bhi_wait(bar, 1000, "A: 512 KB, a new sequence number");
	if (windowed_read(bar, bhi_base + 0x28) != 0) {
		return;
	}

	/* B: the whole image */
	windowed_write(bar, bhi_base + 0x2c, 0);
	windowed_write(bar, bhi_base + 0x10, (uint32_t) size);
	windowed_write(bar, bhi_base + 0x18, seq + 1);
	(void) bhi_wait(bar, 200, "B: the whole image");
	if (windowed_read(bar, bhi_base + 0x28) != 0) {
		return;
	}

	/* C: the doorbell as an edge */
	windowed_write(bar, bhi_base + 0x2c, 0);
	windowed_write(bar, bhi_base + 0x10, chunk);
	windowed_write(bar, bhi_base + 0x18, 0);
	delay_for_interval(1000, NSEC_PER_USEC);
	windowed_write(bar, bhi_base + 0x18, seq + 2);
	(void) bhi_wait(bar, 200, "C: the doorbell as an edge");
	tell("pcie: 38: done: status %#x, exec env %#x, error code %#x, ERRBG %#x %#x %#x\n", bhi_status_of(bar),
	    windowed_read(bar, bhi_base + 0x28), windowed_read(bar, bhi_base + 0x30), windowed_read(bar, bhi_base + 0x34),
	    windowed_read(bar, bhi_base + 0x38), windowed_read(bar, bhi_base + 0x3c));
}

/*
 * Step 39: the map of the iatu window, and what the endpoint's BDF is turned into.
 *
 * Step 38 showed that the endpoint tried the BHI transfer and failed with the descriptor echoed in
 * its error debug words, so the read of host memory is what fails. Two candidates remain: the iATU's
 * inbound translation (which is what an endpoint's own reads go through on this IP) and the stream
 * id its requests are tagged with. Neither can be guessed, so this step reads both:
 *
 *   - the whole first page of the iATU window, sixteen words a row, to find where an inbound region
 *     lives (only region 0 at 0x000 and region 1 at 0x200 have been identified so far);
 *   - PARF_BDF_TO_SID_CFG at bar + 0x1e0ac00 (cnss2/reg.h:88), the endpoint-side table that turns a
 *     BDF into a stream id, so that the SMMU's two match registers can be checked against what the
 *     endpoint will actually send.
 *
 * Read only.
 */
#define EP_BDF_TO_SID   0x1e0ac00u      /* (window 0x01, in-window 0x2c00) */

static void
step_iatu_map(void)
{
	vm_offset_t iatu, bar;

	iatu = ml_io_map(IATU_PHYS, IATU_SIZE);
	if (iatu == 0) {
		tell("pcie: 39: the iatu could not be mapped\n");
		return;
	}
	for (uint32_t at = 0; at < 0x400; at += 32) {
		tell("pcie: 39: iatu %#05x: %#x %#x %#x %#x %#x %#x %#x %#x | %#x %#x %#x %#x %#x %#x %#x %#x\n", at,
		    rd(iatu + at + 0x00), rd(iatu + at + 0x04), rd(iatu + at + 0x08), rd(iatu + at + 0x0c),
		    rd(iatu + at + 0x10), rd(iatu + at + 0x14), rd(iatu + at + 0x18), rd(iatu + at + 0x1c),
		    rd(iatu + at + 0x20), rd(iatu + at + 0x24), rd(iatu + at + 0x28), rd(iatu + at + 0x2c),
		    rd(iatu + at + 0x30), rd(iatu + at + 0x34), rd(iatu + at + 0x38), rd(iatu + at + 0x3c));
	}
	for (uint32_t at = 0x800; at < 0x1000; at += 0x80) {
		uint32_t window = rd(iatu + at);

		if (window != 0 && window != 0xffffffffu) {
			tell("pcie: 39: iatu %#05x is not empty: %#x %#x %#x %#x\n", at, window, rd(iatu + at + 4),
			    rd(iatu + at + 8), rd(iatu + at + 12));
		}
	}
	bar = endpoint_bar();
	if (bar != 0) {
		tell("pcie: 39: endpoint BDF_TO_SID_CFG (bar + %#x) = %#x %#x %#x %#x\n", EP_BDF_TO_SID,
		    windowed_read(bar, EP_BDF_TO_SID), windowed_read(bar, EP_BDF_TO_SID + 4),
		    windowed_read(bar, EP_BDF_TO_SID + 8), windowed_read(bar, EP_BDF_TO_SID + 12));
	}
}

/*
 * Step 40: the root complex's BDF-to-SID table -- the thing that tells the SMMU who is knocking.
 *
 * An endpoint's requests reach the SMMU tagged with a stream id, and on this controller that id does
 * not come from the link: it comes from a 256-entry table in the wrapper's own registers,
 *
 *	parf + 0x2000, one word an entry: BDF[31:16] | SID[15:8] | NEXT[7:0]
 *
 * indexed by the CRC-8 (MSB first, polynomial 0x07, initial value 0) of the BDF as two big-endian
 * bytes (pci-msm.c:4854-4912, msm_pcie_config_sid; the polynomial is BIT(2)|BIT(1)|BIT(0) at :167).
 * Nothing in this file has ever written it, so the table is whatever the boot loader left -- and a
 * request whose BDF is not in it is tagged with a stream the SMMU has no match register for, which
 * is the one thing that would make the endpoint's read of host memory fail while every register
 * read and write from the processor still works.
 *
 * So: entry 0x15 gets BDF 0x0100 (bus 1, device 0, function 0) and SID 0x1c01, which is the stream
 * the SMMU context from step 35 already covers. Then BHI is tried again, unchanged.
 */
#define PARF_BDF_TO_SID_TABLE   0x2000u
#define SID_TABLE_ENTRIES       256u

static uint8_t
crc8_of(const uint8_t *data, unsigned length)
{
	uint8_t crc = 0;

	for (unsigned i = 0; i < length; i++) {
		crc ^= data[i];
		for (unsigned bit = 0; bit < 8; bit++) {
			crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x07) : (uint8_t)(crc << 1);
		}
	}
	return crc;
}

static void
step_bdf_to_sid(void)
{
	uint32_t bdf = 0x0100, sid = 0x1c01, entry, old;
	uint8_t bytes[2], hash;

	if (parf == 0) {
		tell("pcie: 40: no parf window\n");
		return;
	}
	bytes[0] = (uint8_t)(bdf >> 8);
	bytes[1] = (uint8_t)(bdf & 0xff);
	hash = crc8_of(bytes, 2);
	entry = (bdf << 16) | (sid << 8);
	old = rd(parf + PARF_BDF_TO_SID_TABLE + 4u * hash);
	tell("pcie: 40: bdf %#x -> hash %u; the entry there was %#x, writing %#x (bdf %#x, sid %#x)\n", bdf, hash, old,
	    entry, bdf, sid);
	/*
	 * No write. The first attempt did write the entry and then put back what it found -- but what
	 * it found was its own earlier write, because the table lives in the wrapper and survives the
	 * run. The bucket this BDF hashes to also belongs to another BDF (it chained to one), so
	 * overwriting it breaks that BDF's lookup. The value the boot loader had there, read before any
	 * of this, was 0x03801e78 (bdf 0x0380, sid 0x1e, next 0x78); it is restored here and the step
	 * stays read-only afterwards.
	 */
	if (old == entry) {
		wr(parf + PARF_BDF_TO_SID_TABLE + 4u * hash, 0x03801e78u);
		tell("pcie: 40: entry %u still held this experiment's value; the boot loader's 0x03801e78 is put back (%#x)\n",
		    hash, rd(parf + PARF_BDF_TO_SID_TABLE + 4u * hash));
	} else {
		tell("pcie: 40: nothing is written; entry %u is %#x\n", hash, old);
	}
	(void) entry;
	{
		unsigned used = 0;

		for (unsigned i = 0; i < SID_TABLE_ENTRIES; i++) {
			used += rd(parf + PARF_BDF_TO_SID_TABLE + 4u * i) != 0;
		}
		tell("pcie: 40: the table now has %u of %u entries in use\n", used, SID_TABLE_ENTRIES);
	}
	tell("pcie: 40: trying the download again with the stream id in place\n");
	step_bhi_retry();
}

/*
 * Step 41: read the whole BDF-to-SID table, and give the SMMU the stream it actually names.
 *
 * Step 40 found the table full (256 of 256 entries, which the boot loader wrote) and the entry at
 * our BDF's hash belonging to a different BDF -- and, in it, a SID of 0x1e. That number matters: the
 * entry is BDF[31:16] | SID[15:8] | NEXT[7:0], so the field is eight bits wide and the stream ids
 * this controller tags requests with are below 0x100. 0x1c01, which the device tree's iommu-map and
 * step 35's match registers use, cannot even be written into that field -- which is exactly why a
 * context keyed on 0x1c01 never sees the endpoint's traffic.
 *
 * So this step dumps the table (following the NEXT chain from each hash), finds the entry whose BDF
 * is ours, and adds a match register for the SID it names, through the context step 35 built. Then
 * BHI is tried again.
 */
static void
sid_match_add(vm_offset_t smmu, uint32_t stream, uint32_t context)
{
	uint32_t idr0 = rd(smmu + 0x020);
	unsigned free_smr = ~0u;

	for (uint32_t n = 0; n < (idr0 & 0xff); n++) {
		if ((rd(smmu + SMMU_SMR(n)) >> 31) == 0) {
			free_smr = n;
			break;
		}
	}
	if (free_smr == ~0u) {
		tell("pcie: 41: no free match register for stream %#x\n", stream);
		return;
	}
	wr(smmu + SMMU_S2CR(free_smr), context);
	wr(smmu + SMMU_SMR(free_smr), (1u << 31) | stream);
	tell("pcie: 41: match register %u: stream %#x -> context %u (smr %#x, s2cr %#x)\n", free_smr, stream, context,
	    rd(smmu + SMMU_SMR(free_smr)), rd(smmu + SMMU_S2CR(free_smr)));
}

static void
step_sid_table(void)
{
	vm_offset_t smmu;
	unsigned found = 0;

	if (parf == 0 || wifi_smmu_context == ~0u) {
		tell("pcie: 41: parf or the smmu context is missing (pcie_smmu_map=1 runs step 35)\n");
		return;
	}
	smmu = ml_io_map(SMMU_PHYS, 0x1000);
	if (smmu == 0) {
		tell("pcie: 41: the smmu could not be mapped\n");
		return;
	}
	tell("pcie: 41: the bdf-to-sid table, entry by entry (bdf | sid | next):\n");
	for (unsigned i = 0; i < SID_TABLE_ENTRIES; i++) {
		uint32_t entry = rd(parf + PARF_BDF_TO_SID_TABLE + 4u * i);
		uint32_t bdf, sid, next;

		if (entry == 0) {
			continue;
		}
		bdf = entry >> 16;
		sid = (entry >> 8) & 0xff;
		next = entry & 0xff;
		if (i < 32 || bdf == 0x0100 || bdf == 0x0000) {
			tell("pcie: 41:   [%3u] bdf %#06x -> sid %#04x, next %u%s\n", i, bdf, sid, next,
			    (bdf == 0x0100) ? "   <-- ours" : (bdf == 0x0000) ? "   <-- bus 0" : "");
		}
		if (bdf == 0x0100) {
			found++;
			tell("pcie: 41: the table names stream %#x for our bdf; giving the smmu that stream\n", sid);
			sid_match_add(smmu, sid, wifi_smmu_context);
		}
	}
	if (found == 0) {
		tell("pcie: 41: the table has no entry for %#x; trying the low streams 0x1e and 0x1f anyway\n", 0x0100);
		sid_match_add(smmu, 0x1e, wifi_smmu_context);
		sid_match_add(smmu, 0x1f, wifi_smmu_context);
	}
	tell("pcie: 41: trying the download with those streams in place\n");
	step_bhi_retry();
}

/*
 * Step 42: which stream the endpoint's requests actually carry -- read, do not guess.
 *
 * There was a version of this step that pointed all 128 stream slots at one context, to find out by
 * brute force. It blanked the screen: that context's tables map 8 MB at 0x80000000 and nothing else,
 * so every other master in the SoC -- the display's DMA among them -- lost its translation. The
 * lesson is worth the step number it cost: the SMMU is shared, and an experiment on it must address
 * exactly one stream.
 *
 * So this reads instead. Two registers say what the hardware did with a request it could not match:
 *
 *   SMMU_GFSR   (0x48)  the global fault status
 *   SMMU_GFSYNR1(0x8c)? -- the per-fault syndrome words, which on this part carry the stream
 *   SMMU_S2CR   the entry the request was matched to, if any
 *
 * and the BDF-to-SID table is walked properly: our BDF's bucket was taken by another BDF with a
 * NEXT chain behind it, and the entry for ours -- if the boot loader wrote one, and it should have,
 * because Android enumerates the same endpoint at the same BDF -- is somewhere along that chain.
 */
#define SMMU_GFSR       0x048
#define SMMU_GFSYNR0    0x088
#define SMMU_GFSYNR1    0x08c

static void
step_stream_read(void)
{
	vm_offset_t smmu;
	unsigned index, hops = 0;

	if (parf == 0) {
		tell("pcie: 42: no parf window\n");
		return;
	}
	smmu = ml_io_map(SMMU_PHYS, 0x1000);
	if (smmu != 0) {
		tell("pcie: 42: smmu global: gfsr %#x, gfsynr0 %#x, gfsynr1 %#x, scr0 %#x\n", rd(smmu + SMMU_GFSR),
		    rd(smmu + SMMU_GFSYNR0), rd(smmu + SMMU_GFSYNR1), rd(smmu + SMMU_SCR0));
	}
	index = crc8_of((const uint8_t[]){ 0x01, 0x00 }, 2);
	tell("pcie: 42: walking the bdf-to-sid chain from hash %u looking for bdf 0x0100:\n", index);
	while (hops++ < 32) {
		uint32_t entry = rd(parf + PARF_BDF_TO_SID_TABLE + 4u * index);
		uint32_t bdf = entry >> 16, sid = (entry >> 8) & 0xff, next = entry & 0xff;

		if (entry == 0) {
			tell("pcie: 42:   [%3u] empty; the chain ends without our bdf\n", index);
			return;
		}
		tell("pcie: 42:   [%3u] bdf %#06x sid %#04x next %u%s\n", index, bdf, sid, next,
		    bdf == 0x0100 ? "   <-- ours" : "");
		if (bdf == 0x0100) {
			tell("pcie: 42: the endpoint's stream is %#x, and one match register is enough for it\n", sid);
			if (wifi_smmu_context != ~0u && smmu != 0) {
				sid_match_add(smmu, sid, wifi_smmu_context);
				tell("pcie: 42: trying the download with only that stream mapped\n");
				step_bhi_retry();
			}
			return;
		}
		if (next == 0) {
			break;
		}
		index = next;
	}
	tell("pcie: 42: the chain has no entry for bdf 0x0100 after %u hops; nothing is written\n", hops);
}

/*
 * Step 43: the inbound region -- found at last, in Qualcomm's own register map.
 *
 * The last question was where the address translator's inbound registers are. They are not in the
 * DesignWare viewport at DBI + 0x900 (that address reads 0xffffffff on this part), and they are not
 * missing either: the SM8250 boot firmware's hardware map says it outright
 * (BOOT.XF.3.3, QcomPkg/SocPkg/Library/PcieConfigLib/PcieHwioRegs.h):
 *
 *   HWIO_PCIE_GEN3X1_IATU_REGION_CTRL_1_OFF_OUTBOUND_i_ADDR(i)  i * 0x200 + 0x000
 *   HWIO_PCIE_GEN3X1_IATU_REGION_CTRL_1_OFF_INBOUND_i_ADDR(i)   i * 0x200 + 0x100
 *   ... CTRL_2 +0x04, LWR_BASE +0x08, UPPER_BASE +0x0c, LIMIT +0x10,
 *       LWR_TARGET +0x14, UPPER_TARGET +0x18 (and 0x100 added for the inbound copy)
 *
 * So each outbound region is followed, 0x100 later, by the inbound region of the same index -- which
 * is why the dump of 0x400..0x1100 showed a lone 0xfff at +0x10 of every 0x100 block: those were the
 * inbound LIMIT registers at their reset value, and nothing was ever written to them.
 *
 * One region is enough: the bus range the endpoint reads (our IOVA, 8 MB) becomes the physical
 * address of the buffer behind it. Then BHI is tried again.
 */
#define IATU_IN_CTRL1   0x100
#define IATU_IN_CTRL2   0x104
#define IATU_IN_LBASE   0x108
#define IATU_IN_UBASE   0x10c
#define IATU_IN_LIMIT   0x110
#define IATU_IN_LTARGET 0x114
#define IATU_IN_UTARGET 0x118

static void
step_iatu_inbound(void)
{
	vm_offset_t iatu;
	uint64_t phys;

	if (!wifi_mapped) {
		tell("pcie: 43: the dma buffer is not mapped (pcie_smmu_map=1 runs step 35)\n");
		return;
	}
	iatu = ml_io_map(IATU_PHYS, 0x1000);
	if (iatu == 0) {
		tell("pcie: 43: the iatu could not be mapped\n");
		return;
	}
	phys = kvtophys(wifi_buffer);
	tell("pcie: 43: inbound region 0 before: ctrl1 %#x ctrl2 %#x base %#x limit %#x target %#x\n",
	    rd(iatu + IATU_IN_CTRL1), rd(iatu + IATU_IN_CTRL2), rd(iatu + IATU_IN_LBASE), rd(iatu + IATU_IN_LIMIT),
	    rd(iatu + IATU_IN_LTARGET));
	wr(iatu + IATU_IN_CTRL2, 0);                                    /* (off while it is reprogrammed) */
	wr(iatu + IATU_IN_CTRL1, 0);                                    /* (a memory request) */
	wr(iatu + IATU_IN_LBASE, (uint32_t) wifi_iova);
	wr(iatu + IATU_IN_UBASE, 0);
	wr(iatu + IATU_IN_LIMIT, (uint32_t)(wifi_iova + WIFI_BUFFER_BYTES - 1));
	wr(iatu + IATU_IN_LTARGET, (uint32_t) phys);
	wr(iatu + IATU_IN_UTARGET, (uint32_t)(phys >> 32));
	wr(iatu + IATU_IN_CTRL2, 1u << 31);                             /* (and on) */
	tell("pcie: 43: inbound region 0 after: ctrl1 %#x ctrl2 %#x base %#x limit %#x target %#x%08x\n",
	    rd(iatu + IATU_IN_CTRL1), rd(iatu + IATU_IN_CTRL2), rd(iatu + IATU_IN_LBASE), rd(iatu + IATU_IN_LIMIT),
	    rd(iatu + IATU_IN_UTARGET), rd(iatu + IATU_IN_LTARGET));
	tell("pcie: 43: %#llx..%#llx on the bus is now %#llx in host memory; trying the download\n",
	    (unsigned long long) wifi_iova, (unsigned long long)(wifi_iova + WIFI_BUFFER_BYTES - 1),
	    (unsigned long long) phys);
	step_bhi_retry();
}


/*
 * Step 44: the BHI base, from the register that holds it -- and INTVEC, which nothing here wrote.
 *
 * OpenBSD's qwx(4) is the one implementation of this chip's bring-up outside Linux, and it does two
 * things this file did not (sys/dev/pci/if_qwx_pci.c):
 *
 *	psc->bhi_off = qwx_pci_read(sc, MHI_BHI_OFFSET);   (BAR + 0x28, not a constant)
 *	qwx_pci_write(sc, psc->bhi_off + MHI_BHI_INTVEC, 0);   (before every download)
 *
 * The first says where the BHI block is, instead of assuming 0x1e0e200 (which came from cnss2's
 * QCA6390_PCIE_BHI_EXECENV_REG minus the EXECENV offset, and is only as good as that arithmetic).
 * The second is a write that was never made here -- and the register it writes, bhi + 0x20, is the
 * one this file read as 0xffffffff every time. If the base was wrong, that reading is explained; if
 * it was right, the write is still the one the working driver makes before it rings the doorbell.
 *
 * Both, then the download again.
 */
#define MHI_BHI_OFFSET_REG      0x28u   /* (BAR + 0x28 holds the BHI block's offset) */
#define MHI_BHIE_OFFSET_REG     0x2cu   /* (and BAR + 0x2c the BHIe one) */

static void
step_bhi_base(void)
{
	vm_offset_t bar;
	uint32_t base, bhie;

	bar = endpoint_bar();
	if (bar == 0) {
		tell("pcie: 44: the bar could not be mapped\n");
		return;
	}
	base = rd(bar + MHI_BHI_OFFSET_REG);
	bhie = rd(bar + MHI_BHIE_OFFSET_REG);
	tell("pcie: 44: bar + 0x28 says the BHI block is at %#x; bar + 0x2c says BHIe is at %#x (was assumed %#x)\n", base,
	    bhie, bhi_base);
	/*
	 * The value at bar + 0x28 is not the offset this file needs: 0x100 was read there, and the whole
	 * block at 0x100 answers 0xffffffff, while 0x1e0e200 answers coherently (version 1.2, exec env
	 * 0, numseg 1, serial 0xc4e10d95). So the assumed base stays, and the register is printed only.
	 */
	tell("pcie: 44: using base %#x: version %u.%u, exec env %#x, status %#x, intvec %#x, errcode %#x\n", bhi_base,
	    windowed_read(bar, bhi_base + 0x04), windowed_read(bar, bhi_base + 0x00), windowed_read(bar, bhi_base + 0x28),
	    windowed_read(bar, bhi_base + 0x2c), windowed_read(bar, bhi_base + 0x20), windowed_read(bar, bhi_base + 0x30));
	tell("pcie: 44: writing INTVEC (base + 0x20) <- 0, as qwx(4) does before every download\n");
	windowed_write(bar, bhi_base + 0x20, 0);
	tell("pcie: 44: intvec now %#x; trying the download\n", windowed_read(bar, bhi_base + 0x20));
	step_bhi_retry();
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
	{
		/*
		 * Steps 24 to 26 keep the old PERST polarity and only make sense as evidence; they are
		 * opt-in so that a step 27 run is not spent on them (each of them loops for seconds).
		 */
		uint32_t legacy = 0;

		PE_parse_boot_argn("pcie_legacy", &legacy, sizeof(legacy));
		if (legacy != 0) {
			if (steps >= 24) {
				step_link_final();
			}
			if (steps >= 25) {
				step_flush_proper();
			}
			if (steps >= 26) {
				step_endpoint_attempts();
			}
		}
	}
	if (steps >= 27) {
		step_endpoint_polarity();
	}
	if (steps >= 28) {
		step_endpoint_config();
	}
	if (steps >= 29) {
		step_endpoint_bar();
	}
	if (steps >= 30) {
		step_bhi();
	}
	if (steps >= 31) {
		step_capabilities();
	}
	if (steps >= 32) {
		step_smmu_look();
	}
	if (steps >= 33) {
		uint32_t smmu = 0;

		PE_parse_boot_argn("pcie_smmu", &smmu, sizeof(smmu));
		if (smmu != 0) {
			step_smmu_bypass();
		}
	}
	if (steps >= 34) {
		step_core_why();
	}
	if (steps >= 35) {
		uint32_t map = 0;

		PE_parse_boot_argn("pcie_smmu_map", &map, sizeof(map));
		if (map != 0) {
			step_smmu_map();
		}
	}
	if (steps >= 36) {
		uint32_t download = 0;

		PE_parse_boot_argn("pcie_bhi", &download, sizeof(download));
		if (download != 0) {
			step_bhi_download();
		}
	}
	if (steps >= 37) {
		step_after_download();
	}
	if (steps >= 38) {
		step_bhi_retry();
	}
	if (steps >= 39) {
		step_iatu_map();
	}
	if (steps >= 40) {
		step_bdf_to_sid();
	}
	if (steps >= 41) {
		step_sid_table();
	}
	if (steps >= 42) {
		step_stream_read();
	}
	if (steps >= 43) {
		step_iatu_inbound();
	}
	if (steps >= 44) {
		step_bhi_base();
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
