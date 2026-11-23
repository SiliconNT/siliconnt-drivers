/* SPDX-License-Identifier: MIT */
/*
 * The SCSI layer; see scsi.h. Which command gets which answer, and why, is in
 * SiliconNT findings/0071 conclusions 10-15; the table in conclusion 15 is the list.
 */

#include "scsi.h"

/* Operation codes (SPC-4, SBC-3). */
#define OP_TEST_UNIT_READY    0x00
#define OP_FORMAT_UNIT        0x04
#define OP_REASSIGN_BLOCKS    0x07
#define OP_WRITE6             0x0a
#define OP_INQUIRY            0x12
#define OP_MODE_SELECT6       0x15
#define OP_MODE_SENSE6        0x1a
#define OP_START_STOP_UNIT    0x1b
#define OP_READ_CAPACITY10    0x25
#define OP_READ10             0x28
#define OP_WRITE10            0x2a
#define OP_WRITE_VERIFY10     0x2e
#define OP_VERIFY10           0x2f
#define OP_SYNCHRONIZE_CACHE  0x35
#define OP_WRITE_BUFFER       0x3b
#define OP_WRITE_LONG10       0x3f
#define OP_WRITE_SAME10       0x41
#define OP_UNMAP              0x42
#define OP_SANITIZE           0x48
#define OP_XDWRITE10          0x50
#define OP_XPWRITE10          0x51
#define OP_XDWRITEREAD10      0x53
#define OP_MODE_SELECT10      0x55
#define OP_MODE_SENSE10       0x5a
#define OP_PERSISTENT_RES_OUT 0x5f
#define OP_THIRD_PARTY_COPY_OUT 0x83
#define OP_READ16             0x88
#define OP_COMPARE_AND_WRITE  0x89
#define OP_WRITE16            0x8a
#define OP_ORWRITE16          0x8b
#define OP_WRITE_VERIFY16     0x8e
#define OP_VERIFY16           0x8f
#define OP_SYNCHRONIZE_CACHE16 0x91
#define OP_WRITE_SAME16       0x93
#define OP_WRITE_STREAM16     0x9a
#define OP_WRITE_ATOMIC16     0x9c
#define OP_SERVICE_ACTION_IN16 0x9e
#define OP_SERVICE_ACTION_OUT16 0x9f
#define OP_REPORT_LUNS        0xa0
#define OP_WRITE12            0xaa
#define OP_WRITE_VERIFY12     0xae
#define OP_SECURITY_PROT_OUT  0xb5

#define SA_READ_CAPACITY16 0x10

/* Additional sense codes. */
#define ASC_INVALID_OPCODE    0x20
#define ASC_LBA_OUT_OF_RANGE  0x21
#define ASC_INVALID_FIELD_CDB 0x24

#define MODE_PAGE_CACHING 0x08
#define MODE_PAGE_ALL     0x3f
#define MODE_WP           0x80 /* device-specific parameter: write protected */
#define CACHING_PAGE_LEN  20   /* page code, page length 12h, 18 bytes */

static uint32_t be16(const uint8_t *p)
{
    return ((uint32_t)p[0] << 8) | p[1];
}

static uint32_t be32(const uint8_t *p)
{
    return (be16(p) << 16) | be16(p + 2);
}

static uint64_t be64(const uint8_t *p)
{
    return ((uint64_t)be32(p) << 32) | be32(p + 4);
}

static void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static void put_be64(uint8_t *p, uint64_t v)
{
    put_be32(p, (uint32_t)(v >> 32));
    put_be32(p + 4, (uint32_t)v);
}

void ans_scsi_check(struct ans_scsi_reply *reply, uint8_t key, uint8_t asc, uint8_t ascq)
{
    reply->action = ANS_SCSI_COMPLETE;
    reply->status = ANS_SCSI_CHECK_CONDITION;
    reply->sense_key = key;
    reply->asc = asc;
    reply->ascq = ascq;
    reply->length = 0;
}

/*
 * Answers with data: copies min(allocation length, data, buffer) bytes. A short
 * transfer is still GOOD; classpnp does not take DATA_OVERRUN for READ CAPACITY
 * (findings/0071 conclusion 10).
 */
