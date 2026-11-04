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
#include <kern/locks.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <mach/vm_param.h>
#include <pexpert/pexpert.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <sys/hakios.lx.h>
#include <vm/memory_types.h>
#include <vm/vm_kern.h>

#include "hakios.rpmh.h"

/* (hakios.display.c declares these the same way: the header that has them is not one the drivers take) */
extern vm_offset_t ml_io_map_unmappable(vm_offset_t phys_addr, vm_size_t size, unsigned int flags);
extern kern_return_t thread_terminate(thread_t thread);
extern void loader_log_putc(char c);
extern boolean_t ml_get_interrupts_enabled(void);
extern uint64_t kvtophys(vm_offset_t address);
extern const uint8_t *hakios_image_file(const char *dir, const char *name, uint64_t *size, int *disk_out);
extern lck_mtx_t hakios_smmu_lock;

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

/*
 * This probe runs after VM startup. ml_io_map() uses the permanent 8 MiB
 * io_submap, which the display and GPU already share. Repeated north-tile
 * mappings exhausted that map at step 19 on a GPU-enabled alioth boot.
 *
 * Use the ordinary kernel map with device attributes, and reuse whole mapped
 * pages across these sequential steps. Only pcie_run's single thread uses
 * this cache; it never changes another driver's mappings or SMMU context.
 */
static struct {
	vm_offset_t physical;
	vm_size_t size;
	vm_offset_t virtual;
} pcie_mappings[32];
static unsigned pcie_mapping_count;

static vm_offset_t
pcie_io_map(vm_offset_t physical, vm_size_t size)
{
	vm_offset_t first, offset, mapped;
	vm_size_t length;

	if (size == 0 || physical > (vm_offset_t)-1 - size) {
		return 0;
	}
	first = trunc_page(physical);
	offset = physical - first;
	if (size > (vm_size_t)-1 - offset - PAGE_MASK) {
		return 0;
	}
	length = round_page(size + offset);
	for (unsigned i = 0; i < pcie_mapping_count; i++) {
		if (first >= pcie_mappings[i].physical &&
		    first - pcie_mappings[i].physical <= pcie_mappings[i].size &&
		    length <= pcie_mappings[i].size - (first - pcie_mappings[i].physical)) {
			return pcie_mappings[i].virtual + physical - pcie_mappings[i].physical;
		}
	}
	if (pcie_mapping_count == sizeof(pcie_mappings) / sizeof(pcie_mappings[0])) {
		tell("pcie: mapping cache full; refusing a new window\n");
		return 0;
	}
	mapped = ml_io_map_unmappable(first, length, VM_WIMG_IO);
	if (mapped == 0) {
		return 0;
	}
	pcie_mappings[pcie_mapping_count].physical = first;
	pcie_mappings[pcie_mapping_count].size = length;
	pcie_mappings[pcie_mapping_count++].virtual = mapped;
	tell("pcie: dynamic mapping %#lx + %#lx -> %#lx\n", first, length, mapped);
	return mapped + offset;
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
	parf = pcie_io_map(PCIE_PARF_PHYS, PCIE_PARF_SIZE);
	if (parf == 0) {
		tell("pcie: parf (%#lx) could not be mapped\n", (unsigned long) PCIE_PARF_PHYS);
		return false;
	}
	tell("pcie: parf at %#lx\n", parf);
	phy = pcie_io_map(PCIE_PHY_PHYS, PCIE_PHY_SIZE);
	tell("pcie: phy at %#lx\n", phy);
	dbi = pcie_io_map(PCIE_DM_CORE_PHYS, PCIE_DM_CORE_SIZE);
	tell("pcie: dbi at %#lx\n", dbi);
	elbi = pcie_io_map(PCIE_ELBI_PHYS, PCIE_ELBI_SIZE);
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
	vm_offset_t gcc = pcie_io_map(0x00100000UL, 0x00080000UL);
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
		gcc_window = pcie_io_map(0x00100000UL, 0x00100000UL);
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

	tlmm = pcie_io_map(TLMM_NORTH_PHYS, TLMM_SIZE);
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

	window = pcie_io_map(CMD_DB_PHYS, CMD_DB_SIZE);
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

	rsc = pcie_io_map(RSC_DRV2_PHYS, RSC_DRV_SIZE);
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
	window = pcie_io_map(CMD_DB_PHYS, CMD_DB_SIZE);
	if (window == 0) {
		tell("pcie: 15: the command database could not be mapped\n");
		return;
	}
	for (i = 0; i < CMD_DB_SIZE / 4; i++) {
		uint32_t word = ((volatile uint32_t *) window)[i];

		__builtin_memcpy(db + 4 * i, &word, 4);
	}
	rsc = pcie_io_map(RSC_DRV2_PHYS, RSC_DRV_SIZE);
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

	tlmm = pcie_io_map(0x0f900000UL, 0x0007d000UL);   /* (the north tile: see step 11) */
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

	arb_core = pcie_io_map(ARB_CORE_PHYS, ARB_CORE_SIZE);
	arb_observer = pcie_io_map(ARB_OBSERVER_PHYS, ARB_OBSERVER_SIZE);
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

	tlmm = pcie_io_map(0x0f900000UL, 0x0007d000UL);
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
	bus0 = pcie_io_map(ECAM_BUS0_PHYS, ECAM_SIZE);
	bus1 = pcie_io_map(ECAM_BUS1_PHYS, ECAM_SIZE);
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
	tlmm = pcie_io_map(0x0f900000UL, 0x0007d000UL);
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
		vm_offset_t ep = pcie_io_map(ECAM_EP_PHYS, 0x00010000UL);

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
				vm_offset_t ep = pcie_io_map(ECAM_EP_PHYS, 0x00010000UL);

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
	tlmm = pcie_io_map(0x0f900000UL, 0x0007d000UL);
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
			vm_offset_t ep = pcie_io_map(ECAM_EP_PHYS, 0x00010000UL);

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

	return (rd(parf + PARF_LTSSM) & 0x3f) == 0x11 ||
	    (debug1 & DBI_PORT_DEBUG1_LINK_UP) != 0 ||
	    (link != 0xffffffffu && ((link >> 29) & 1) != 0);
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
	/* Step 19 may already have trained this board. Do not reset a live link. */
	if (link_up_now()) {
		tell("pcie: 27: link already up (state %#x); preserving the trained endpoint\n",
		    rd(parf + PARF_LTSSM) & 0x3f);
		return;
	}
	tlmm = pcie_io_map(0x0f900000UL, 0x0007d000UL);
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
	now = rd(parf + PARF_LTSSM);
	wr(parf + PARF_LTSSM, (now & ~(1u << 11) & ~(1u << 10)) | (1u << 8));
	tell("pcie: 27: parf ltssm %#x -> %#x\n", now, rd(parf + PARF_LTSSM));

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
		}
	}
	now = rd(parf + PARF_LTSSM);
	tell("pcie: 27: after %u tries: ltssm %#x (state %#x, flush %u), debug0 %#x, debug1 %#x, dbi link %#x\n", i, now,
	    now & 0x3f, (now >> 11) & 1, rd(dbi + DBI_PORT_DEBUG0), rd(dbi + DBI_PORT_DEBUG1),
	    rd(dbi + DBI_LINK_CTRL_STATUS));
	if (link_up_now()) {
		vm_offset_t ep = pcie_io_map(ECAM_EP_PHYS, 0x00010000UL);

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
		return;
	}
	iatu = pcie_io_map(IATU_PHYS, IATU_SIZE);
	conf = pcie_io_map(CONF_PHYS, CONF_SIZE);
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
 * Step 29: assign the QCA6390's 64-bit BAR0 and read its boot interface.
 * Addresses and the 1 MiB size were independently read from this K40's
 * Android PCI resources. MHI/BHI offsets follow OpenBSD if_qwx_pci.c and
 * Linux ath11k (BSD/ISC). No firmware transfer, MSI, bus mastering or DMA.
 * DWC unrolled outbound region 1 is +0x200; region 0 remains for config.
 */
#define QCA6390_ID             0x110117cbu
#define QCA6390_BAR0_PHYS      0x60300000u
#define QCA6390_BAR0_SIZE      0x00100000u

static inline void
wr16(vm_offset_t address, uint16_t value)
{
	*(volatile uint16_t *) address = value;
}

static void
step_endpoint_mhi(void)
{
	vm_offset_t conf, iatu, registers;
	uint32_t original_low, original_high, mask_low, mask_high, bhi, bhie;
	uint16_t command;
	uint64_t mask, size;

	if (!link_up_now()) {
		tell("pcie: 29: link down; no BAR access\n");
		return;
	}
	conf = pcie_io_map(CONF_PHYS, CONF_SIZE);
	iatu = pcie_io_map(IATU_PHYS, IATU_SIZE);
	if (conf == 0 || iatu == 0 || rd(conf) != QCA6390_ID) {
		tell("pcie: 29: endpoint is not QCA6390; no configuration writes\n");
		return;
	}
	command = rd(conf + 0x04) & 0xffff;
	original_low = rd(conf + 0x10);
	original_high = rd(conf + 0x14);
	if ((original_low & 7) != 4) {
		tell("pcie: 29: BAR0 %#x is not 64-bit memory\n", original_low);
		return;
	}
	/* 16-bit accesses avoid writing PCI status W1C bits. Keep DMA disabled. */
	wr16(conf + 0x04, command & ~6u);
	wr(conf + 0x10, 0xffffffffu);
	wr(conf + 0x14, 0xffffffffu);
	mask_low = rd(conf + 0x10);
	mask_high = rd(conf + 0x14);
	wr(conf + 0x10, original_low);
	wr(conf + 0x14, original_high);
	mask = ((uint64_t) mask_high << 32) | (mask_low & ~15u);
	size = ~mask + 1;
	tell("pcie: 29: BAR0 mask %#x:%#x, size %#llx\n", mask_high, mask_low, size);
	if (size != QCA6390_BAR0_SIZE || (mask_low & 7) != 4) {
		tell("pcie: 29: BAR size differs from Android; original BARs restored\n");
		wr16(conf + 0x04, command & ~4u);
		return;
	}
	iatu_region0(iatu + 0x200, 0, QCA6390_BAR0_PHYS,
	    QCA6390_BAR0_PHYS + QCA6390_BAR0_SIZE - 1, QCA6390_BAR0_PHYS);
	if ((rd(iatu + 0x204) & (1u << 31)) == 0) {
		tell("pcie: 29: memory iATU did not enable; original BARs restored\n");
		wr16(conf + 0x04, command & ~4u);
		return;
	}
	wr(conf + 0x14, 0);
	wr(conf + 0x10, QCA6390_BAR0_PHYS | (original_low & 15));
	if (rd(conf + 0x10) != (QCA6390_BAR0_PHYS | (original_low & 15)) || rd(conf + 0x14) != 0) {
		tell("pcie: 29: BAR assignment did not stick; original BARs restored\n");
		wr(conf + 0x10, original_low);
		wr(conf + 0x14, original_high);
		wr16(conf + 0x04, command & ~4u);
		return;
	}
	wr16(conf + 0x04, (command | 2u) & ~4u);
	registers = pcie_io_map(QCA6390_BAR0_PHYS, QCA6390_BAR0_SIZE);
	if (registers == 0) {
		tell("pcie: 29: BAR0 virtual mapping unavailable\n");
		return;
	}
	tell("pcie: 29: BAR0 %#x, command %#x; DMA disabled\n", rd(conf + 0x10), rd(conf + 0x04) & 0xffff);
	tell("pcie: 29: MHI len %#x version %#x config %#x ctrl %#x status %#x\n",
	    rd(registers), rd(registers + 8), rd(registers + 0x10),
	    rd(registers + 0x38), rd(registers + 0x48));
	bhi = rd(registers + 0x28);
	bhie = rd(registers + 0x2c);
	tell("pcie: 29: BHI offset %#x, BHIE offset %#x, chdb %#x, erdb %#x\n",
	    bhi, bhie, rd(registers + 0x18), rd(registers + 0x20));
	/* ath11k guarantees the first 4 KiB are accessible without a wake request. */
	if ((bhi & 3) == 0 && bhi >= 0x50 && bhi <= 0xfe0 - 0x44) {
		tell("pcie: 29: BHI execenv %#x, status %#x, serial %#x\n",
		    rd(registers + bhi + 0x28), rd(registers + bhi + 0x2c), rd(registers + bhi + 0x40));
	} else {
		tell("pcie: 29: BHI offset outside always-accessible registers; not dereferenced\n");
	}
}

/*
 * Step 30 adapts only the generic Arm SMMU page-table and context sequence of
 * hakios.wifi.c/radio_space_on(). No WCN6750 registers or WPSS startup code.
 * The live K40 iommu-map sends BDF 01:00.0 (0x100) to SID 0x1c01, not 0x1c00.
 * Only an unused match slot and a disabled, unreferenced context are taken.
 */
