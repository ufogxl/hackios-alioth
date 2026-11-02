/*
 * RPMh: the command database, and the register writes of one request. Ported from UniXNU-sheng's
 * package/QcomRpmh.c (BSD-2-Clause-Patent); the algorithm is theirs, the commentary is about where
 * each number comes from on this board.
 *
 * The command database is a small table the AOP leaves in memory (this board: 0x80860000, 0x20000
 * bytes, magic 0x0c0330db) that maps a resource's eight-character name -- "smpa6" for pm8150_s6,
 * "ldoa5" for the PCIe PHY's 0.9 V rail -- to the 32-bit command word a TCS has to be given, plus,
 * for the resources that have them, a table of levels. The caller copies it out of device memory
 * first: the database is mapped as device memory and byte accesses to it are not what the port that
 * works does either.
 */
#include <sys/types.h>
#include <libkern/libkern.h>

#include "hakios.rpmh.h"

static uint16_t
le16(const uint8_t *p)
{
	return (uint16_t)(p[0] | (uint16_t)p[1] << 8);
}

static uint32_t
le32(const uint8_t *p)
{
	return (uint32_t)le16(p) | (uint32_t)le16(p + 2) << 16;
}

static int
fits(size_t at, size_t length, size_t size)
{
	return at <= size && length <= size - at;
}

/*
 * Header: u32 version, u32 magic, 8 slave records of 16 bytes (u16 id, header offset, data offset,
 * entry count, version, reserved[3]), u32 checksum, u32 reserved, then the data area the offsets
 * count from (byte 144). Entries are 24 bytes: an 8-byte id, two u32 priorities, u32 address,
 * u16 aux length, u16 aux offset.
 */
int
hakios_cmd_db_find(const uint8_t *db, size_t bytes, const char *name, struct hakios_rpmh_resource *out)
{
	char id[8] = { 0 };

	if (db == 0 || name == 0 || out == 0) {
		return hakios_rpmh_argument;
	}
	if (bytes < 144 || le32(db + 4) != HAKIOS_CMD_DB_MAGIC) {
		return hakios_rpmh_no_database;
	}
	for (unsigned i = 0; i < 8 && name[i] != 0; i++) {
		id[i] = name[i];
	}
	for (unsigned s = 0; s < 8; s++) {
		const uint8_t *slave = db + 8 + 16 * s;
		size_t heads, data, count;

		if (le16(slave) == 0) {
			break;                  /* (the slave records are terminated by an empty one) */
		}
		heads = 144u + le16(slave + 2);
		data = 144u + le16(slave + 4);
		count = le16(slave + 6);
		if (!fits(heads, count * 24, bytes)) {
			return hakios_rpmh_corrupt;
		}
		for (size_t e = 0; e < count; e++) {
			const uint8_t *entry = db + heads + 24 * e;
			unsigned same = 1;

			for (unsigned i = 0; i < 8 && same; i++) {
				same = entry[i] == (uint8_t)id[i];
			}
			if (!same) {
				continue;
			}
			out->address = le32(entry + 16);
			out->level_count = 0;
			{
				size_t length = le16(entry + 20), at = data + le16(entry + 22);

				if (out->address == 0 || !fits(at, length, bytes)) {
					return hakios_rpmh_corrupt;
				}
				for (size_t l = 0; l + 1 < length && out->level_count < HAKIOS_RPMH_MAX_LEVELS; l += 2) {
					uint16_t level = le16(db + at + l);

					if (out->level_count != 0 && level == 0) {
						break;  /* (the list is zero padded) */
					}
					out->levels[out->level_count++] = level;
				}
			}
			return hakios_rpmh_ok;
		}
	}
	return hakios_rpmh_missing;
}

/*
 * Every entry, so that a name can be found when it is not the one the device tree suggests: the
 * database is the authority on what a resource is called, and this board's pm8009 is not "smpf2".
 * The visit callback gets the eight characters with the padding cut off.
 */
void
hakios_cmd_db_each(const uint8_t *db, size_t bytes,
    void (*visit)(void *context, const char *name, uint32_t address, unsigned levels), void *context)
{
	if (bytes < 144 || le32(db + 4) != HAKIOS_CMD_DB_MAGIC) {
		return;
	}
	for (unsigned s = 0; s < 8; s++) {
		const uint8_t *slave = db + 8 + 16 * s;
		size_t heads, data, count;

		if (le16(slave) == 0) {
			break;
		}
		heads = 144u + le16(slave + 2);
		data = 144u + le16(slave + 4);
		count = le16(slave + 6);
		if (!fits(heads, count * 24, bytes)) {
			return;
		}
		for (size_t e = 0; e < count; e++) {
			const uint8_t *entry = db + heads + 24 * e;
			char name[9] = { 0 };
			uint32_t address = le32(entry + 16);
			size_t length = le16(entry + 20), at = data + le16(entry + 22);
			unsigned levels = 0;

			if (address == 0 || !fits(at, length, bytes)) {
				continue;
			}
			for (unsigned i = 0; i < 8; i++) {
				name[i] = (char)entry[i];
			}
			for (size_t l = 0; l + 1 < length && levels < HAKIOS_RPMH_MAX_LEVELS; l += 2) {
				if (levels != 0 && le16(db + at + l) == 0) {
					break;
				}
				levels++;
			}
			visit(context, name, address, levels);
		}
	}
}