static void reply_data(struct ans_scsi_reply *reply, uint8_t *buf, uint32_t buf_len,
                       uint32_t alloc, const uint8_t *data, uint32_t len)
{
    uint32_t n = len;

    if (n > alloc)
        n = alloc;
    if (n > buf_len)
        n = buf_len;
    if (!buf)
        n = 0;
    for (uint32_t i = 0; i < n; i++)
        buf[i] = data[i];
    reply->length = n;
}

/* Copies an identify string into a fixed field, padded with spaces. */
static void put_ascii(uint8_t *field, uint32_t width, const char *s)
{
    uint32_t i = 0;

    for (; s && s[i] && i < width; i++)
        field[i] = (uint8_t)s[i];
    for (; i < width; i++)
        field[i] = ' ';
}

static void inquiry(const struct ans_scsi_disk *disk, const uint8_t *cdb, uint8_t *buf,
                    uint32_t buf_len, struct ans_scsi_reply *reply)
{
    uint8_t data[36];
    uint32_t alloc = be16(cdb + 3);
    uint32_t len;
    uint32_t i;

    for (i = 0; i < sizeof(data); i++)
        data[i] = 0;

    if (!(cdb[1] & 1)) { /* standard data */
        if (cdb[2] != 0) {
            ans_scsi_check(reply, ANS_SENSE_ILLEGAL_REQUEST, ASC_INVALID_FIELD_CDB, 0);
            return;
        }
        data[0] = 0x00; /* direct-access block device */
        data[1] = 0x00; /* not removable */
        data[2] = 0x06; /* SPC-4 */
        data[3] = 0x02; /* response data format 2 */
        data[4] = 36 - 5;
        data[7] = 0x02; /* CmdQue */
        put_ascii(data + 8, 8, "NVMe");
        put_ascii(data + 16, 16, disk->model);
        put_ascii(data + 32, 4, disk->firmware);
        len = 36;
    } else if (cdb[2] == 0x00) { /* supported pages: 00h, 80h */
        data[1] = 0x00;
        data[3] = 2;
        data[4] = 0x00;
        data[5] = 0x80;
        len = 6;
    } else if (cdb[2] == 0x80) { /* unit serial number */
        data[1] = 0x80;
        data[3] = 20;
        put_ascii(data + 4, 20, disk->serial);
        len = 24;
    } else {
        ans_scsi_check(reply, ANS_SENSE_ILLEGAL_REQUEST, ASC_INVALID_FIELD_CDB, 0);
        return;
    }
    reply_data(reply, buf, buf_len, alloc, data, len);
}

static void mode_sense(const uint8_t *cdb, bool ten, uint8_t *buf, uint32_t buf_len,
                       struct ans_scsi_reply *reply)
{
    uint8_t data[8 + CACHING_PAGE_LEN];
    uint8_t pc = (uint8_t)(cdb[2] >> 6);
    uint8_t page = (uint8_t)(cdb[2] & 0x3f);
    uint8_t subpage = cdb[3];
    uint32_t head = ten ? 8u : 4u;
    uint32_t len = head + CACHING_PAGE_LEN;
    uint32_t alloc = ten ? be16(cdb + 7) : cdb[4];
    uint32_t i;

    if (pc != 0 || subpage != 0 || (page != MODE_PAGE_ALL && page != MODE_PAGE_CACHING)) {
        ans_scsi_check(reply, ANS_SENSE_ILLEGAL_REQUEST, ASC_INVALID_FIELD_CDB, 0);
        return;
    }
    for (i = 0; i < sizeof(data); i++)
        data[i] = 0;
    if (ten) {
        data[0] = (uint8_t)((len - 2) >> 8);
        data[1] = (uint8_t)(len - 2);
        data[3] = MODE_WP; /* medium type 0, no block descriptors */
    } else {
        data[0] = (uint8_t)(len - 1);
        data[2] = MODE_WP;
    }
    /* the caching page: WCE 0 (no write cache), RCD 0 */
    data[head] = MODE_PAGE_CACHING;
    data[head + 1] = CACHING_PAGE_LEN - 2;
    reply_data(reply, buf, buf_len, alloc, data, len);
}

static uint64_t last_lba(const struct ans_scsi_disk *disk)
{
    return disk->blocks - 1;
}

