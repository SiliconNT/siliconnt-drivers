/* SPDX-License-Identifier: MIT */
/*
 * The SCSI layer of the miniport: what the one disk answers to the commands Storport,
 * classpnp and disk.sys send (SiliconNT findings/0071 conclusions 10-15).
 *
 * The disk is read-only. Every command that would write to the medium or change the
 * device is refused with DATA PROTECT (sense key 7, ASC 24h) and never reaches the
 * controller; the only command that does is a read, and only the core sends it
 * (ans_read()). Everything else is answered here from what the core found when it
 * started (ans_get_info()).
 *
 * Pure C, no Storport and no hardware: the miniport hands over the CDB and, for
 * commands that return data, the mapped buffer, and acts on the reply. Built into the
 * miniport and into the host test (test/host_test.c).
 */
#ifndef ANS_SCSI_H
#define ANS_SCSI_H

#include "core/ans_env.h"

/* The disk, as the core found it. Strings: ASCII, NUL-terminated (struct ans_info). */
struct ans_scsi_disk {
    uint64_t blocks;
    uint32_t block_size;
    const char *model;
    const char *firmware;
    const char *serial;
};

enum ans_scsi_action {
    ANS_SCSI_COMPLETE = 0, /* answered here: complete the request with the reply */
    ANS_SCSI_READ          /* a read of blocks at lba: hand it to the core */
};

/* SCSI status bytes and sense keys the layer uses. */
#define ANS_SCSI_GOOD            0x00
#define ANS_SCSI_CHECK_CONDITION 0x02
#define ANS_SENSE_ILLEGAL_REQUEST 0x05
#define ANS_SENSE_DATA_PROTECT    0x07

struct ans_scsi_reply {
    enum ans_scsi_action action;
    uint8_t opcode;
    uint8_t status;    /* ANS_SCSI_GOOD or ANS_SCSI_CHECK_CONDITION */
    uint8_t sense_key; /* with CHECK CONDITION: fixed-format sense */
    uint8_t asc;
    uint8_t ascq;
    uint32_t length;   /* COMPLETE: bytes of data put into the buffer */
    bool write_refused; /* a command that writes, refused (sense DATA PROTECT) */
    bool lba_known;     /* READ, or a refused write whose CDB names where it goes */
    uint64_t lba;
    uint32_t blocks;
};

/*
 * Executes one CDB of cdb_len bytes. buf, buf_len: the data buffer and the request's
 * transfer length. For commands that return data buf is where they go (mapped; may be
 * NULL when buf_len is 0); for a read buf is not used (the data goes straight from the
 * disk to the request's memory) and buf_len must be blocks x block size.
 */
void ans_scsi_execute(const struct ans_scsi_disk *disk, const uint8_t *cdb, uint32_t cdb_len,
                      uint8_t *buf, uint32_t buf_len, struct ans_scsi_reply *reply);

/*
 * Writes the fixed-format sense data (response code 70h) for a CHECK CONDITION reply
 * into sense, at most len bytes. Returns the bytes written: 18, or fewer when len is
 * smaller.
 */
uint32_t ans_scsi_sense(const struct ans_scsi_reply *reply, uint8_t *sense, uint32_t len);

/* Fills reply as a CHECK CONDITION with the given sense (also for the miniport's own use). */
void ans_scsi_check(struct ans_scsi_reply *reply, uint8_t key, uint8_t asc, uint8_t ascq);

#endif