#define QCA_SID          0x1c01u
#define QCA_DMA_IOVA     0xa0000000ull
#define QCA_SBL_SIZE     0x80000u
#define QCA_AMSS_SIZE    4352144u
#define QCA_DMA_SIZE     0xc00000u
#define QCA_RUNTIME_OFFSET 0x480000u
#define QCA_M3_SIZE      266684u
#define QCA_TABLE_SIZE   0x26000u
#define QCA_DRIVER_IOVA  0xa1000000ull
#define QCA_DRIVER_SIZE  (56u << 20)
#define QCA_FW_OFFSET    0x10000u
#define QCA_SMMU_SCTLR   (1u | 1u << 1 | 1u << 2 | 1u << 5 | 1u << 8 | 1u << 12)
#define QCA_SMMU_TCR     (25u | 1u << 8 | 1u << 10 | 3u << 12 | 1u << 23)
#define QCA_SMMU_TCR2    (1u | 1u << 4 | 7u << 15)
#define QCA_SMMU_CBAR    (1u << 16 | 3u << 8 | 0xfu << 12)
#define QCA_DMA_PTE      (3ull | 1ull << 6 | 2ull << 8 | 1ull << 10 | 1ull << 11)

static vm_offset_t qca_driver_tables_address;
static bool qca_driver_pool_mapped;

static const uint32_t qca_cb_offsets[] = {
	0x10, 0x20, 0x24, 0x28, 0x2c, 0x30, 0x38, 0x3c
};
static struct {
	bool active;
	vm_offset_t smmu, second, cb, conf, tlmm;
	uint32_t match, context, old_smr, old_s2cr, old_cbar, old_cba2r, old_sctlr;
	uint32_t old_cb[sizeof(qca_cb_offsets) / sizeof(qca_cb_offsets[0])];
	uint16_t old_command, old_root_command;
	uint32_t old_sid_entry, old_sid_config;
	bool msi_active;
	uint32_t old_msi_low, old_msi_high, old_msi_enable, old_msi_mask, old_msi_status;
} qca_dma;

static void
qca_dma_sync(vm_offset_t address, vm_size_t size)
{
	for (vm_offset_t line = address; line < address + size; line += 64) {
		__asm__ volatile("dc civac, %0" : : "r"(line) : "memory");
	}
	__asm__ volatile("dsb sy" : : : "memory");
}

static void
qca_dma_tables(uint64_t *tables, uint64_t table_pa, uint64_t data_pa)
{
	/* 39-bit IOVA, 4 KiB pages. The gap and absent driver pool stay invalid. */
	bzero(tables, QCA_TABLE_SIZE);
	tables[QCA_DMA_IOVA >> 30] = (table_pa + 0x1000) | 3;
	for (unsigned i = 0; i < 36; i++) {
		tables[512 + ((QCA_DMA_IOVA >> 21) & 511) + i] = (table_pa + 0x2000 + 0x1000ull * i) | 3;
	}
	for (unsigned i = 0; i < QCA_DMA_SIZE / 4096; i++) {
		tables[1024 + i] = (data_pa + 4096ull * i) | QCA_DMA_PTE;
	}
}

static bool
qca_driver_pool_tables(uint64_t *tables, uint64_t physical, unsigned long size)
{
	if (!tables || !physical || (physical & 4095) || size != QCA_DRIVER_SIZE ||
	    physical > (1ull << 48) - size) return false;
	unsigned first = (QCA_DRIVER_IOVA - QCA_DMA_IOVA) / 4096;
	for (unsigned i = 0; i < size / 4096; i++) {
		if (tables[1024 + first + i]) return false;
	}
	for (unsigned i = 0; i < size / 4096; i++) {
		tables[1024 + first + i] = (physical + 4096ull * i) | QCA_DMA_PTE;
	}
	return true;
}

int
lx_os_pcie(void)
{
	uint32_t mode = 0;
	PE_parse_boot_argn("pcie_ipcr", &mode, sizeof(mode));
	return mode == 2;
}

int
hakios_pcie_radio_map(unsigned long long physical, unsigned long size, unsigned long long *address)
{
	/* Called by lx_prepare BEFORE the private SMMU context is enabled. */
	if (!address || qca_dma.active || qca_driver_pool_mapped) {
		tell("pcie: 34: radio_map refused: address %p active %u mapped %u (phys %#llx size %#lx)\n",
		    address, qca_dma.active, qca_driver_pool_mapped, physical, size);
		return 0;
	}
	if (!qca_driver_pool_tables((void *)qca_driver_tables_address, physical, size)) {
		tell("pcie: 34: radio_map: the pool tables refused phys %#llx size %#lx (tables %p)\n",
		    physical, size, (void *)qca_driver_tables_address);
		return 0;
	}
	qca_driver_pool_mapped = true;
	*address = QCA_DRIVER_IOVA;
	return 1;
}

void
hakios_pcie_quiesce(void)
{
	if (!qca_dma.active) {
		return;
	}
	qca_dma.active = false; /* also makes this hook safe on a nested panic */
	/* Stop the endpoint before it loses its DMA translations. */
	wr16(qca_dma.conf + 4, qca_dma.old_command & ~4u);
	wr(qca_dma.tlmm + 0x4f004, 0);            /* PERST# asserted */
	wr(qca_dma.tlmm + 0x14004, rd(qca_dma.tlmm + 0x14004) & ~2u);
	__asm__ volatile("dsb sy" : : : "memory");
	if (qca_dma.msi_active) {
		qca_dma.msi_active = false;
		wr(dbi + 0x828, 0);
		wr(dbi + 0x82c, 0xffffffff);
		wr(dbi + 0x830, rd(dbi + 0x830) & ~qca_dma.old_msi_status);
		wr(dbi + 0x820, qca_dma.old_msi_low);
		wr(dbi + 0x824, qca_dma.old_msi_high);
		wr(dbi + 0x82c, qca_dma.old_msi_mask);
		wr(dbi + 0x828, qca_dma.old_msi_enable);
		tell("pcie: 31: masked MSI receiver restored\n");
	}
	wr(qca_dma.smmu + 0x800 + 4 * qca_dma.match, qca_dma.old_smr);
	wr(qca_dma.smmu + 0xc00 + 4 * qca_dma.match, qca_dma.old_s2cr);
	wr(qca_dma.cb, 0);
	wr(qca_dma.second + 4 * qca_dma.context, qca_dma.old_cbar);
	wr(qca_dma.second + 0x800 + 4 * qca_dma.context, qca_dma.old_cba2r);
	for (unsigned i = 0; i < sizeof(qca_cb_offsets) / sizeof(qca_cb_offsets[0]); i++) {
		wr(qca_dma.cb + qca_cb_offsets[i], qca_dma.old_cb[i]);
	}
	wr(qca_dma.cb, qca_dma.old_sctlr);
	wr(parf + 0x2054, qca_dma.old_sid_entry);
	wr(parf + 0x2c00, qca_dma.old_sid_config);
	wr16(dbi + 4, qca_dma.old_root_command);
	__asm__ volatile("dsb sy" : : : "memory");
	tell("pcie: 30: endpoint stopped; SMMU match %u and context %u restored\n", qca_dma.match, qca_dma.context);
}

static bool
qca_dma_prepare(uint64_t table_pa)
{
	bool used[256] = { false };
	uint32_t scr0, idr0, idr1, page, pages, matches, contexts;
	uint32_t match = ~0u, context = ~0u;
	uint64_t root;

	qca_dma.smmu = pcie_io_map(0x15000000, 0x2000);
	qca_dma.conf = pcie_io_map(CONF_PHYS, CONF_SIZE);
	qca_dma.tlmm = pcie_io_map(0x0f900000, 0x7d000);
	if (!qca_dma.smmu || !qca_dma.conf || !qca_dma.tlmm) {
		return false;
	}
	scr0 = rd(qca_dma.smmu);
	idr0 = rd(qca_dma.smmu + 0x20);
	idr1 = rd(qca_dma.smmu + 0x24);
	page = idr1 >> 31 ? 0x10000 : 0x1000;
	pages = 2u << ((idr1 >> 28) & 7);
	matches = idr0 & 0xff;
	contexts = idr1 & 0xff;
	tell("pcie: 30: apps SMMU scr0 %#x, idr0 %#x, idr1 %#x, SID %#x\n", scr0, idr0, idr1, QCA_SID);
	if ((scr0 & (1u | 1u << 3)) || 2 * pages * page != 0x100000 ||
	    matches == 0 || contexts == 0 || contexts > pages || page != 0x1000) {
		tell("pcie: 30: unsupported SMMU layout; no writes\n");
		return false;
	}
	lck_mtx_lock(&hakios_smmu_lock);
	for (unsigned i = 0; i < matches; i++) {
		uint32_t smr = rd(qca_dma.smmu + 0x800 + 4 * i);
		uint32_t s2cr = rd(qca_dma.smmu + 0xc00 + 4 * i);
		if (smr & (1u << 31)) {
			used[s2cr & 0xff] = true;
			if (((smr ^ QCA_SID) & 0x7fff & ~(smr >> 16)) == 0) {
				tell("pcie: 30: SID already covered by match %u (%#x/%#x); left untouched\n", i, smr, s2cr);
				lck_mtx_unlock(&hakios_smmu_lock);
				return false;
			}
		} else if (match == ~0u) {
			match = i;
		}
	}
	for (unsigned i = 0; i < contexts; i++) {
		vm_offset_t cb;
		if (used[i]) {
			continue;
		}
		cb = pcie_io_map(0x15000000 + (pages + i) * page, page);
		if (cb != 0 && (rd(cb) & 1) == 0) {
			context = i;
			qca_dma.cb = cb;
			break;
		}
	}
	qca_dma.second = pcie_io_map(0x15000000 + page, page);
	if (match == ~0u || context == ~0u || qca_dma.second == 0) {
		lck_mtx_unlock(&hakios_smmu_lock);
		tell("pcie: 30: no free SMMU slot/context; no writes\n");
		return false;
	}
	qca_dma.match = match;
	qca_dma.context = context;
	qca_dma.old_smr = rd(qca_dma.smmu + 0x800 + 4 * match);
	qca_dma.old_s2cr = rd(qca_dma.smmu + 0xc00 + 4 * match);
	qca_dma.old_cbar = rd(qca_dma.second + 4 * context);
	qca_dma.old_cba2r = rd(qca_dma.second + 0x800 + 4 * context);
	qca_dma.old_sctlr = rd(qca_dma.cb);
	qca_dma.old_command = rd(qca_dma.conf + 4) & 0xffff;
	qca_dma.old_root_command = rd(dbi + 4) & 0xffff;
	/* CRC-8/poly 0x07 of BDF bytes {0x01, 0x00} is 0x15.
	 * PARF entry: BDF[31:16], SID offset[15:8], next[7:0].
	 * This root complex has only the verified QCA6390 endpoint. Vendor
	 * msm_pcie_config_sid() clears the uninitialized SRAM table at startup.
	 * Replace just our hash slot while its bus master is disabled, and
	 * restore the inherited value after resetting that endpoint.
	 */
	qca_dma.old_sid_entry = rd(parf + 0x2054);
	qca_dma.old_sid_config = rd(parf + 0x2c00);
	if (qca_dma.old_command & 4) {
		lck_mtx_unlock(&hakios_smmu_lock);
		tell("pcie: 30: endpoint DMA was already enabled; no SMMU writes\n");
		return false;
	}
	tell("pcie: 30: private BDF/SID hash slot inherited %#x; saved for restoration\n", qca_dma.old_sid_entry);
	for (unsigned i = 0; i < sizeof(qca_cb_offsets) / sizeof(qca_cb_offsets[0]); i++) {
		qca_dma.old_cb[i] = rd(qca_dma.cb + qca_cb_offsets[i]);
	}
	qca_dma.active = true;
	root = table_pa | (uint64_t)context << 48;
	wr(qca_dma.cb, 0);
	wr(qca_dma.second + 0x800 + 4 * context, 1);
	wr(qca_dma.second + 4 * context, QCA_SMMU_CBAR);
	wr(qca_dma.cb + 0x10, QCA_SMMU_TCR2);
	wr(qca_dma.cb + 0x20, (uint32_t)root);
	wr(qca_dma.cb + 0x24, root >> 32);
	wr(qca_dma.cb + 0x28, 0);
	wr(qca_dma.cb + 0x2c, 0);
	wr(qca_dma.cb + 0x30, QCA_SMMU_TCR);
	wr(qca_dma.cb + 0x38, 0xf404ff44);
	wr(qca_dma.cb + 0x3c, 0);
	wr(qca_dma.cb + 0x58, 0xc00001fe);
	wr(qca_dma.cb + 0x610, context);
	wr(qca_dma.cb + 0x7f0, 1);
	for (unsigned us = 0; rd(qca_dma.cb + 0x7f4) & 1; us += 10) {
		if (us >= 100000) {
			lck_mtx_unlock(&hakios_smmu_lock);
			hakios_pcie_quiesce();
			return false;
		}
		delay_for_interval(10, NSEC_PER_USEC);
	}
	wr(qca_dma.cb, QCA_SMMU_SCTLR);
	wr(qca_dma.smmu + 0xc00 + 4 * match, context);
	wr(qca_dma.smmu + 0x800 + 4 * match, 1u << 31 | QCA_SID);
	wr(parf + 0x2054, 0x01000100);
	wr(parf + 0x2c00, qca_dma.old_sid_config & ~1u);
	__asm__ volatile("dsb sy" : : : "memory");
	bool accepted = rd(qca_dma.smmu + 0x800 + 4 * match) == (1u << 31 | QCA_SID) &&
	    (rd(qca_dma.smmu + 0xc00 + 4 * match) & 0x300ff) == context &&
	    (rd(qca_dma.second + 4 * context) & 0x30000) == (QCA_SMMU_CBAR & 0x30000) &&
	    (rd(qca_dma.cb) & 1) && rd(qca_dma.cb + 0x20) == (uint32_t)root &&
	    rd(qca_dma.cb + 0x24) == (uint32_t)(root >> 32) &&
	    rd(qca_dma.cb + 0x30) == QCA_SMMU_TCR && rd(qca_dma.cb + 0x38) == 0xf404ff44 &&
	    rd(parf + 0x2054) == 0x01000100 && (rd(parf + 0x2c00) & 1) == 0;
	lck_mtx_unlock(&hakios_smmu_lock);
	if (!accepted) {
		tell("pcie: 30: SMMU programming rejected; DMA stays disabled\n");
		hakios_pcie_quiesce();
		return false;
	}
	tell("pcie: 30: SID %#x -> match %u context %u, root %#llx; isolated %#x-byte DMA mapping\n",
	    QCA_SID, match, context, root, QCA_DMA_SIZE);
	return true;
}