/* The commands that write to the medium or change the device (findings/0071 c. 15). */
static bool writes(uint8_t op)
{
    switch (op) {
        case OP_WRITE6:
        case OP_WRITE10:
        case OP_WRITE12:
        case OP_WRITE16:
        case OP_WRITE_VERIFY10:
        case OP_WRITE_VERIFY12:
        case OP_WRITE_VERIFY16:
        case OP_WRITE_SAME10:
        case OP_WRITE_SAME16:
        case OP_UNMAP:
        case OP_FORMAT_UNIT:
        case OP_SANITIZE:
        case OP_WRITE_BUFFER:
        case OP_WRITE_LONG10:
        case OP_REASSIGN_BLOCKS:
        case OP_SECURITY_PROT_OUT:
        case OP_MODE_SELECT6:
        case OP_MODE_SELECT10:
        case OP_PERSISTENT_RES_OUT:
        /* not in conclusion 15's list, but they write too */
        case OP_COMPARE_AND_WRITE:
        case OP_ORWRITE16:
        case OP_XDWRITE10:
        case OP_XPWRITE10:
        case OP_XDWRITEREAD10:
        case OP_WRITE_STREAM16:
        case OP_WRITE_ATOMIC16:
        case OP_THIRD_PARTY_COPY_OUT: /* EXTENDED COPY, WRITE USING TOKEN */
        case OP_SERVICE_ACTION_OUT16: /* WRITE LONG(16) and the like */
            return true;
        default:
            return false;
    }
}

/* Where a refused write was to go, for the log; only the commands whose CDB says. */
static void write_target(const uint8_t *cdb, uint32_t cdb_len, struct ans_scsi_reply *reply)
{
    switch (cdb[0]) {
        case OP_WRITE6:
            if (cdb_len < 6)
                return;
            reply->lba = ((uint32_t)(cdb[1] & 0x1f) << 16) | be16(cdb + 2);
            reply->blocks = cdb[4] ? cdb[4] : 256u;
            break;
        case OP_WRITE10:
        case OP_WRITE_VERIFY10:
        case OP_WRITE_SAME10:
        case OP_XDWRITE10:
        case OP_XPWRITE10:
        case OP_XDWRITEREAD10:
            if (cdb_len < 10)
                return;
            reply->lba = be32(cdb + 2);
            reply->blocks = be16(cdb + 7);
            break;
        case OP_WRITE12:
        case OP_WRITE_VERIFY12:
            if (cdb_len < 12)
                return;
            reply->lba = be32(cdb + 2);
            reply->blocks = be32(cdb + 6);
            break;
        case OP_WRITE16:
        case OP_WRITE_VERIFY16:
        case OP_WRITE_SAME16:
        case OP_ORWRITE16:
        case OP_WRITE_STREAM16:
            if (cdb_len < 16)
                return;
            reply->lba = be64(cdb + 2);
            reply->blocks = be32(cdb + 10);
            break;
        case OP_COMPARE_AND_WRITE:
            if (cdb_len < 16)
                return;
            reply->lba = be64(cdb + 2);
            reply->blocks = cdb[13];
            break;
        default:
            return;
    }
    reply->lba_known = true;
}

static void read_cmd(const struct ans_scsi_disk *disk, uint64_t lba, uint32_t blocks,
                     uint32_t buf_len, struct ans_scsi_reply *reply)
{
    reply->lba = lba;
    reply->blocks = blocks;
    reply->lba_known = true;
    if (blocks == 0) /* transfer nothing: not an error (SBC-3) */
        return;
    if (lba >= disk->blocks || blocks > disk->blocks - lba) {
        ans_scsi_check(reply, ANS_SENSE_ILLEGAL_REQUEST, ASC_LBA_OUT_OF_RANGE, 0);
        return;
    }
    if ((uint64_t)blocks * disk->block_size != buf_len) {
        ans_scsi_check(reply, ANS_SENSE_ILLEGAL_REQUEST, ASC_INVALID_FIELD_CDB, 0);
        return;
    }
    reply->action = ANS_SCSI_READ;
}

/* Length of the CDB each operation code group has (SPC-4 4.2.5.1). */
static uint32_t cdb_length(uint8_t op)
{
    switch (op >> 5) {
        case 0:
            return 6;
        case 1:
        case 2:
            return 10;
        case 4:
            return 16;
        case 5:
            return 12;
        default:
            return 6; /* reserved and vendor groups: only the opcode is looked at */
    }
}