int
hakios_rpmh_level_index(const struct hakios_rpmh_resource *r, uint32_t level, uint32_t *index)
{
	if (r->level_count == 0) {
		return hakios_rpmh_no_level;
	}
	if (level == 0) {
		*index = 0;                     /* (0 turns the vote off) */
		return hakios_rpmh_ok;
	}
	for (unsigned i = 0; i < r->level_count; i++) {
		if (r->levels[i] >= level) {
			*index = i;
			return hakios_rpmh_ok;
		}
	}
	*index = r->level_count - 1;
	return hakios_rpmh_ok;
}

/*
 * Two layouts, and the reason this is not one hardcoded set of offsets: the RSC's major version 3
 * moved them. Bit by bit the old one is k40-ref's rpmh-rsc.c:33-58 (tcs offset 672, command offset
 * 20, wait-for-completion 0x10, control 0x14, status 0x18, command enable 0x1c, msgid 0x30), which
 * is this board's vendor kernel and therefore this board's layout.
 */
struct hakios_rsc_layout
hakios_rsc_layout_for(uint32_t version)
{
	if (((version >> 16) & 0xff) >= 3) {
		struct hakios_rsc_layout v3 = {
			672, 24, 0x04, 0x08, 0x20, 0x24, 0x28, 0x2c, 0x34, 0x38, 0x3c
		};

		return v3;
	}
	{
		struct hakios_rsc_layout v2 = {
			672, 20, 0x04, 0x08, 0x10, 0x14, 0x18, 0x1c, 0x30, 0x34, 0x38
		};

		return v2;
	}
}

#define MSGID_WRITE_RESPONSE    (8u | 1u << 8 | 1u << 16)  /* (4-byte payload, response asked, a write) */
#define CONTROL_ACTIVE_MODE     (1u << 16)
#define CONTROL_TRIGGER         (1u << 24)

/*
 * One request, in the order the working port writes it: the control register is cleared before the
 * TCS is reprogrammed, the TCS's own interrupt is cleared, each command goes in as msgid then
 * address then data, and only then do the completion mask, the command enables, the active mode and
 * the trigger follow -- the trigger as a separate write after the mode, never together with it.
 */
unsigned
hakios_rpmh_active_request(const struct hakios_rsc_layout *l, unsigned tcs, const uint32_t *addresses,
    const uint32_t *values, unsigned count, struct hakios_rpmh_write *w, unsigned capacity)
{
	uint32_t base, mask;
	unsigned n = 0;

	if (count == 0 || count > 4 || capacity < 3 * count + 6) {
		return 0;
	}
	base = l->tcs_stride * tcs;
	w[n].offset = base + l->control;
	w[n++].value = 0;
	w[n].offset = l->irq_clear;
	w[n++].value = 1u << tcs;
	for (unsigned c = 0; c < count; c++) {
		uint32_t cmd = base + l->cmd_stride * c;

		w[n].offset = cmd + l->cmd_msgid;
		w[n++].value = MSGID_WRITE_RESPONSE;
		w[n].offset = cmd + l->cmd_addr;
		w[n++].value = addresses[c];
		w[n].offset = cmd + l->cmd_data;
		w[n++].value = values[c];
	}
	mask = (1u << count) - 1;
	w[n].offset = base + l->wait_for_completion;
	w[n++].value = mask;
	w[n].offset = base + l->cmd_enable;
	w[n++].value = mask;
	w[n].offset = base + l->control;
	w[n++].value = CONTROL_ACTIVE_MODE;
	w[n].offset = base + l->control;
	w[n++].value = CONTROL_ACTIVE_MODE | CONTROL_TRIGGER;
	return n;
}

unsigned
hakios_rpmh_finish(const struct hakios_rsc_layout *l, unsigned tcs, struct hakios_rpmh_write *w, unsigned capacity)
{
	uint32_t base;

	if (capacity < 3) {
		return 0;
	}
	base = l->tcs_stride * tcs;
	w[0].offset = l->irq_clear;
	w[0].value = 1u << tcs;
	w[1].offset = base + l->control;
	w[1].value = 0;
	w[2].offset = base + l->cmd_enable;
	w[2].value = 0;
	return 3;
}