/* QCA6390 pre-boot reset sequence from qwx (same notices as below).
 * This is called only for the verified endpoint with bus mastering off.
 * qwx documents stale hot-reset interrupts crashing SBL, and a warm-entry
 * cookie taking Q6 down an invalid path after a host reboot.
 */
static bool
qca_boot_reset(vm_offset_t registers, vm_offset_t conf)
{
	uint32_t window, warm, cause, reset, status;

	if (rd(conf) != QCA6390_ID || (rd(conf + 4) & 4) ||
	    rd(registers + 0x28) != 0x100 || rd(registers + 0x128) != 0) {
		tell("pcie: 31: pre-boot reset preconditions failed; left untouched\n");
		return false;
	}
	window = rd(registers + 0x310c);
	if (window == 0xffffffff) {
		return false;
	}
	tell("pcie: 31: pre-boot reset: clearing endpoint hot-reset interrupts\n");
	wr(registers + 0x310c, 0x4000003c); /* window for 0x01e08228 */
	if ((rd(registers + 0x310c) & 0x4000003f) != 0x4000003c) {
		wr(registers + 0x310c, window);
		return false;
	}
	wr(registers + 0x88228, 0xffffffff); /* PCIE_PCIE_INT_ALL_CLEAR, W1C */
	wr(registers + 0x310c, 0x4000003f); /* WLAON window */
	if ((rd(registers + 0x310c) & 0x4000003f) != 0x4000003f) {
		wr(registers + 0x310c, window);
		return false;
	}
	warm = rd(registers + 0x80504);       /* WLAON_WARM_SW_ENTRY */
	wr(registers + 0x80504, 0);
	delay_for_interval(10000, NSEC_PER_USEC);
	cause = rd(registers + 0x8060c);      /* reset cause, read-to-clear */
	wr(registers + 0x310c, window);
	reset = rd(registers + 0x3008);      /* PCIE_SOC_GLOBAL_RESET */
	if (reset == 0xffffffff) {
		return false;
	}
	tell("pcie: 31: pre-boot warm entry %#x, reset cause %#x; toggling SoC reset\n", warm, cause);
	wr(registers + 0x3008, reset | 1);
	delay_for_interval(10000, NSEC_PER_USEC);
	wr(registers + 0x3008, reset & ~1u);
	delay_for_interval(10000, NSEC_PER_USEC);
	if (rd(conf) != QCA6390_ID) {
		tell("pcie: 31: endpoint absent after SoC reset; no firmware transfer\n");
		return false;
	}
	status = rd(registers + 0x48);
	if ((status & 4) || ((status >> 8) & 0xff) == 0xff) {
		wr(registers + 0x38, 2); /* clear SoC-reset SYSERR through MHI reset */
		delay_for_interval(10000, NSEC_PER_USEC);
	}
	tell("pcie: 31: pre-boot reset requested, MHI ctrl %#x status %#x, EE %#x, BHI error %#x\n",
	    rd(registers + 0x38), rd(registers + 0x48), rd(registers + 0x128), rd(registers + 0x130));
	/* In PBL, download SBL before requiring MHI RESET to clear. This
	 * matches ath11k's sw_reset -> mhi_start ordering. SBL/READY and
	 * actual M0 are still required before any BHIE/context transport.
	 */
	reset = rd(registers + 0x38);
	status = rd(registers + 0x48);
	return (reset == 0 || reset == 2) && (status == 0 || status == 0xff04) &&
	       rd(registers + 0x28) == 0x100 && rd(registers + 0x128) == 0 && (rd(conf + 4) & 4) == 0;
}

static bool
qca_wait_pbl_reset(vm_offset_t registers)
{
	uint32_t control = 2, status = 0xff04, ee = 0;
	/* MSI and bus mastering are configured by the caller. Do not write
	 * a BHI transaction while the endpoint is still clearing MHI RESET:
	 * that reset can discard the freshly programmed BHI descriptors.
	 */
	for (unsigned ms = 0; ms < 3000; ms += 10) {
		control = rd(registers + 0x38);
		status = rd(registers + 0x48);
		ee = rd(registers + 0x128);
		if (control == 0xffffffff || status == 0xffffffff || ee != 0) {
			break;
		}
		if ((control & 2) == 0 && (status & 4) == 0) {
			tell("pcie: 31: PBL reset acknowledged before BHI; ctrl %#x status %#x\n", control, status);
			return true;
		}
		delay_for_interval(10000, NSEC_PER_USEC);
	}
	tell("pcie: 31: PBL reset not acknowledged, ctrl %#x status %#x EE %#x; no BHI transfer\n",
	    control, status, ee);
	return false;
}

/* MHI context ABI, READY/M0 sequence and BHIE vectors are adapted from
 * OpenBSD if_qwx_pci.c (Stefan Sperling, ISC; Qualcomm, BSD-3-Clause-Clear).
 * The complete upstream notices accompany this overlay in THIRD_PARTY.txt.
 * No GPL MHI host implementation is incorporated.
 */
struct qca_mhi_context {
	uint32_t config, type, event;
	uint64_t base, length, read, write;
} __attribute__((packed));
struct qca_mhi_vector {
	uint64_t address, length;
};
_Static_assert(sizeof(struct qca_mhi_context) == 44, "MHI context ABI");

static void
qca_mhi_contexts(uint8_t *data)
{
	struct qca_mhi_context *channels = (void *)data;
	struct qca_mhi_context *events = (void *)(data + 0x2000);
	struct qca_mhi_context *command = (void *)(data + 0x3000);

	for (unsigned i = 0; i < 128; i++) {
		channels[i].config = 2u << 8; /* disabled channel, burst mode disabled */
	}
	/* Device-tree channel directions: LOOPBACK, DIAG and IPCR.
	 * All six remain disabled; no transfer buffers are submitted.
	 */
	for (unsigned i = 0; i < 128; i++) {
		if (i == 0 || i == 4 || i == 20) {
			channels[i].type = 1;
			channels[i].event = 1;
		} else if (i == 1 || i == 5 || i == 21) {
			channels[i].type = 2;
			channels[i].event = 1;
		}
	}
	/* Native K40 DT: 32 control, 256 data, 32 bandwidth-scale events.
	 * Special-purpose bandwidth scaling is not enabled by this probe.
	 */
	for (unsigned i = 0; i < 3; i++) {
		uint64_t base = QCA_DMA_IOVA + 0x5000 + 0x1000 * i;
		events[i].config = i == 2 ? 1u << 16 : 0;
		events[i].type = 1;
		events[i].event = i == 2 ? 0 : i + 1;   /* QCA6390 MSI mapping; CPU vectors stay masked */
		events[i].base = base;
		events[i].length = i == 1 ? 0x1000 : 0x200;
		events[i].read = base;
		events[i].write = base + events[i].length - 16;
	}
	command->base = QCA_DMA_IOVA + 0x4000;
	command->length = 0x1000;
	command->read = command->write = command->base;
}

static unsigned
qca_mhi_vectors(uint8_t *data)
{
	struct qca_mhi_vector *vectors = (void *)(data + 0x8000);
	unsigned count = 0;

	for (unsigned offset = 0; offset < QCA_AMSS_SIZE; offset += QCA_SBL_SIZE) {
		unsigned remaining = QCA_AMSS_SIZE - offset;
		vectors[count].address = QCA_DMA_IOVA + QCA_FW_OFFSET + offset;
		vectors[count].length = remaining < QCA_SBL_SIZE ? remaining : QCA_SBL_SIZE;
		count++;
	}
	return count;
}

static void
qca_mhi_write64(vm_offset_t address, uint64_t value)
{
	wr(address + 4, value >> 32);
	wr(address, (uint32_t)value);
}

static bool
qca_mhi_failed(uint32_t status, uint32_t ee)
{
	return (status & 4) != 0 || ((status >> 8) & 0xff) == 0xff || ee == 3;
}

static bool
qca_mhi_sbl_m0(uint32_t status, uint32_t ee)
{
	/* SBL alone is not M0: READY/SBL is observed before this request. */
	return !qca_mhi_failed(status, ee) && ((status >> 8) & 0xff) == 2 && ee == 1;
}

