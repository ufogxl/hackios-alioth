/*
 * RPMh, as far as this port needs it: find a resource in the AOP's command database, and turn what
 * a request for it is into the register writes of one TCS in the application processor's RSC.
 *
 * The database and the writes are from UniXNU-sheng's package/QcomRpmh.{c,h} (BSD-2-Clause), which
 * is the port that powers the WCN7850's six rails and gets the link to L0; the TCS offsets are the
 * ones this board's own vendor kernel uses (k40-ref, drivers/soc/qcom/rpmh-rsc.c:33-58, and the
 * device tree's rsc@18200000: qcom,tcs-offset 0xd00, drv-id 2).
 *
 * Nothing here touches hardware: the caller maps the windows, applies the writes and waits.
 */
#ifndef _HAKIOS_RPMH_H_
#define _HAKIOS_RPMH_H_

#include <sys/types.h>

#define HAKIOS_CMD_DB_MAGIC     0x0c0330dbu
#define HAKIOS_RPMH_MAX_LEVELS  16

/*
 * One entry of the database: the 32-bit command word a TCS is given (bits 19:16 are the
 * accelerator type -- 4 for a VRM regulator -- and are part of what the TCS must carry), and, for
 * the resources that have them, the ARC voltage levels.
 */
struct hakios_rpmh_resource {
	uint32_t address;
	uint16_t levels[HAKIOS_RPMH_MAX_LEVELS];
	unsigned level_count;
};

enum {
	hakios_rpmh_ok = 0,
	hakios_rpmh_no_database = -1,   /* (not a command database: the magic is not there) */
	hakios_rpmh_missing = -2,       /* (this database is, but not by that name) */
	hakios_rpmh_corrupt = -3,
	hakios_rpmh_no_level = -4,
	hakios_rpmh_argument = -5,
};

/* Looks name (up to 8 characters) up in a copy of the database; levels are its aux data. */
int hakios_cmd_db_find(const uint8_t *db, size_t bytes, const char *name, struct hakios_rpmh_resource *out);

/* Every entry in a copy of the database, in the order it is stored. */
void hakios_cmd_db_each(const uint8_t *db, size_t bytes,
    void (*visit)(void *context, const char *name, uint32_t address, unsigned levels), void *context);

/* The ARC level index for a request of at least level (the highest index when above all of them). */
int hakios_rpmh_level_index(const struct hakios_rpmh_resource *r, uint32_t level, uint32_t *index);

struct hakios_rpmh_write {
	uint32_t offset;                /* (relative to the mapped DRV) */
	uint32_t value;
};

/* The register layout of one DRV; offsets are relative to the TCS block (DRV base + tcs-offset). */
struct hakios_rsc_layout {
	uint32_t tcs_stride, cmd_stride, irq_status, irq_clear, wait_for_completion, control, status,
	    cmd_enable, cmd_msgid, cmd_addr, cmd_data;
};

/* The layout for a DRV whose version word reads version (the major version is bits 23:16). */
struct hakios_rsc_layout hakios_rsc_layout_for(uint32_t version);

/*
 * The writes of one active request of count (<= 4) commands in TCS tcs, in order, ending with the
 * trigger. Returns how many were written, 0 on bad arguments.
 */
unsigned hakios_rpmh_active_request(const struct hakios_rsc_layout *l, unsigned tcs, const uint32_t *addresses,
    const uint32_t *values, unsigned count, struct hakios_rpmh_write *writes, unsigned capacity);

/* The writes that return a TCS to idle after it is done. */
unsigned hakios_rpmh_finish(const struct hakios_rsc_layout *l, unsigned tcs, struct hakios_rpmh_write *writes,
    unsigned capacity);

#endif /* _HAKIOS_RPMH_H_ */
