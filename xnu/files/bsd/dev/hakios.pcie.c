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
	dbi = ml_io_map(PCIE_DM_CORE_PHYS, PCIE_DM_CORE_SIZE);
	tell("pcie: dbi at %#lx\n", dbi);
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
		 *   PARF_SLV_ADDR_SPACE_SIZE = 16 MB
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

		wr(parf + PARF_SLV_ADDR_SPACE, 0x01000000);
		tell("pcie: 6: slv_addr_space %#x\n", rd(parf + PARF_SLV_ADDR_SPACE));

		was = rd(parf + PARF_AXI_MSTR_HALT);
		wr(parf + PARF_AXI_MSTR_HALT, was | (1u << 31));
		tell("pcie: 6: axi_mstr_halt %#x -> %#x\n", was, rd(parf + PARF_AXI_MSTR_HALT));
	}
	if (steps >= 7) {
		tell("pcie: 7: parf ltssm %#x, status %#x, pm_stts %#x\n", rd(parf + PARF_LTSSM),
		    rd(parf + PARF_STATUS), rd(parf + PARF_PM_STTS));
		if (dbi != 0) {
			tell("pcie: 7: reading the dbi's id (%#x at %#lx) -- if nothing follows, that read hung\n",
			    DBI_ID, dbi);
			tell("pcie: 7: dbi id %#x (Qualcomm is 0x17cb), header type %#x, cap %#x\n",
			    rd(dbi + DBI_ID), rd(dbi + DBI_HEADER_TYPE), rd(dbi + DBI_CAP));
		}
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