static bool
step_endpoint_amss(vm_offset_t registers, vm_offset_t data)
{
	uint32_t bhi = rd(registers + 0x28), bhie = rd(registers + 0x2c);
	uint32_t erdb = rd(registers + 0x20), chdb = rd(registers + 0x18);
	uint32_t status = 0, ee = 0, sequence;
	unsigned vectors, stable_ms = 0;

	/* Limit all MMIO to the verified always-accessible first 4 KiB. */
	if (bhi != 0x100 || bhie < 0x180 || (bhie & 3) || bhie > 0xfe0 - 0x80 ||
	    erdb < 0x100 || (erdb & 7) || erdb > 0xfe0 - 24 ||
	    chdb < 0x100 || (chdb & 7) || chdb > 0xfe0 - 128 * 8) {
		tell("pcie: 31: unsupported MHI register layout; no AMSS transfer\n");
		return false;
	}
	for (unsigned ms = 0; ms < 3000; ms += 10) {
		status = rd(registers + 0x48);
		if ((rd(registers + 0x38) & 2) == 0 && (status & 1)) {
			break;
		}
		delay_for_interval(10000, NSEC_PER_USEC);
	}
	tell("pcie: 31: after SBL transfer MHI status %#x, execenv %#x, version %#x\n",
	    status, rd(registers + bhi + 0x28), rd(registers + 8));
	if ((rd(registers + 0x38) & 2) || (status & 1) == 0 || (status & 4)) {
		tell("pcie: 31: READY not reached; no MHI context writes\n");
		return false;
	}
	qca_mhi_contexts((void *)data);
	vectors = qca_mhi_vectors((void *)data);
	qca_dma_sync(data, QCA_FW_OFFSET);
	/* Preserve READY like the device-matched vendor host; only MHICTRL requests M0. */
	qca_mhi_write64(registers + 0x58, QCA_DMA_IOVA);
	qca_mhi_write64(registers + 0x60, QCA_DMA_IOVA + 0x2000);
	qca_mhi_write64(registers + 0x68, QCA_DMA_IOVA + 0x3000);
	/* Match qwx's 32-bit MHI address aperture. These are device-side
	 * PCIe window bounds, not SMMU permissions: only this probe's
	 * allocated pages remain accessible in the independent SMMU table.
	 */
	qca_mhi_write64(registers + 0x80, 0);
	qca_mhi_write64(registers + 0x88, 0xffffffffu);
	qca_mhi_write64(registers + 0x98, 0);
	qca_mhi_write64(registers + 0xa0, 0xffffffffu);
	wr(registers + 0x10, (rd(registers + 0x10) & 0x0000ffffu) | 3u << 16);
	tell("pcie: 31: MHI cfg %#x, context bases %#x/%#x/%#x, ctrl/data bounds %#x-%#x/%#x-%#x\n",
	    rd(registers + 0x10), rd(registers + 0x58), rd(registers + 0x60), rd(registers + 0x68),
	    rd(registers + 0x80), rd(registers + 0x88), rd(registers + 0x98), rd(registers + 0xa0));
	for (unsigned i = 0; i < 3; i++) {
		const struct qca_mhi_context *events = (void *)(data + 0x2000);
		qca_mhi_write64(registers + erdb + 8 * i, events[i].write);
	}
	/* Reserve native-sized event rings for boot events; all transfer channels are disabled.
	 * CPU context memory stays untouched while the device owns it; no
	 * unsynchronized CPU/device writes to the same cache line.
	 */
	/* Like qwx and the vendor host: keep wake low during READY. Only
	 * assert it after the device has actually acknowledged M0 below.
	 */
	qca_mhi_write64(registers + chdb + 127 * 8, 0);
	__asm__ volatile("dsb sy" : : : "memory");
	wr(registers + 0x38, (rd(registers + 0x38) & ~0xff00u) | 0x200u);
	tell("pcie: 31: polled MHI contexts ready, requested M0; channels disabled\n");
	for (unsigned ms = 0; ms < 3000; ms += 10) {
		ee = rd(registers + bhi + 0x28);
		status = rd(registers + 0x48);
		if (qca_mhi_sbl_m0(status, ee) || qca_mhi_failed(status, ee)) {
			break;
		}
		delay_for_interval(10000, NSEC_PER_USEC);
	}
	tell("pcie: 31: after M0 MHI status %#x, execenv %#x\n", status, ee);
	tell("pcie: 31: post-M0 BHI error %#x, debug %#x/%#x/%#x, ctrl %#x, wake %#x\n",
	    rd(registers + bhi + 0x30), rd(registers + bhi + 0x34),
	    rd(registers + bhi + 0x38), rd(registers + bhi + 0x3c),
	    rd(registers + 0x38), rd(registers + chdb + 127 * 8));
	if (!qca_mhi_sbl_m0(status, ee)) {
		tell("pcie: 31: M0/SBL not reached; no AMSS transfer\n");
		return false;
	}
	/* Keep the device awake while the bounded firmware-only probe runs. */
	qca_mhi_write64(registers + chdb + 127 * 8, 1);
	qca_mhi_write64(registers + bhie + 0x2c, QCA_DMA_IOVA + 0x8000);
	wr(registers + bhie + 0x34, vectors * sizeof(struct qca_mhi_vector));
	__asm__ volatile("dsb sy" : : : "memory");
	sequence = ((uint32_t)mach_absolute_time() & 0x3fffffff) | 1;
	wr(registers + bhie + 0x3c, (rd(registers + bhie + 0x3c) & 0xc0000000u) | sequence);
	tell("pcie: 31: requested %u-byte AMSS transfer in %u BHIE vectors\n", QCA_AMSS_SIZE, vectors);
	for (unsigned ms = 0; ms < 10000; ms += 10) {
		status = rd(registers + bhie + 0x44);
		ee = rd(registers + bhi + 0x28);
		if ((status >> 30) == 3 || ee == 2 || ee == 4 ||
		    qca_mhi_failed(rd(registers + 0x48), ee)) {
			break;
		}
		delay_for_interval(10000, NSEC_PER_USEC);
	}
	tell("pcie: 31: BHIE status %#x, execenv %#x, MHI status %#x; no network channel started\n",
	    status, ee, rd(registers + 0x48));
	/* Observe mission mode after transfer; seeing AMSS once is insufficient. */
	if ((status >> 30) == 2 && (ee == 2 || ee == 4)) {
		for (unsigned ms = 0; ms < 3000; ms += 10) {
			status = rd(registers + 0x48);
			ee = rd(registers + bhi + 0x28);
			if (qca_mhi_failed(status, ee)) {
				break;
			}
			stable_ms = ((status >> 8) & 0xff) == 2 && (ee == 2 || ee == 4) ? stable_ms + 10 : 0;
			if (stable_ms >= 2000) {
				break;
			}
			delay_for_interval(10000, NSEC_PER_USEC);
		}
		tell("pcie: 31: mission observation MHI status %#x, execenv %#x, stable M0 %u ms\n",
		    status, ee, stable_ms);
	}
	tell("pcie: 31: final BHI error %#x, debug %#x/%#x/%#x, MSI pending %#x\n",
	    rd(registers + bhi + 0x30), rd(registers + bhi + 0x34),
	    rd(registers + bhi + 0x38), rd(registers + bhi + 0x3c), rd(dbi + 0x830));
	return stable_ms >= 2000 && !qca_mhi_failed(status, ee) && (ee == 2 || ee == 4);
}

static bool
qca_boot_msi(uint64_t target)
{
	uint32_t cap = rd(qca_dma.conf + 0x34) & 0xfcu, header = 0;
	uint16_t control;
	unsigned multiple;
	uint32_t enabled;
	bool wide;
	bool visited[64] = { false };

	for (unsigned n = 0; n < 48 && cap >= 0x40 && cap <= 0xe8; n++) {
		if (visited[cap / 4]) {
			break;
		}
		visited[cap / 4] = true;
		header = rd(qca_dma.conf + cap);
		if ((header & 0xff) == 5) {
			break;
		}
		cap = (header >> 8) & 0xfcu;
	}
	control = header >> 16;
	multiple = (control >> 1) & 7;
	wide = (control & 0x80) != 0;
	if (cap < 0x40 || cap > 0xe8 || (header & 0xff) != 5 ||
	    (!wide && (target >> 32)) || (control & 1) || multiple > 5 || rd(dbi + 0x828) != 0) {
		tell("pcie: 31: MSI capability %#x/%#x, root enable %#x mask %#x; left untouched\n",
		    cap, header, rd(dbi + 0x828), rd(dbi + 0x82c));
		return false;
	}
	qca_dma.old_msi_low = rd(dbi + 0x820);
	qca_dma.old_msi_high = rd(dbi + 0x824);
	qca_dma.old_msi_enable = rd(dbi + 0x828);
	qca_dma.old_msi_mask = rd(dbi + 0x82c);
	qca_dma.old_msi_status = rd(dbi + 0x830);
	qca_dma.msi_active = true;
	enabled = multiple == 5 ? 0xffffffff : (1u << (1u << multiple)) - 1;
	/* Consume the endpoint's MSI messages at the DesignWare receiver.
	 * All 32 vectors stay masked: no GIC interrupt or foreign mapping.
	 * The target is in this probe's isolated IOVA range. A 32-bit MSI
	 * message is consumed at the receiver before ordinary inbound DMA.
	 */
	wr(dbi + 0x82c, 0xffffffff);
	wr(dbi + 0x820, (uint32_t)target);
	wr(dbi + 0x824, target >> 32);
	wr(dbi + 0x828, enabled);
	wr(qca_dma.conf + cap + 4, (uint32_t)target);
	if (wide) {
		wr(qca_dma.conf + cap + 8, target >> 32);
	}
	wr16(qca_dma.conf + cap + (wide ? 12 : 8), 0);
	if (control & 0x100) {
		wr(qca_dma.conf + cap + (wide ? 16 : 12), 0);
	}
	__asm__ volatile("dsb sy" : : : "memory");
	wr16(qca_dma.conf + cap + 2, (control & ~0x71u) | multiple << 4 | 1);
	tell("pcie: 31: MSI capability %#x control %#x, target %#llx; root vectors all masked\n",
	    cap, rd(qca_dma.conf + cap) >> 16, target);
	return rd(qca_dma.conf + cap + 4) == (uint32_t)target &&
	       (!wide || rd(qca_dma.conf + cap + 8) == (uint32_t)(target >> 32)) &&
	       ((rd(qca_dma.conf + cap) >> 16) & 0x71) == (multiple << 4 | 1) &&
	       rd(dbi + 0x820) == (uint32_t)target && rd(dbi + 0x824) == (uint32_t)(target >> 32) &&
	       rd(dbi + 0x828) == enabled && rd(dbi + 0x82c) == 0xffffffff &&
	       (!(control & 0x100) || rd(qca_dma.conf + cap + (wide ? 16 : 12)) == 0);
}

/* K40 bounded IPCR probe.
 * MHI command/event/TRE formats and channel startup adapt OpenBSD qwx,
 * with the ISC/BSD notices in THIRD_PARTY.txt. QRTR formats match the
 * existing hakios.wifi.c interface. This is not a network interface.
 * Control and RX memory use the existing lx_os_radio_memory() pattern:
 * clean the original allocation once, then use only its normal-NC view.
 */
#include "hakios.qca6390_qmi.h"

/*
 * The firmware image this phone already has keeps the vendor's names one directory down
 * (fw/qca6390/amss20.bin and friends), while this driver asks for them at the top of fw/
 * (fw/qca6390_amss20.bin). hakios_image_file looks a directory up by name, so a nested path in
 * "name" is not found -- on 2026-11-04 that ended the run at
 * "qmi failed to fetch board file: -2" with the firmware already in mission mode.
 *
 * So each name is tried three ways: as the reference names it, as the vendor's directory under fw/,
 * and with fw/qca6390 as the directory.
 */
static const uint8_t *
fw_find(const char *name, uint64_t *size)
{
	static char nested[64];
	const char *base = name;
	const char *slash = name;
	const uint8_t *found;
	unsigned i = 0;

	while (*name != 0) {
		if (*name == '/') {
			base = name + 1;
		}
		name++;
	}
	(void) slash;
	found = hakios_image_file("fw", base, size, NULL);
	if (found != NULL) {
		return found;
	}
	for (const char *p = "qca6390/"; *p != 0 && i + 1 < sizeof(nested); p++) {
		nested[i++] = *p;
	}
	for (const char *p = base; *p != 0 && i + 1 < sizeof(nested); p++) {
		nested[i++] = *p;
	}
	nested[i] = 0;
	found = hakios_image_file("fw", nested, size, NULL);
	if (found != NULL) {
		return found;
	}
	return hakios_image_file("fw/qca6390", base, size, NULL);
}

struct qca_mhi_tre {
	uint64_t pointer;
	uint32_t word0, word1;
};
_Static_assert(sizeof(struct qca_mhi_tre) == 16, "MHI TRE ABI");

struct qca_ipcr_state {
	unsigned cursor[3], returned[3];
	bool started[2], tx_done, hello, wlfw, failed;
	uint32_t received_slots;
	unsigned packets, peer, service_node, service_port;

	unsigned tx_submitted, tx_completed;
	uint16_t request, transaction;
	bool response, refused, capability, registered, host_cap, memory_request;
	bool confirm, memory_supplied, memory_accepted, memory_ready, init_done, m3, board, regdb;
	unsigned resume_count;
	uint16_t result, error;
	struct qmi_wlanfw_cap_resp_msg_v01 target;
	struct qmi_wlanfw_request_mem_ind_msg_v01 memory;
};

static void
qca_ipcr_barrier(void)
{
	__asm__ volatile("dsb sy" : : : "memory");
}

static unsigned
qca_wire16(const uint8_t *p)
{
	return p[0] | (unsigned)p[1] << 8;
}

