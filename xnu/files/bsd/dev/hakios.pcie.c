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

/* (hakios.display.c declares these the same way: the header that has them is not one the drivers take) */
extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);

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

static vm_offset_t parf, phy, dbi, elbi;
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
	printf("pcie: about to map the controller's windows\n");
	parf = ml_io_map(PCIE_PARF_PHYS, PCIE_PARF_SIZE);
	if (parf == 0) {
		printf("pcie: parf (%#lx) could not be mapped\n", (unsigned long) PCIE_PARF_PHYS);
		return false;
	}
	printf("pcie: parf at %#lx\n", parf);
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
		printf("pcie: 1: GCC could not be mapped\n");
		return;
	}
	gdscr = rd(gcc + 0x6b004);
	gdsc_was = gdscr;
	printf("pcie: 1: pcie_0_gdsc %#x -> %s (power on %u, sw collapse %u, hw ctrl %u)\n", gdscr,
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
		printf("pcie: 1: after writing %#x it reads %#x -> %s\n", now, gdscr,
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
		printf("pcie: 2: GCC could not be mapped\n");
		return;
	}
	vote = rd(gcc_window + GCC_PCIE_0_VOTE);
	printf("pcie: 2: gcc +%#x (the six PCIe 0 branches) reads %#x; setting the low six bits\n",
	    GCC_PCIE_0_VOTE, vote);
	wr(gcc_window + GCC_PCIE_0_VOTE, vote | 0x3f);
	printf("pcie: 2: gcc +%#x now reads %#x\n", GCC_PCIE_0_VOTE, rd(gcc_window + GCC_PCIE_0_VOTE));
	for (unsigned i = 0; i < sizeof(pcie_0_branches) / sizeof(pcie_0_branches[0]); i++) {
		uint32_t halt = rd(gcc_window + pcie_0_branches[i].at);

		printf("pcie: 2:   %-16s halt register %#x: %#x -> %s\n", pcie_0_branches[i].name,
		    pcie_0_branches[i].at, halt, (halt >> 2) & 1 ? "halted" : "running");
	}
	for (unsigned i = 0; i < sizeof(pcie_clock_gates) / sizeof(pcie_clock_gates[0]); i++) {
		uint32_t was = rd(gcc_window + pcie_clock_gates[i].at);

		wr(gcc_window + pcie_clock_gates[i].at, was | (1u << pcie_clock_gates[i].bit));
		printf("pcie: 2:   %-22s %#x: %#x -> %#x\n", pcie_clock_gates[i].name,
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
		printf("pcie: 4:   %s %#x: %#x -> %#x (%s)\n", pcie_resets[i].name, pcie_resets[i].at,
		    was, rd(gcc_window + pcie_resets[i].at), (was & 1) ? "was asserted" : "was already out");
	}
	printf("pcie: 4:   link_down %#x, nocsr_com_phy %#x\n", rd(gcc_window + 0x6c014),
	    rd(gcc_window + 0x6c020));
}

static void
step_read(void)
{
	uint32_t v;

	printf("pcie: 4: reading parf sys_ctrl (%#x) -- if nothing follows this line, that read hung\n", PARF_SYS_CTRL);
	v = rd(parf + PARF_SYS_CTRL);
	printf("pcie: 4: parf sys_ctrl %#x\n", v);
	printf("pcie: 4: reading parf pm_ctrl (%#x)\n", PARF_PM_CTRL);
	printf("pcie: 4: parf pm_ctrl %#x, pm_stts %#x\n", rd(parf + PARF_PM_CTRL), rd(parf + PARF_PM_STTS));
	printf("pcie: 4: reading parf ltssm (%#x)\n", PARF_LTSSM);
	v = rd(parf + PARF_LTSSM);
	printf("pcie: 4: parf ltssm %#x -> enabled %u, state %#x\n", v, (v >> 8) & 1, v & 0x3f);
	printf("pcie: 4: phy_ctrl %#x, mhi_clock_reset %#x, axi_mstr_halt %#x, status %#x\n",
	    rd(parf + PARF_PHY_CTRL), rd(parf + PARF_MHI_CLOCK_RESET), rd(parf + PARF_AXI_MSTR_HALT),
	    rd(parf + PARF_STATUS));
	printf("pcie: 4: dbi_base %#x, slv_addr_space %#x, device_type %#x, int_all_status %#x\n",
	    rd(parf + PARF_DBI_BASE_ADDR), rd(parf + PARF_SLV_ADDR_SPACE), rd(parf + PARF_DEVICE_TYPE),
	    rd(parf + PARF_INT_ALL_STATUS));
}

void
hakios_pcie_init(void)
{
	uint32_t steps = 0;

	if (!PE_parse_boot_argn("pcie_step", &steps, sizeof(steps)) || steps == 0) {
		return;
	}
	step = 1;
	printf("pcie: step %u of the PCIe root complex\n", steps);
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
		printf("pcie: stopping here (pcie_step=%u)\n", steps);
		return;
	}
	if (parf == 0 && !pcie_map()) {
		return;
	}
	step_read();
}