void ans_scsi_execute(const struct ans_scsi_disk *disk, const uint8_t *cdb, uint32_t cdb_len,
                      uint8_t *buf, uint32_t buf_len, struct ans_scsi_reply *reply)
{
    uint8_t data[32];
    uint8_t op;
    uint32_t i;

    reply->action = ANS_SCSI_COMPLETE;
    reply->opcode = 0;
    reply->status = ANS_SCSI_GOOD;
    reply->sense_key = reply->asc = reply->ascq = 0;
    reply->length = 0;
    reply->write_refused = false;
    reply->lba_known = false;
    reply->lba = 0;
    reply->blocks = 0;

    if (!cdb || cdb_len == 0) {
        ans_scsi_check(reply, ANS_SENSE_ILLEGAL_REQUEST, ASC_INVALID_OPCODE, 0);
        return;
    }
    op = cdb[0];
    reply->opcode = op;

    /* first, before anything else looks at the CDB: nothing that writes gets through */
    if (writes(op)) {
        reply->write_refused = true;
        write_target(cdb, cdb_len, reply);
        ans_scsi_check(reply, ANS_SENSE_DATA_PROTECT, ASC_INVALID_FIELD_CDB, 0);
        return;
    }

    if (cdb_len < cdb_length(op)) {
        ans_scsi_check(reply, ANS_SENSE_ILLEGAL_REQUEST, ASC_INVALID_FIELD_CDB, 0);
        return;
    }

    for (i = 0; i < sizeof(data); i++)
        data[i] = 0;

    switch (op) {
        case OP_READ10:
            read_cmd(disk, be32(cdb + 2), be16(cdb + 7), buf_len, reply);
            return;
        case OP_READ16:
            read_cmd(disk, be64(cdb + 2), be32(cdb + 10), buf_len, reply);
            return;
        case OP_INQUIRY:
            inquiry(disk, cdb, buf, buf_len, reply);
            return;
        case OP_REPORT_LUNS:
            /* one LUN, number 0: list length 8, then 4 reserved bytes, then the LUN */
            data[3] = 8;
            reply_data(reply, buf, buf_len, be32(cdb + 6), data, 16);
            return;
        case OP_READ_CAPACITY10: {
            uint64_t last = last_lba(disk);
            put_be32(data, last > 0xffffffffull ? 0xffffffffu : (uint32_t)last);
            put_be32(data + 4, disk->block_size);
            reply_data(reply, buf, buf_len, 8, data, 8);
            return;
        }
        case OP_SERVICE_ACTION_IN16:
            if ((cdb[1] & 0x1f) != SA_READ_CAPACITY16) {
                ans_scsi_check(reply, ANS_SENSE_ILLEGAL_REQUEST, ASC_INVALID_FIELD_CDB, 0);
                return;
            }
            /* last LBA, block length; one logical block per physical, aligned at 0,
               no thin provisioning, no protection */
            put_be64(data, last_lba(disk));
            put_be32(data + 8, disk->block_size);
            reply_data(reply, buf, buf_len, be32(cdb + 10), data, 32);
            return;
        case OP_MODE_SENSE6:
            mode_sense(cdb, false, buf, buf_len, reply);
            return;
        case OP_MODE_SENSE10:
            mode_sense(cdb, true, buf, buf_len, reply);
            return;
        case OP_TEST_UNIT_READY:
        case OP_START_STOP_UNIT:
        case OP_SYNCHRONIZE_CACHE:
        case OP_SYNCHRONIZE_CACHE16:
        case OP_VERIFY10:
        case OP_VERIFY16:
            /* nothing to do on a read-only disk without a write cache */
            return;
        default:
            ans_scsi_check(reply, ANS_SENSE_ILLEGAL_REQUEST, ASC_INVALID_OPCODE, 0);
            return;
    }
}

uint32_t ans_scsi_sense(const struct ans_scsi_reply *reply, uint8_t *sense, uint32_t len)
{
    uint8_t data[18];
    uint32_t n = len < sizeof(data) ? len : (uint32_t)sizeof(data);
    uint32_t i;

    for (i = 0; i < sizeof(data); i++)
        data[i] = 0;
    data[0] = 0x70; /* current error, fixed format */
    data[2] = reply->sense_key;
    data[7] = sizeof(data) - 8;
    data[12] = reply->asc;
    data[13] = reply->ascq;
    for (i = 0; i < n; i++)
        sense[i] = data[i];
    return n;
}