static bool
qca_qmi_receive(const uint8_t *packet, unsigned length, struct qca_ipcr_state *state)
{
	if (length < QMI_HEADER_SIZE || qca_wire16(packet + 5) != length - QMI_HEADER_SIZE) return false;
	unsigned message = qca_wire16(packet + 3);
	if (packet[0] == QMI_INDICATION) {
		if (message == QMI_WLFW_FW_MEM_READY_IND_V01 || message == QMI_WLFW_FW_INIT_DONE_IND_V01) {
			if (length != QMI_HEADER_SIZE || !state->memory_supplied) return false;
			if (message == QMI_WLFW_FW_MEM_READY_IND_V01) state->memory_ready = true;
			else state->init_done = true;
			tell("pcie: 34: QMI indication %#x received\n", message);
			return true;
		}
		if (message != QMI_WLANFW_REQUEST_MEM_IND_V01 || state->memory_request) return false;
		if (hakios_qmi_decode(packet, length, qmi_wlanfw_request_mem_ind_msg_v01_ei, &state->memory) < 0 ||
		    state->memory.mem_seg_len == 0 || state->memory.mem_seg_len > ATH11K_QMI_WLANFW_MAX_NUM_MEM_SEG_V01) return false;
		uint64_t total = 0;
		for (unsigned i = 0; i < state->memory.mem_seg_len; i++) {
			const struct qmi_wlanfw_mem_seg_s_v01 *segment = &state->memory.mem_seg[i];
			if (!segment->size || segment->mem_cfg_len > QMI_WLANFW_MAX_NUM_MEM_CFG_V01) return false;
			total += segment->size;
			tell("pcie: 33: QMI memory request segment %u type %u size %#x configs %u; no address supplied\n",
			    i, (unsigned)segment->type, segment->size, segment->mem_cfg_len);
			for (unsigned j = 0; j < segment->mem_cfg_len; j++) {
				tell("pcie: 33: QMI memory config %u/%u offset %#llx size %#x secure %u\n", i, j,
				    (unsigned long long)segment->mem_cfg[j].offset, segment->mem_cfg[j].size, segment->mem_cfg[j].secure_flag);
			}
		}
		state->memory_request = true;
		tell("pcie: 33: QMI memory request %u segments, total %#llx bytes; observation only\n",
		    state->memory.mem_seg_len, (unsigned long long)total);
		return true;
	}
	if (packet[0] != QMI_RESPONSE || qca_wire16(packet + 1) != state->transaction ||
	    message != state->request || state->response) return false;
	/* The existing decoder leaves missing fields zeroed; require the result TLV explicitly. */
	bool found = false;
	for (unsigned at = QMI_HEADER_SIZE; at < length;) {
		if (length - at < 3) return false;
		unsigned size = qca_wire16(packet + at + 1);
		if (size > length - at - 3) return false;
		if (packet[at] == 2) {
			if (found || size != 4) return false;
			found = true;
		}
		at += 3 + size;
	}
	if (!found) return false;
	struct qmi_wlanfw_ind_register_resp_msg_v01 registration = { 0 };
	struct qmi_wlanfw_host_cap_resp_msg_v01 host = { 0 };
	struct qmi_wlanfw_respond_mem_resp_msg_v01 memory = { 0 };
	struct qmi_wlanfw_bdf_download_resp_msg_v01 board = { 0 };
	struct qmi_wlanfw_m3_info_resp_msg_v01 m3 = { 0 };
	struct qmi_response_type_v01 *reply;
	int decoded;
	if (message == QMI_WLANFW_CAP_REQ_V01) {
		decoded = hakios_qmi_decode(packet, length, qmi_wlanfw_cap_resp_msg_v01_ei, &state->target);
		reply = &state->target.resp;
	} else if (message == QMI_WLANFW_IND_REGISTER_REQ_V01) {
		decoded = hakios_qmi_decode(packet, length, qmi_wlanfw_ind_register_resp_msg_v01_ei, &registration);
		reply = &registration.resp;
	} else if (message == QMI_WLANFW_HOST_CAP_REQ_V01) {
		decoded = hakios_qmi_decode(packet, length, qmi_wlanfw_host_cap_resp_msg_v01_ei, &host);
		reply = &host.resp;
	} else if (message == QMI_WLANFW_RESPOND_MEM_REQ_V01) {
		decoded = hakios_qmi_decode(packet, length, qmi_wlanfw_respond_mem_resp_msg_v01_ei, &memory);
		reply = &memory.resp;
	} else if (message == QMI_WLANFW_BDF_DOWNLOAD_REQ_V01) {
		decoded = hakios_qmi_decode(packet, length, qmi_wlanfw_bdf_download_resp_msg_v01_ei, &board);
		reply = &board.resp;
	} else if (message == QMI_WLANFW_M3_INFO_REQ_V01) {
		decoded = hakios_qmi_decode(packet, length, qmi_wlanfw_m3_info_resp_msg_v01_ei, &m3);
		reply = &m3.resp;
	} else return false;
	if (decoded < 0) return false;
	state->response = true;
	state->result = reply->result;
	state->error = reply->error;
	state->refused = reply->result != QMI_RESULT_SUCCESS_V01 || reply->error != QMI_ERR_NONE_V01;
	if (!state->refused) {
		if (message == QMI_WLANFW_CAP_REQ_V01) state->capability = true;
		if (message == QMI_WLANFW_IND_REGISTER_REQ_V01) state->registered = true;
		if (message == QMI_WLANFW_HOST_CAP_REQ_V01) state->host_cap = true;
		if (message == QMI_WLANFW_RESPOND_MEM_REQ_V01) state->memory_accepted = true;
		if (message == QMI_WLANFW_M3_INFO_REQ_V01) state->m3 = true;
	}
	tell("pcie: 33: QMI response message %#x transaction %u result %u error %u\n",
	    message, state->transaction, state->result, state->error);
	return true;
}

static bool
qca_ipcr_qrtr(const uint8_t *packet, unsigned length, struct qca_ipcr_state *state)
{
	uint32_t header[8] = { 0 }, words[5];
	unsigned offset;
	if (length >= sizeof(header) && packet[0] == 1) {
		memcpy(header, packet, sizeof(header));
		if (header[0] != 1) {
			return false;
		}
		offset = sizeof(header);
	} else if (length >= 16 && packet[0] == 3) {
		/* QRTR compact header uses wire version 3. */
		header[1] = packet[1];
		header[2] = packet[8] | (uint32_t)packet[9] << 8;
		header[3] = packet[10] | (uint32_t)packet[11] << 8;
		header[4] = packet[2] & 1;
		memcpy(&header[5], packet + 4, 4);
		header[6] = packet[12] | (uint32_t)packet[13] << 8;
		header[7] = packet[14] | (uint32_t)packet[15] << 8;
		if (header[3] == 0xfffe) header[3] = 0xfffffffe;
		if (header[7] == 0xfffe) header[7] = 0xfffffffe;
		offset = 16u + packet[3];
	} else {
		return false;
	}
	if (offset > length || header[5] > length - offset || length - offset - header[5] > 3) {
		return false;
	}
	if (header[1] == 1) {
		if (!state->wlfw || header[2] != state->service_node || header[3] != state->service_port ||
		    header[6] != 1 || header[7] != 0x4000 || header[4] > 1) return false;
		if (!qca_qmi_receive(packet + offset, header[5], state)) return false;
		if (header[4]) state->confirm = true;
		state->packets++;
		return true;
	}
	if (header[5] < sizeof(words) || header[3] != 0xfffffffe || header[7] != 0xfffffffe ||
	    (header[6] != 1 && header[6] != 0xffffffff && header[6] != 0xffff)) return false;
	memcpy(words, packet + offset, sizeof(words));
	if (words[0] != header[1]) {
		return false;
	}
	state->packets++;
	state->peer = header[2];
	if (words[0] == 2) {
		state->hello = true;
		tell("pcie: 32: QRTR HELLO received from node %u\n", header[2]);
	} else if (words[0] == 4) {
		tell("pcie: 32: QRTR service %u instance %#x at node %u port %u\n",
		    words[1], words[2], words[3], words[4]);
		if (words[1] == 69 && words[3] == header[2] && words[4] != 0 && words[4] != 0xfffffffe) {
			state->wlfw = true;
			state->service_node = words[3];
			state->service_port = words[4];
		}
	}
	return true;
}

static bool
qca_ipcr_poll(vm_offset_t registers, vm_offset_t control, vm_offset_t receive,
    struct qca_ipcr_state *state)
{
	volatile struct qca_mhi_context *events = (void *)(control + 0x2000);
	uint32_t erdb = rd(registers + 0x20);
	for (unsigned ring = 0; ring < 3; ring++) {
		unsigned entries = ring == 1 ? 256 : 32, consumed = 0;
		uint64_t base = QCA_DMA_IOVA + 0x5000 + ring * 0x1000;
		uint64_t device = events[ring].read;
		qca_ipcr_barrier();
		if (device < base || device >= base + entries * 16 || ((device - base) & 15)) {
			return false;
		}
		unsigned limit = (unsigned)(device - base) / 16;
		while (state->cursor[ring] != limit && consumed++ < entries) {
			volatile struct qca_mhi_tre *event = (void *)(control + 0x5000 + ring * 0x1000 + state->cursor[ring] * 16);
			uint64_t pointer = event->pointer;
			uint32_t word0 = event->word0, word1 = event->word1;
			unsigned type = (word1 >> 16) & 255, channel = word1 >> 24, code = word0 >> 24;
			if (type == 0x21) {
				if (ring != 0 || code != 1 || pointer < QCA_DMA_IOVA + 0x4000 ||
				    pointer >= QCA_DMA_IOVA + 0x4020 || (pointer & 15)) {
					return false;
				}
				unsigned slot = (unsigned)(pointer - QCA_DMA_IOVA - 0x4000) / 16;
				if (state->started[slot]) return false;
				state->started[slot] = true;
				tell("pcie: 32: IPCR channel %u start acknowledged, code %u\n", 20 + slot, code);
			} else if (type == 0x22) {
				if (ring != 1 || code != 2) return false;
				if (channel == 20) {
					if (pointer < QCA_DMA_IOVA + 0x9000 || pointer >= QCA_DMA_IOVA + 0x9200 || (pointer & 15)) return false;
					unsigned slot = (unsigned)(pointer - QCA_DMA_IOVA - 0x9000) / 16;
					if (slot >= state->tx_submitted || (state->tx_completed & (1u << slot))) return false;
					state->tx_completed |= 1u << slot;
					if (slot == 0) state->tx_done = true;
				} else if (channel == 21) {
					if (pointer < QCA_DMA_IOVA + 0xa000 || pointer >= QCA_DMA_IOVA + 0xa1f0 || (pointer & 15)) return false;
					unsigned slot = (unsigned)(pointer - QCA_DMA_IOVA - 0xa000) / 16;
					unsigned length = word0 & 0xffff;
					if (length > 4096 || (state->received_slots & (1u << slot))) return false;
					state->received_slots |= 1u << slot;
					qca_ipcr_barrier();
					if (!qca_ipcr_qrtr((const uint8_t *)(receive + 4096 * slot), length, state)) {
						tell("pcie: 32: rejected IPCR packet in slot %u, length %u\n", slot, length);
						return false;
					}
				} else return false;
			} else if (type == 0x20) {
				if (code == 0xff) return false;
			} else if (type == 0x40) {
				if (code == 3) return false;
			} else {
				tell("pcie: 32: unsupported event type %#x code %#x on ring %u\n", type, code, ring);
				return false;
			}
			state->cursor[ring] = (state->cursor[ring] + 1) % entries;
			state->returned[ring] = (state->returned[ring] + 1) % entries;
		}
		if (state->cursor[ring] != limit) return false;
		if (consumed) {
			events[ring].write = base + state->returned[ring] * 16;
			qca_ipcr_barrier();
			qca_mhi_write64(registers + erdb + ring * 8, events[ring].write);
		}
	}
	return true;
}

static bool
qca_ipcr_submit(vm_offset_t registers, vm_offset_t control, struct qca_ipcr_state *state, unsigned length)
{
	unsigned slot = state->tx_submitted;
	if (slot < 1 || slot >= 31 || length < 32 || length > 0x2000 ||
	    state->tx_completed != (1u << slot) - 1u) return false;
	volatile struct qca_mhi_context *channels = (void *)control;
	struct qca_mhi_tre *tx = (void *)(control + 0x9000 + slot * 16);
	tx->pointer = QCA_DMA_IOVA + 0xb000;
	tx->word0 = length;
	tx->word1 = 2u << 16 | 1u << 9 | 1u << 10;
	state->tx_submitted++;
	channels[20].write = QCA_DMA_IOVA + 0x9000 + state->tx_submitted * 16;
	qca_ipcr_barrier();
	qca_mhi_write64(registers + rd(registers + 0x18) + 20 * 8, channels[20].write);
	return true;
}

static bool
qca_ipcr_resume(vm_offset_t registers, vm_offset_t control, vm_offset_t receive, struct qca_ipcr_state *state)
{
	if (!state->confirm) return true;
	uint32_t resume[13] = { 1, 7, 1, 0x4000, 0, 20, state->service_node, state->service_port, 7, 1, 0x4000, 0, 0 };
	state->confirm = false;
	memcpy((void *)(control + 0xb000), resume, sizeof(resume));
	if (!qca_ipcr_submit(registers, control, state, sizeof(resume))) return false;
	unsigned complete = (1u << state->tx_submitted) - 1u;
	for (unsigned ms = 0; ms < 1000; ms++) {
		if (qca_mhi_failed(rd(registers + 0x48), rd(registers + 0x128)) ||
		    !qca_ipcr_poll(registers, control, receive, state)) return false;
		if (state->tx_completed == complete) {
			state->resume_count++;
			return true;
		}
		delay_for_interval(1000, NSEC_PER_USEC);
	}
	return false;
}

static bool
qca_qmi_ask(vm_offset_t registers, vm_offset_t control, vm_offset_t receive,
    struct qca_ipcr_state *state, uint16_t message, const struct qmi_elem_info *table, const void *request)
{
	if (!qca_ipcr_resume(registers, control, receive, state)) return false;
	unsigned slot = state->tx_submitted;
	if (slot < 1 || slot >= 31 || state->transaction == 0xffff || state->tx_completed != (1u << slot) - 1u) return false;
	uint8_t *packet = (void *)(control + 0xb000);
	uint32_t header[8] = { 1, 1, 1, 0x4000, 0, 0, state->service_node, state->service_port };
	state->transaction++;
	state->request = message;
	state->response = state->refused = false;
	state->result = state->error = 0xffff;
	int size = hakios_qmi_encode(QMI_REQUEST, message, state->transaction, table, request, packet + 32, 0x2000 - 32);
	if (size < QMI_HEADER_SIZE || size > 0x2000 - 32) return false;
	header[5] = (unsigned)size;
	memcpy(packet, header, sizeof(header));
	unsigned length = (32u + (unsigned)size + 3u) & ~3u;
	memset(packet + 32 + size, 0, length - 32 - size);
	if (!qca_ipcr_submit(registers, control, state, length)) return false;
	tell("pcie: 33: QMI request message %#x transaction %u submitted\n", message, state->transaction);
	for (unsigned ms = 0; ms < 3000; ms++) {
		if (qca_mhi_failed(rd(registers + 0x48), rd(registers + 0x128)) ||
		    !qca_ipcr_poll(registers, control, receive, state)) return false;
		if (state->response && (state->tx_completed & (1u << slot))) {
			return !state->refused && qca_ipcr_resume(registers, control, receive, state);
		}
		delay_for_interval(1000, NSEC_PER_USEC);
	}
	return false;
}

static bool
qca_qmi_memory_plan(const struct qmi_wlanfw_request_mem_ind_msg_v01 *requested,
    struct qmi_wlanfw_respond_mem_req_msg_v01 *reply, uint32_t *m3_offset)
{
	struct qmi_wlanfw_respond_mem_req_msg_v01 planned = { 0 };
	uint32_t cursor = QCA_RUNTIME_OFFSET;
	if (!requested->mem_seg_len || requested->mem_seg_len > 52) return false;
	planned.mem_seg_len = requested->mem_seg_len;
	for (unsigned i = 0; i < planned.mem_seg_len; i++) {
		const struct qmi_wlanfw_mem_seg_s_v01 *segment = &requested->mem_seg[i];
		if (!segment->size || segment->mem_cfg_len ||
		    (segment->type != QMI_WLANFW_MEM_TYPE_DDR_V01 && segment->type != QMI_WLANFW_MEM_CAL_V01) ||
		    segment->size > QCA_DMA_SIZE - cursor) return false;
		planned.mem_seg[i].addr = QCA_DMA_IOVA + cursor;
		planned.mem_seg[i].size = segment->size;
		planned.mem_seg[i].type = segment->type;
		cursor += segment->size;
		if (cursor > QCA_DMA_SIZE - 0xffffu) return false;
		cursor = (cursor + 0xffffu) & ~0xffffu;
	}
	if (cursor > QCA_DMA_SIZE - QCA_M3_SIZE) return false;
	*m3_offset = cursor;
	*reply = planned;
	return true;
}

static bool
qca_qmi_wait(vm_offset_t registers, vm_offset_t control, vm_offset_t receive,
    struct qca_ipcr_state *state, const bool *flag)
{
	for (unsigned ms = 0; ms < 5000; ms++) {
		if (qca_mhi_failed(rd(registers + 0x48), rd(registers + 0x128)) ||
		    !qca_ipcr_poll(registers, control, receive, state)) return false;
		if (*flag) return qca_ipcr_resume(registers, control, receive, state);
		delay_for_interval(1000, NSEC_PER_USEC);
	}
	return false;
}

static bool
qca_qmi_file(vm_offset_t registers, vm_offset_t control, vm_offset_t receive,
    struct qca_ipcr_state *state, const char *name, unsigned expected, unsigned type)
{
	uint64_t size = 0;
	const uint8_t *file = fw_find(name, &size);

	if (!file || size != expected || expected > 0x20000) {
		tell("pcie: 34: qmi file %s: %s (size %llu, expected %u)\n", name,
		    file == NULL ? "NOT FOUND" : "found but wrong size or too big",
		    (unsigned long long) size, expected);
		return false;
	}
	tell("pcie: 34: qmi file %s found, %llu bytes\n", name, (unsigned long long) size);
	static struct qmi_wlanfw_bdf_download_req_msg_v01 request;
	memset(&request, 0, sizeof(request));
	request.valid = request.file_id_valid = request.total_size_valid = request.seg_id_valid = 1;
	request.data_valid = request.end_valid = request.bdf_type_valid = 1;
	request.file_id = state->target.board_info_valid ? state->target.board_info.board_id : 0xff;
	request.bdf_type = type;
	for (unsigned at = 0; at < expected; request.seg_id++) {
		unsigned left = expected - at;
		request.total_size = left;
		request.end = left <= QMI_WLANFW_MAX_DATA_SIZE_V01;
		request.data_len = request.end ? left : QMI_WLANFW_MAX_DATA_SIZE_V01;
		memcpy(request.data, file + at, request.data_len);
		if (!qca_qmi_ask(registers, control, receive, state, QMI_WLANFW_BDF_DOWNLOAD_REQ_V01,
		    qmi_wlanfw_bdf_download_req_msg_v01_ei, &request)) return false;
		at += request.data_len;
	}
	tell("pcie: 34: QMI file %s accepted, type %u bytes %u segments %u\n", name, type, expected, request.seg_id);
	return true;
}

static bool
qca_ipcr_probe(vm_offset_t registers, vm_offset_t control, vm_offset_t receive, vm_offset_t runtime)
{
	struct qca_ipcr_state state = { .returned = { 31, 255, 31 } };
	volatile struct qca_mhi_context *channels = (void *)control;
	volatile struct qca_mhi_context *command = (void *)(control + 0x3000);
	struct qca_mhi_tre *commands = (void *)(control + 0x4000);
	struct qca_mhi_tre *tx = (void *)(control + 0x9000), *rx = (void *)(control + 0xa000);
	uint32_t chdb = rd(registers + 0x18), status, ee;
	if (chdb < 0x100 || (chdb & 7) || chdb > 0xfe0 - 128 * 8 ||
	    rd(registers + 0x20) < 0x100 || (rd(registers + 0x20) & 7) || rd(registers + 0x20) > 0xfe0 - 24 ||
	    (rd(registers + 0x48) & 0xff04) != 0x200 ||
	    (rd(registers + 0x128) != 2 && rd(registers + 0x128) != 4)) {
		return false;
	}
	/* Both channel contexts are prepared before either START command. */
	for (unsigned i = 0; i < 2; i++) {
		channels[20 + i].base = QCA_DMA_IOVA + 0x9000 + i * 0x1000;
		channels[20 + i].length = 32 * 16;
		channels[20 + i].read = channels[20 + i].write = channels[20 + i].base;
		channels[20 + i].config = 2u << 8 | 1;
		commands[i].pointer = 0;
		commands[i].word0 = 0;
		commands[i].word1 = (20u + i) << 24 | 18u << 16;
	}
	command->write = QCA_DMA_IOVA + 0x4020;
	qca_ipcr_barrier();
	qca_mhi_write64(registers + 0x70, command->write);
	for (unsigned ms = 0; ms < 3000; ms++) {
		if (qca_mhi_failed(rd(registers + 0x48), rd(registers + 0x128)) ||
		    !qca_ipcr_poll(registers, control, receive, &state)) goto failed;
		if (state.started[0] && state.started[1]) break;
		delay_for_interval(1000, NSEC_PER_USEC);
	}
	if (!state.started[0] || !state.started[1]) goto failed;
	for (unsigned i = 0; i < 31; i++) {
		rx[i].pointer = QCA_DMA_IOVA + 0x440000 + 4096 * i;
		rx[i].word0 = 4096;
		rx[i].word1 = 2u << 16 | 1u << 9 | 1u << 10;
	}
	channels[21].write = QCA_DMA_IOVA + 0xa1f0;
	qca_ipcr_barrier();
	qca_mhi_write64(registers + chdb + 21 * 8, channels[21].write);
	/* One real QRTR HELLO; no credentials or wireless scan are involved. */
	uint32_t hello[13] = { 1, 2, 1, 0xfffffffe, 0, 20, 0xffffffff, 0xfffffffe, 2, 0, 0, 0, 0 };
	memcpy((void *)(control + 0xb000), hello, sizeof(hello));
	tx->pointer = QCA_DMA_IOVA + 0xb000;
	tx->word0 = sizeof(hello);
	tx->word1 = 2u << 16 | 1u << 9 | 1u << 10;
	state.tx_submitted = 1;
	channels[20].write = QCA_DMA_IOVA + 0x9010;
	qca_ipcr_barrier();
	qca_mhi_write64(registers + chdb + 20 * 8, channels[20].write);
	tell("pcie: 32: submitted QRTR HELLO; 31 isolated 4096-byte receive buffers\n");
	for (unsigned ms = 0; ms < 5000; ms++) {
		if (qca_mhi_failed(rd(registers + 0x48), rd(registers + 0x128)) ||
		    !qca_ipcr_poll(registers, control, receive, &state)) goto failed;
		if (state.tx_done && state.hello && state.wlfw) break;
		delay_for_interval(1000, NSEC_PER_USEC);
	}
	if (!state.tx_done || !state.hello || !state.wlfw) goto failed;
	struct qmi_wlanfw_ind_register_req_msg_v01 indications = { 0 };
	indications.client_id_valid = 1; indications.client_id = QMI_WLANFW_CLIENT_ID;
	indications.fw_ready_enable_valid = indications.fw_ready_enable = 1;
	indications.cal_done_enable_valid = indications.cal_done_enable = 1;
	indications.fw_init_done_enable_valid = indications.fw_init_done_enable = 1;
	indications.request_mem_enable_valid = indications.request_mem_enable = 1;
	indications.fw_mem_ready_enable_valid = indications.fw_mem_ready_enable = 1;
	if (!qca_qmi_ask(registers, control, receive, &state, QMI_WLANFW_IND_REGISTER_REQ_V01,
	    qmi_wlanfw_ind_register_req_msg_v01_ei, &indications)) goto failed;
	struct qmi_wlanfw_host_cap_req_msg_v01 host = { 0 };
	host.num_clients_valid = 1; host.num_clients = 1;
	host.mem_cfg_mode_valid = 1; host.mem_cfg_mode = 0;
	host.bdf_support_valid = host.bdf_support = 1;
	host.m3_support_valid = host.m3_support = 1;
	host.m3_cache_support_valid = host.m3_cache_support = 1;
	host.cal_done_valid = 1; host.cal_done = 0;
	if (!qca_qmi_ask(registers, control, receive, &state, QMI_WLANFW_HOST_CAP_REQ_V01,
	    qmi_wlanfw_host_cap_req_msg_v01_ei, &host)) goto failed;
	if (!qca_qmi_wait(registers, control, receive, &state, &state.memory_request)) goto failed;
	struct qmi_wlanfw_respond_mem_req_msg_v01 memory = { 0 };
	uint32_t m3_offset = 0;
	if (!runtime || !qca_qmi_memory_plan(&state.memory, &memory, &m3_offset)) goto failed;
	for (unsigned i = 0; i < memory.mem_seg_len; i++) {
		tell("pcie: 34: isolated QMI memory segment %u type %u IOVA %#llx size %#x\n", i,
		    (unsigned)memory.mem_seg[i].type, (unsigned long long)memory.mem_seg[i].addr, memory.mem_seg[i].size);
	}
	qca_ipcr_barrier();
	state.memory_supplied = true;
	if (!qca_qmi_ask(registers, control, receive, &state, QMI_WLANFW_RESPOND_MEM_REQ_V01,
	    qmi_wlanfw_respond_mem_req_msg_v01_ei, &memory) ||
	    !qca_qmi_wait(registers, control, receive, &state, &state.memory_ready)) goto failed;
	struct qmi_wlanfw_cap_req_msg_v01 what = { 0 };
	if (!qca_qmi_ask(registers, control, receive, &state, QMI_WLANFW_CAP_REQ_V01,
	    qmi_wlanfw_cap_req_msg_v01_ei, &what)) goto failed;
	tell("pcie: 33: QMI capability chip %#x family %#x valid %u, board %#x valid %u, soc %#x valid %u\n",
	    state.target.chip_info.chip_id, state.target.chip_info.chip_family, state.target.chip_info_valid,
	    state.target.board_info.board_id, state.target.board_info_valid, state.target.soc_info.soc_id, state.target.soc_info_valid);
	tell("pcie: 33: QMI firmware %#x timestamp %.32s valid %u, build %.128s valid %u\n",
	    state.target.fw_version_info.fw_version, state.target.fw_version_info.fw_build_timestamp,
	    state.target.fw_version_info_valid, state.target.fw_build_id, state.target.fw_build_id_valid);
	state.regdb = qca_qmi_file(registers, control, receive, &state, "qca6390/regdb.bin", 19348, ATH11K_QMI_BDF_TYPE_REGDB);
	if (!state.regdb) goto failed;
	state.board = qca_qmi_file(registers, control, receive, &state, "qca6390/bd_k11a.elf", 57896, ATH11K_QMI_BDF_TYPE_ELF);
	if (!state.board) goto failed;
	uint64_t m3_length = 0;
	const uint8_t *m3_file = fw_find("m3.bin", &m3_length);
	if (!m3_file || m3_length != QCA_M3_SIZE || m3_offset < QCA_RUNTIME_OFFSET || m3_offset > QCA_DMA_SIZE - QCA_M3_SIZE) goto failed;
	memcpy((void *)(runtime + m3_offset - QCA_RUNTIME_OFFSET), m3_file, QCA_M3_SIZE);
	qca_ipcr_barrier();
	struct qmi_wlanfw_m3_info_req_msg_v01 m3 = { .addr = QCA_DMA_IOVA + m3_offset, .size = QCA_M3_SIZE };
	if (!qca_qmi_ask(registers, control, receive, &state, QMI_WLANFW_M3_INFO_REQ_V01,
	    qmi_wlanfw_m3_info_req_msg_v01_ei, &m3) ||
	    !qca_qmi_wait(registers, control, receive, &state, &state.init_done)) goto failed;
	goto done;
failed:
	state.failed = true;
done:
	status = rd(registers + 0x48);
	ee = rd(registers + 0x128);
	tell("pcie: 32: IPCR summary starts %u/%u, TX %u, packets %u, HELLO %u, WLFW %u, failed %u, MHI %#x EE %#x\n",
	    state.started[0], state.started[1], state.tx_done, state.packets, state.hello, state.wlfw, state.failed, status, ee);
	tell("pcie: 33: QMI summary capability %u registered %u host %u memory %u segments %u, failed %u, MHI %#x EE %#x\n",
	    state.capability, state.registered, state.host_cap, state.memory_request, state.memory.mem_seg_len, state.failed, status, ee);
	tell("pcie: 34: QMI initialization supplied %u accepted %u memory_ready %u regdb %u board %u m3 %u init_done %u resumes %u failed %u, MHI %#x EE %#x\n",
	    state.memory_supplied, state.memory_accepted, state.memory_ready, state.regdb, state.board, state.m3, state.init_done,
	    state.resume_count, state.failed, status, ee);
	return !state.failed && state.memory_ready && state.board && state.m3 && state.init_done && !qca_mhi_failed(status, ee);
}

/* K40 MHI backend for the existing QRTR router. One worker owns all rings.
 * The send callback only queues copies, including sends made by RX callbacks.
 * Wire/ring definitions follow the ISC/BSD OpenBSD qwx reference; no GPL code.
 */
#define QCA_LINK_MAX 8480u
#define QCA_LINK_QUEUE 32u
#define QCA_LINK_TX_OFFSET 0x480000u
#define QCA_LINK_TX_STRIDE 0x3000u

static LCK_GRP_DECLARE(qca_link_group, "k40-mhi");
static LCK_MTX_DECLARE(qca_link_lock, &qca_link_group);
static LCK_MTX_DECLARE(qca_window_lock, &qca_link_group);
static struct {
	vm_offset_t registers, control, receive, transmit;
	unsigned chdb, erdb, cursor[3], returned[3];
	unsigned rx_next, rx_post, tx_head, tx_tail, tx_pending;
	unsigned queue_head, queue_tail, queue_count, assembled;
	unsigned sent, heard, fragments, polls, wraps;
	unsigned diagnostic_events;
	bool started[2], wanted, running, failed, router;
	uint8_t packet[QCA_LINK_MAX];
	struct { unsigned length; uint8_t data[QCA_LINK_MAX]; } queue[QCA_LINK_QUEUE];
} qca_link;

static bool
qca_qrtr_size(const uint8_t *data, unsigned length, unsigned *total)
{
	uint32_t size, node, port, at;
	if (length >= 32 && data[0] == 1 && data[1] == 0 && data[2] == 0 && data[3] == 0) {
		memcpy(&size, data + 20, 4);
		memcpy(&node, data + 24, 4);
		memcpy(&port, data + 28, 4);
		at = 32;
	} else if (length >= 16 && data[0] == 3) {
		memcpy(&size, data + 4, 4);
		node = data[12] | (unsigned)data[13] << 8;
		port = data[14] | (unsigned)data[15] << 8;
		at = 16u + data[3];
	} else return false;
	if (!size || size > 8192 || at > QCA_LINK_MAX || size > QCA_LINK_MAX - at ||
	    (node != 1 && node != 0xffffffffu && node != 0xffffu) || port == 0) return false;
	*total = at + size;
	return true;
}

static int
qca_link_send(const void *data, unsigned length)
{
	if (length < 16 || length > QCA_LINK_MAX) return EMSGSIZE;
	lck_mtx_lock(&qca_link_lock);
	if (!qca_link.wanted || qca_link.failed) {
		lck_mtx_unlock(&qca_link_lock);
		return ENOTCONN;
	}
	if (qca_link.queue_count == QCA_LINK_QUEUE) {
		lck_mtx_unlock(&qca_link_lock);
		return ENOBUFS;
	}
	unsigned tail = qca_link.queue_tail;
	memcpy(qca_link.queue[tail].data, data, length);
	qca_link.queue[tail].length = length;
	qca_link.queue_tail = (tail + 1) % QCA_LINK_QUEUE;
	qca_link.queue_count++;
	lck_mtx_unlock(&qca_link_lock);
	return 0;
}

static bool
qca_link_receive(unsigned slot, unsigned length, unsigned code)
{
	unsigned expected;
	if (slot != qca_link.rx_next || length == 0 || length > 4096 ||
	    (code != 2 && code != 3 && code != 4) ||
	    length > QCA_LINK_MAX - qca_link.assembled) return false;
	qca_ipcr_barrier();
	memcpy(qca_link.packet + qca_link.assembled, (void *)(qca_link.receive + slot * 4096), length);
	qca_link.assembled += length;
	if (!qca_qrtr_size(qca_link.packet, qca_link.assembled, &expected)) return false;
	if (qca_link.assembled >= expected) {
		if (code == 3 || qca_link.assembled - expected > 3) return false;
		/* QRTR alignment bytes are outside its payload and unspecified. */
		hakios_qrtr_external_input(qca_link.packet, qca_link.assembled);
		qca_link.heard++;
		qca_link.assembled = 0;
	} else {
		/* A full inbound TRE may end before the QRTR datagram does. */
		if (length != 4096) return false;
		qca_link.fragments++;
	}
	volatile struct qca_mhi_context *channels = (void *)qca_link.control;
	struct qca_mhi_tre *rx = (void *)(qca_link.control + 0xa000);
	unsigned tail = qca_link.rx_post;
	rx[tail].pointer = QCA_DMA_IOVA + 0x440000 + tail * 4096;
	rx[tail].word0 = 4096;
	rx[tail].word1 = 2u << 16 | 1u << 8 | 1u << 9 | 1u << 10;
	qca_link.rx_next = (slot + 1) % 32;
	qca_link.rx_post = (tail + 1) % 32;
	channels[21].write = QCA_DMA_IOVA + 0xa000 + qca_link.rx_post * 16;
	qca_ipcr_barrier();
	qca_mhi_write64(qca_link.registers + qca_link.chdb + 21 * 8, channels[21].write);
	return true;
}

static bool
qca_link_events(void)
{
	volatile struct qca_mhi_context *events = (void *)(qca_link.control + 0x2000);
	for (unsigned ring = 0; ring < 3; ring++) {
		unsigned entries = ring == 1 ? 256 : 32, consumed = 0;
		uint64_t base = QCA_DMA_IOVA + 0x5000 + ring * 0x1000, device = events[ring].read;
		qca_ipcr_barrier();
		if (device < base || device >= base + entries * 16 || ((device - base) & 15)) return false;
		unsigned limit = (unsigned)(device - base) / 16;
		while (qca_link.cursor[ring] != limit && consumed++ < entries) {
			volatile struct qca_mhi_tre *event = (void *)(qca_link.control + 0x5000 + ring * 0x1000 + qca_link.cursor[ring] * 16);
			uint64_t pointer = event->pointer;
			uint32_t word0 = event->word0, word1 = event->word1;
			unsigned type = (word1 >> 16) & 255, channel = word1 >> 24, code = word0 >> 24;
			if (qca_link.diagnostic_events++ < 64) {
				tell("pcie: 35: event ring %u type %#x channel %u code %u pointer %#llx length %u tx_head %u pending %u rx_next %u\n",
				    ring, type, channel, code, pointer, word0 & 0xffff,
				    qca_link.tx_head, qca_link.tx_pending, qca_link.rx_next);
			}
			if (type == 0x21) {
				if (ring != 0 || code != 1 || pointer < QCA_DMA_IOVA + 0x4000 ||
				    pointer >= QCA_DMA_IOVA + 0x4020 || (pointer & 15)) return false;
				unsigned slot = (unsigned)(pointer - QCA_DMA_IOVA - 0x4000) / 16;
				if (qca_link.started[slot]) return false;
				qca_link.started[slot] = true;
			} else if (type == 0x22) {
				if (ring != 1) return false;
				if (channel == 20) {
					/* Every submission requests EOT, so each completion must be next. */
					if (code != 2 || !qca_link.tx_pending ||
					    pointer != QCA_DMA_IOVA + 0x9000 + qca_link.tx_head * 16) return false;
					qca_link.tx_head = (qca_link.tx_head + 1) % 32;
					qca_link.tx_pending--;
				} else if (channel == 21) {
					if (!qca_link.started[1] || pointer < QCA_DMA_IOVA + 0xa000 ||
					    pointer >= QCA_DMA_IOVA + 0xa200 || (pointer & 15) ||
					    !qca_link_receive((unsigned)(pointer - QCA_DMA_IOVA - 0xa000) / 16, word0 & 0xffff, code)) return false;
				} else return false;
			} else if (type == 0x20) {
				if (code == 0xff) return false;
			} else if (type == 0x40) {
				if (code == 3) return false;
			} else return false;
			qca_link.cursor[ring] = (qca_link.cursor[ring] + 1) % entries;
			qca_link.returned[ring] = (qca_link.returned[ring] + 1) % entries;
		}
		if (qca_link.cursor[ring] != limit) return false;
		if (consumed) {
			events[ring].write = base + qca_link.returned[ring] * 16;
			qca_ipcr_barrier();
			qca_mhi_write64(qca_link.registers + qca_link.erdb + ring * 8, events[ring].write);
		}
	}
	return true;
}

static void
qca_link_transmit(void)
{
	volatile struct qca_mhi_context *channels = (void *)qca_link.control;
	struct qca_mhi_tre *tx = (void *)(qca_link.control + 0x9000);
	while (qca_link.tx_pending < 31) {
		lck_mtx_lock(&qca_link_lock);
		if (!qca_link.queue_count || !qca_link.wanted) {
			lck_mtx_unlock(&qca_link_lock);
			break;
		}
		unsigned head = qca_link.queue_head, slot = qca_link.tx_tail;
		unsigned length = qca_link.queue[head].length;
		memcpy((void *)(qca_link.transmit + slot * QCA_LINK_TX_STRIDE), qca_link.queue[head].data, length);
		qca_link.queue_head = (head + 1) % QCA_LINK_QUEUE;
		qca_link.queue_count--;
		lck_mtx_unlock(&qca_link_lock);
		tx[slot].pointer = QCA_DMA_IOVA + QCA_LINK_TX_OFFSET + slot * QCA_LINK_TX_STRIDE;
		tx[slot].word0 = length;
		tx[slot].word1 = 2u << 16 | 1u << 9 | 1u << 10;
		qca_link.tx_tail = (slot + 1) % 32;
		qca_link.tx_pending++;
		qca_link.sent++;
		qca_link.wraps += qca_link.tx_tail == 0;
		channels[20].write = QCA_DMA_IOVA + 0x9000 + qca_link.tx_tail * 16;
		qca_ipcr_barrier();
		qca_mhi_write64(qca_link.registers + qca_link.chdb + 20 * 8, channels[20].write);
	}
}

static void
qca_link_worker(__unused void *argument, __unused wait_result_t result)
{
	int error = hakios_qrtr_external_start(qca_link_send);
	lck_mtx_lock(&qca_link_lock);
	qca_link.router = error == 0;
	qca_link.failed = error != 0;
	lck_mtx_unlock(&qca_link_lock);
	for (;;) {
		lck_mtx_lock(&qca_link_lock);
		bool wanted = qca_link.wanted && !qca_link.failed;
		lck_mtx_unlock(&qca_link_lock);
		if (!wanted) break;
		if (qca_mhi_failed(rd(qca_link.registers + 0x48), rd(qca_link.registers + 0x128)) || !qca_link_events()) {
			lck_mtx_lock(&qca_link_lock);
			qca_link.failed = true;
			lck_mtx_unlock(&qca_link_lock);
			tell("pcie: 35: MHI worker stopped on invalid state or completion; status %#x EE %#x assembled %u\n",
			    rd(qca_link.registers + 0x48), rd(qca_link.registers + 0x128), qca_link.assembled);
			break;
		}
		hakios_qrtr_external_poll();
		qca_link_transmit();
		qca_link.polls++;
		delay_for_interval(1000, NSEC_PER_USEC);
	}
	if (qca_link.router) hakios_qrtr_external_stop();
	tell("pcie: 35: MHI worker end: sent %u received %u fragments %u wraps %u failed %u\n",
	    qca_link.sent, qca_link.heard, qca_link.fragments, qca_link.wraps, qca_link.failed);
	lck_mtx_lock(&qca_link_lock);
	qca_link.running = false;
	lck_mtx_unlock(&qca_link_lock);
	thread_terminate(current_thread());
}

static bool
qca_link_start(vm_offset_t registers, vm_offset_t control, vm_offset_t receive, vm_offset_t transmit)
{
	bzero(&qca_link, sizeof(qca_link));
	qca_link.registers = registers;
	qca_link.control = control;
	qca_link.receive = receive;
	qca_link.transmit = transmit;
	qca_link.chdb = rd(registers + 0x18);
	qca_link.erdb = rd(registers + 0x20);
	if (qca_link.chdb < 0x100 || (qca_link.chdb & 7) || qca_link.chdb > 0xfe0 - 128 * 8 ||
	    qca_link.erdb < 0x100 || (qca_link.erdb & 7) || qca_link.erdb > 0xfe0 - 24) return false;
	qca_link.returned[0] = qca_link.returned[2] = 31;
	qca_link.returned[1] = 255;
	volatile struct qca_mhi_context *channels = (void *)control, *command = (void *)(control + 0x3000);
	struct qca_mhi_tre *commands = (void *)(control + 0x4000), *rx = (void *)(control + 0xa000);
	for (unsigned i = 0; i < 2; i++) {
		channels[20 + i].base = QCA_DMA_IOVA + 0x9000 + i * 0x1000;
		channels[20 + i].length = 32 * 16;
		channels[20 + i].read = channels[20 + i].write = channels[20 + i].base;
		channels[20 + i].config = 2u << 8 | 1;
		commands[i].pointer = 0;
		commands[i].word0 = 0;
		commands[i].word1 = (20u + i) << 24 | 18u << 16;
	}
	command->write = QCA_DMA_IOVA + 0x4020;
	qca_ipcr_barrier();
	qca_mhi_write64(registers + 0x70, command->write);
	for (unsigned ms = 0; ms < 3000; ms++) {
		if (qca_mhi_failed(rd(registers + 0x48), rd(registers + 0x128)) || !qca_link_events()) return false;
		if (qca_link.started[0] && qca_link.started[1]) break;
		delay_for_interval(1000, NSEC_PER_USEC);
	}
	if (!qca_link.started[0] || !qca_link.started[1]) return false;
	for (unsigned i = 0; i < 31; i++) {
		rx[i].pointer = QCA_DMA_IOVA + 0x440000 + i * 4096;
		rx[i].word0 = 4096;
		rx[i].word1 = 2u << 16 | 1u << 8 | 1u << 9 | 1u << 10;
	}
	qca_link.rx_post = 31;
	channels[21].write = QCA_DMA_IOVA + 0xa1f0;
	qca_ipcr_barrier();
	qca_mhi_write64(registers + qca_link.chdb + 21 * 8, channels[21].write);
	qca_link.wanted = qca_link.running = true;
	thread_t thread;
	if (kernel_thread_start(qca_link_worker, NULL, &thread) != KERN_SUCCESS) {
		qca_link.wanted = qca_link.running = false;
		return false;
	}
	thread_deallocate(thread);
	for (unsigned ms = 0; ms < 1000; ms++) {
		lck_mtx_lock(&qca_link_lock);
		bool ready = qca_link.router, failed = qca_link.failed;
		lck_mtx_unlock(&qca_link_lock);
		if (ready || failed) return ready && !failed;
		delay_for_interval(1000, NSEC_PER_USEC);
	}
	return false;
}

static void
qca_link_stop(void)
{
	lck_mtx_lock(&qca_link_lock);
	qca_link.wanted = false;
	while (qca_link.running) {
		lck_mtx_unlock(&qca_link_lock);
		delay_for_interval(1000, NSEC_PER_USEC);
		lck_mtx_lock(&qca_link_lock);
	}
	lck_mtx_unlock(&qca_link_lock);
}


int
hakios_pcie_radio_up(void)
{
	lck_mtx_lock(&qca_link_lock);
	bool ready = qca_dma.active && qca_link.running && qca_link.router && !qca_link.failed;
	lck_mtx_unlock(&qca_link_lock);
	return ready;
}

static uint32_t
qca_register_window(uint32_t offset)
{
	return offset < 0x80000 ? offset : 0x80000 + (offset & 0x7ffff);
}

unsigned
lx_os_pcie_read32(unsigned offset)
{
	if ((offset & 3) || offset >= 0x2000000 || !hakios_pcie_radio_up()) return ~0u;
	lck_mtx_lock(&qca_window_lock);
	if (offset >= 0x80000) {
		wr(qca_link.registers + 0x310c, 0x40000000u | (offset >> 19));
		(void)rd(qca_link.registers + 0x310c);
	}
	unsigned value = rd(qca_link.registers + qca_register_window(offset));
	lck_mtx_unlock(&qca_window_lock);
	return value;
}

void
lx_os_pcie_write32(unsigned offset, unsigned value)
{
	if ((offset & 3) || offset >= 0x2000000 || !hakios_pcie_radio_up()) return;
	lck_mtx_lock(&qca_window_lock);
	if (offset >= 0x80000) {
		wr(qca_link.registers + 0x310c, 0x40000000u | (offset >> 19));
		(void)rd(qca_link.registers + 0x310c);
	}
	qca_ipcr_barrier();
	wr(qca_link.registers + qca_register_window(offset), value);
	lck_mtx_unlock(&qca_window_lock);
}

/* Normal return still has threads: detach the station before stopping DMA.
 * Panic/last-core quiesce continues to use the lock-free immediate reset hook. */
void
hakios_pcie_stop(void)
{
	if (!qca_dma.active) return;
	hakios_lx_stop();
	qca_link_stop();
	if (qca_link.registers) {
		tell("pcie: 36: station stopped; final MHI %#x EE %#x\n",
		    rd(qca_link.registers + 0x48), rd(qca_link.registers + 0x128));
	}
	tell("pcie: 30: SMMU context fsr %#x, far %#x:%#x, global fsr %#x, syndrome %#x\n",
	    rd(qca_dma.cb + 0x58), rd(qca_dma.cb + 0x64), rd(qca_dma.cb + 0x60),
	    rd(qca_dma.smmu + 0x48), rd(qca_dma.smmu + 0x54));
	hakios_pcie_quiesce();
}

static void
step_endpoint_sbl(bool full)
{
	uint64_t length = 0;
	const uint8_t *firmware;
	vm_offset_t registers, conf, tables = 0, data = 0, control = 0, receive = 0, runtime = 0;
	uint32_t bhi, ee, status = 0, ipcr = 0;
	PE_parse_boot_argn("pcie_ipcr", &ipcr, sizeof(ipcr));

	conf = pcie_io_map(CONF_PHYS, CONF_SIZE);
	if (!link_up_now() || conf == 0 || rd(conf) != QCA6390_ID ||
	    rd(conf + 0x10) != (QCA6390_BAR0_PHYS | 4)) {
		tell("pcie: 30: verified QCA6390 BAR unavailable; no firmware transfer\n");
		return;
	}
	registers = pcie_io_map(QCA6390_BAR0_PHYS, QCA6390_BAR0_SIZE);
	if (registers == 0) {
		return;
	}
	bhi = rd(registers + 0x28);
	if (bhi != 0x100 || rd(registers + bhi + 0x28) != 0) {
		tell("pcie: 30: endpoint is not in the verified PBL state; no firmware transfer\n");
		return;
	}
	if (full && !qca_boot_reset(registers, conf)) {
		tell("pcie: 31: endpoint reset failed; no firmware DMA\n");
		return;
	}
	firmware = fw_find("amss20.bin", &length);
	if (!firmware || length != 4352144 || memcmp(firmware, "\177ELF", 4) != 0) {
		tell("pcie: 30: pinned device firmware absent; no DMA\n");
		return;
	}
	if (kmem_alloc_contig(kernel_map, &tables, QCA_TABLE_SIZE, 0, 0, 0,
	    KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS ||
	    kmem_alloc_contig(kernel_map, &data, QCA_DMA_SIZE, 0, 0, 0,
	    KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		tell("pcie: 30: contiguous SBL memory unavailable; no DMA\n");
		return;
	}
	memcpy((void *)(data + QCA_FW_OFFSET), firmware, QCA_AMSS_SIZE);
	qca_dma_tables((uint64_t *)tables, kvtophys(tables), kvtophys(data));
	qca_dma_sync(data, QCA_DMA_SIZE);
	qca_driver_tables_address = tables;
	if (ipcr == 2 && full) {
		int error = hakios_lx_prepare();
		if (error || !qca_driver_pool_mapped) {
			tell("pcie: 35: driver memory preparation failed: %d\n", error);
			return;
		}
	}
	qca_dma_sync(tables, QCA_TABLE_SIZE);
	control = data;
	if ((ipcr == 1 || ipcr == 2) && full) {
		control = ml_io_map_unmappable(kvtophys(data), QCA_FW_OFFSET, VM_WIMG_WCOMB);
		receive = ml_io_map_unmappable(kvtophys(data) + 0x440000, 0x20000, VM_WIMG_WCOMB);
		runtime = ml_io_map_unmappable(kvtophys(data) + QCA_RUNTIME_OFFSET, QCA_DMA_SIZE - QCA_RUNTIME_OFFSET, VM_WIMG_WCOMB);
		if (!control || !receive || !runtime) {
			tell("pcie: 32: normal-NC views unavailable; no firmware DMA\n");
			return;
		}
		/* The original cached control/RX views are never used again. */
	}
	if (!qca_dma_prepare(kvtophys(tables))) {
		return;
	}
	if (full && !qca_boot_msi(QCA_DMA_IOVA + 0xf000)) {
		hakios_pcie_quiesce();
		return;
	}
	wr16(dbi + 4, qca_dma.old_root_command | 6u);
	wr16(conf + 4, qca_dma.old_command | 6u);
	wr(registers + bhi + 0x20, 0);          /* polled BHI, no MSI */
	if (full && !qca_wait_pbl_reset(registers)) {
		hakios_pcie_quiesce();
		return;
	}
	wr(registers + bhi + 0x2c, 0);
	wr(registers + bhi + 0x0c, 0);
	wr(registers + bhi + 0x08, (uint32_t)(QCA_DMA_IOVA + QCA_FW_OFFSET));
	wr(registers + bhi + 0x10, QCA_SBL_SIZE);
	__asm__ volatile("dsb sy" : : : "memory");
	wr(registers + bhi + 0x18, ((uint32_t)mach_absolute_time() & 0x3fffffff) | 1);
	tell("pcie: 30: requested signed SBL transfer (512 KiB) through BHI\n");
	for (unsigned ms = 0; ms < 10000; ms += 10) {
		delay_for_interval(10000, NSEC_PER_USEC);
		status = rd(registers + bhi + 0x2c);
		ee = rd(registers + bhi + 0x28);
		if ((status >> 30) >= 2 || (ee >= 1 && ee <= 7)) {
			break;
		}
	}
	tell("pcie: 30: BHI status %#x, execenv %#x, MHI status %#x\n",
	    status, rd(registers + bhi + 0x28), rd(registers + 0x48));
	tell("pcie: 31: MSI pending %#x, BHI error %#x, debug %#x/%#x/%#x\n",
	    rd(dbi + 0x830), rd(registers + bhi + 0x30), rd(registers + bhi + 0x34),
	    rd(registers + bhi + 0x38), rd(registers + bhi + 0x3c));
	if (full && (status >> 30) == 2) {
		if (step_endpoint_amss(registers, control)) {
			if (ipcr == 1) qca_ipcr_probe(registers, control, receive, runtime);
			if (ipcr == 2) {
				if (qca_link_start(registers, control, receive, runtime)) {
					tell("pcie: 35: cyclic MHI/QRTR ready; starting QCA6390 ath11k\n");
					int error = hakios_lx_start();
					tell("pcie: 35: ath11k returned %d\n", error);
					uint32_t keep = 0, station = 0;
					PE_parse_boot_argn("wifi_keep", &keep, sizeof(keep));
					PE_parse_boot_argn("wifi_station", &station, sizeof(station));
					if (!error && keep && station && hakios_pcie_radio_up()) {
						tell("pcie: 36: station offered to iOS; keeping isolated endpoint until session return\n");
						return;
					}
					hakios_lx_stop();
				}
				qca_link_stop();
			}
		}
	}
	tell("pcie: 30: SMMU context fsr %#x, far %#x:%#x, global fsr %#x, syndrome %#x\n",
	    rd(qca_dma.cb + 0x58), rd(qca_dma.cb + 0x64), rd(qca_dma.cb + 0x60),
	    rd(qca_dma.smmu + 0x48), rd(qca_dma.smmu + 0x54));
	hakios_pcie_quiesce();
	/* The endpoint is reset and cannot write DMA memory during this dump. */
	if (full) {
		qca_dma_sync(control, QCA_FW_OFFSET);
		struct qca_mhi_context *events = (void *)(control + 0x2000);
		for (unsigned i = 0; i < 3; i++) {
			tell("pcie: 31: stopped event %u context read %#llx write %#llx\n",
			    i, events[i].read, events[i].write);
			const uint32_t *words = (void *)(control + 0x5000 + i * 0x1000);
			for (unsigned n = 0; n < 8; n++) {
				tell("pcie: 31: stopped event %u entry %u: %#x %#x %#x %#x\n",
				    i, n, words[n * 4], words[n * 4 + 1], words[n * 4 + 2], words[n * 4 + 3]);
			}
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
		step_endpoint_mhi();
	}
	if (steps >= 30) {
		step_endpoint_sbl(steps >= 31);
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
