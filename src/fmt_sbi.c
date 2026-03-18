/*
 * fmt_sbi.c — SBI (SubChannel Binary Index) file parser
 *
 * SBI files store corrected Q subchannel data for specific sectors,
 * used by PS1 LibCrypt copy protection. Redump distributes them
 * alongside CUE/BIN dumps for European PS1 games with LibCrypt.
 *
 * Format:
 *   4 bytes: "SBI\0" magic
 *   N × 14-byte records:
 *     3 bytes: BCD-encoded MSF (minute, second, frame)
 *     1 byte:  type (0x01 = Q subchannel)
 *     10 bytes: Q subchannel data (without CRC16)
 *
 * LibCrypt encodes protection keys by intentionally corrupting the
 * Q subchannel CRC at specific sectors. The SBI records contain the
 * as-dumped (corrupted) Q data. When writing to .aaru, we reconstruct
 * the full 96-byte raw subchannel with the invalid CRC preserved.
 */

#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "dimg.h"
#include "disc.h"
#include "aaru.h"
#include "aaruformat.h"

#define SBI_MAGIC_SIZE    4
#define SBI_RECORD_SIZE  14
#define SBI_Q_DATA_SIZE  10
#define SUBCHANNEL_SIZE  96

/* Q subchannel CRC16 table (CRC-CCITT, poly 0x1021) */
static const uint16_t subq_crc_table[256] = {
    0x0000, 0x1021, 0x2042, 0x3063, 0x4084, 0x50A5, 0x60C6, 0x70E7,
    0x8108, 0x9129, 0xA14A, 0xB16B, 0xC18C, 0xD1AD, 0xE1CE, 0xF1EF,
    0x1231, 0x0210, 0x3273, 0x2252, 0x52B5, 0x4294, 0x72F7, 0x62D6,
    0x9339, 0x8318, 0xB37B, 0xA35A, 0xD3BD, 0xC39C, 0xF3FF, 0xE3DE,
    0x2462, 0x3443, 0x0420, 0x1401, 0x64E6, 0x74C7, 0x44A4, 0x5485,
    0xA56A, 0xB54B, 0x8528, 0x9509, 0xE5EE, 0xF5CF, 0xC5AC, 0xD58D,
    0x3653, 0x2672, 0x1611, 0x0630, 0x76D7, 0x66F6, 0x5695, 0x46B4,
    0xB75B, 0xA77A, 0x9719, 0x8738, 0xF7DF, 0xE7FE, 0xD79D, 0xC7BC,
    0x4864, 0x5845, 0x6826, 0x7807, 0x08E0, 0x18C1, 0x28A2, 0x38A3,
    0xC94C, 0xD96D, 0xE90E, 0xF92F, 0x89C8, 0x99E9, 0xA98A, 0xB9AB,
    0x5A75, 0x4A54, 0x7A37, 0x6A16, 0x1AF1, 0x0AD0, 0x3AB3, 0x2A92,
    0xDB7D, 0xCB5C, 0xFB3F, 0xEB1E, 0x9BF9, 0x8BD8, 0xBBBB, 0xAB9A,
    0x6CA6, 0x7C87, 0x4CE4, 0x5CC5, 0x2C22, 0x3C03, 0x0C60, 0x1C41,
    0xEDAE, 0xFD8F, 0xCDEC, 0xDDCD, 0xAD2A, 0xBD0B, 0x8D68, 0x9D49,
    0x7E97, 0x6EB6, 0x5ED5, 0x4EF4, 0x3E13, 0x2E32, 0x1E51, 0x0E70,
    0xFF9F, 0xEFBE, 0xDFDD, 0xCFFC, 0xBF1B, 0xAF3A, 0x9F59, 0x8F78,
    0x9188, 0x81A9, 0xB1CA, 0xA1EB, 0xD10C, 0xC12D, 0xF14E, 0xE16F,
    0x1080, 0x00A1, 0x30C2, 0x20E3, 0x5004, 0x4025, 0x7046, 0x6067,
    0x83B9, 0x9398, 0xA3FB, 0xB3DA, 0xC33D, 0xD31C, 0xE37F, 0xF35E,
    0x02B1, 0x1290, 0x22F3, 0x32D2, 0x4235, 0x5214, 0x6277, 0x7256,
    0xB5EA, 0xA5CB, 0x95A8, 0x85A9, 0xF56E, 0xE54F, 0xD52C, 0xC50D,
    0x34E2, 0x24C3, 0x14A0, 0x0481, 0x7466, 0x6447, 0x5424, 0x4405,
    0xA7DB, 0xB7FA, 0x8799, 0x97B8, 0xE75F, 0xF77E, 0xC71D, 0xD73C,
    0x26D3, 0x36F2, 0x0691, 0x16B0, 0x6657, 0x7676, 0x4615, 0x5634,
    0xD9EC, 0xC9CD, 0xF9AE, 0xE98F, 0x9968, 0x8949, 0xB92A, 0xA90B,
    0x58E4, 0x48C5, 0x78A6, 0x6887, 0x1860, 0x0841, 0x3822, 0x2803,
    0xCB7D, 0xDB5C, 0xEB3F, 0xFB1E, 0x8BF9, 0x9BD8, 0xABBB, 0xBB9A,
    0x4A75, 0x5A54, 0x6A37, 0x7A16, 0x0AF1, 0x1AD0, 0x2AB3, 0x3A92,
    0xFD2E, 0xED0F, 0xDD6C, 0xCD4D, 0xBDAA, 0xAD8B, 0x9DE8, 0x8DC9,
    0x7C26, 0x6C07, 0x5C64, 0x4C45, 0x3CA2, 0x2C83, 0x1CE0, 0x0CC1,
    0xEF1F, 0xFF3E, 0xCF5D, 0xDF7C, 0xAF9B, 0xBFBA, 0x8FD9, 0x9FF8,
    0x6E17, 0x7E36, 0x4E55, 0x5E74, 0x2E93, 0x3EB2, 0x0ED1, 0x1EF0,
};

/* Decode BCD byte to binary */
static int bcd_to_bin(uint8_t bcd)
{
    return (bcd >> 4) * 10 + (bcd & 0x0F);
}

/* Convert BCD-encoded MSF to LBA */
static int64_t bcd_msf_to_lba(uint8_t m, uint8_t s, uint8_t f)
{
    return (int64_t)bcd_to_bin(m) * 60 * 75
         + (int64_t)bcd_to_bin(s) * 75
         + (int64_t)bcd_to_bin(f);
}

/*
 * Compute Q subchannel CRC16 over 10 data bytes,
 * store inverted result in bytes 10-11 (big-endian).
 *
 * Per CD spec: CRC-CCITT poly 0x1021, init 0, XorOut 0xFFFF.
 * The inversion (bitwise NOT) is the XorOut step.
 */
static void subq_compute_crc(uint8_t *q12)
{
    uint16_t crc = 0;
    for(int i = 0; i < 10; i++)
        crc = subq_crc_table[(crc >> 8) ^ q12[i]] ^ (uint16_t)(crc << 8);

    q12[10] = (uint8_t)~(crc >> 8);
    q12[11] = (uint8_t)~(crc & 0xFF);
}

/*
 * Build 96-byte interleaved raw subchannel from 12-byte Q data.
 *
 * Sets P channel to all-ones (normal for data track content).
 * Sets Q channel from the provided 12 bytes.
 * R-W channels are all zeros (no CD-TEXT/CD+G).
 *
 * Interleaving: each of the 96 output bytes contains one bit
 * from each channel: bit7=P, bit6=Q, bits5-0=R..W (all zero).
 */
static void q_to_raw96(const uint8_t *q12, uint8_t *raw96)
{
    for(int i = 0; i < 96; i++)
    {
        int q_bit = (q12[i >> 3] >> (7 - (i & 7))) & 1;
        raw96[i] = (uint8_t)(0x80 | (q_bit ? 0x40 : 0x00));
    }
}

int sbi_load_and_write(const char *sbi_path, void *aaru_ctx)
{
    assert(sbi_path != NULL);
    assert(aaru_ctx != NULL);

    FILE *f = fopen(sbi_path, "rb");
    if(f == NULL)
    {
        fprintf(stderr, "Cannot open SBI: %s\n", sbi_path);
        return DIMG_ERR_IO;
    }

    /* Verify magic */
    uint8_t magic[SBI_MAGIC_SIZE];
    if(fread(magic, 1, SBI_MAGIC_SIZE, f) != SBI_MAGIC_SIZE ||
       magic[0] != 'S' || magic[1] != 'B' || magic[2] != 'I' || magic[3] != '\0')
    {
        fprintf(stderr, "Invalid SBI magic: %s\n", sbi_path);
        fclose(f);
        return DIMG_ERR_FORMAT;
    }

    /* Determine number of records */
    fseeko(f, 0, SEEK_END);
    int64_t file_size = ftello(f);
    fseeko(f, SBI_MAGIC_SIZE, SEEK_SET);

    int64_t data_size = file_size - SBI_MAGIC_SIZE;
    if(data_size <= 0 || data_size % SBI_RECORD_SIZE != 0)
    {
        fprintf(stderr, "Invalid SBI file size (%" PRId64 " bytes): %s\n",
                file_size, sbi_path);
        fclose(f);
        return DIMG_ERR_FORMAT;
    }

    int record_count = (int)(data_size / SBI_RECORD_SIZE);

    fprintf(stderr, "  SBI: %s (%d subchannel records)\n", sbi_path, record_count);

    /* Process each record */
    int written = 0;
    for(int r = 0; r < record_count; r++)
    {
        uint8_t record[SBI_RECORD_SIZE];
        if(fread(record, 1, SBI_RECORD_SIZE, f) != SBI_RECORD_SIZE)
        {
            fprintf(stderr, "Short read at SBI record %d\n", r);
            fclose(f);
            return DIMG_ERR_IO;
        }

        uint8_t bcd_m = record[0];
        uint8_t bcd_s = record[1];
        uint8_t bcd_f = record[2];
        uint8_t type  = record[3];

        if(type != 0x01)
        {
            fprintf(stderr, "Unknown SBI record type 0x%02X at record %d\n", type, r);
            continue;
        }

        /* Convert MSF to LBA.
         * SBI MSF addresses include the standard 150-frame (2-second) offset,
         * so the LBA for writing is MSF - 150. */
        int64_t msf_lba = bcd_msf_to_lba(bcd_m, bcd_s, bcd_f);
        int64_t lba = msf_lba - 150;

        if(lba < 0)
        {
            fprintf(stderr, "SBI record %d: negative LBA %" PRId64 ", skipping\n", r, lba);
            continue;
        }

        /* Build 12-byte Q subchannel: 10 data bytes + CRC16 */
        uint8_t q12[12];
        memcpy(q12, &record[4], SBI_Q_DATA_SIZE);
        subq_compute_crc(q12);

        /* Build 96-byte interleaved raw subchannel */
        uint8_t raw96[SUBCHANNEL_SIZE];
        q_to_raw96(q12, raw96);

        /* Write subchannel tag to .aaru */
        int32_t res = aaruf_write_sector_tag(aaru_ctx, (uint64_t)lba, false,
                                              raw96, SUBCHANNEL_SIZE,
                                              8 /* kSectorTagCdSubchannel */);
        if(res != 0)
        {
            fprintf(stderr, "Failed to write subchannel at LBA %" PRId64 ": %d\n",
                    lba, res);
            fclose(f);
            return DIMG_ERR_IO;
        }

        written++;
    }

    fclose(f);
    fprintf(stderr, "  SBI: %d subchannel sectors written\n", written);
    return DIMG_OK;
}

/*
 * Try to find an SBI file matching a CUE file.
 * Looks for same basename with .sbi extension in the same directory.
 * Returns the path in sbi_buf, or empty string if not found.
 */
void sbi_find_for_cue(const char *cue_path, char *sbi_buf, size_t bufsize)
{
    assert(cue_path != NULL);
    assert(sbi_buf != NULL);

    sbi_buf[0] = '\0';

    size_t len = strlen(cue_path);
    if(len < 4 || len >= bufsize)
        return;

    memcpy(sbi_buf, cue_path, len - 4);
    memcpy(sbi_buf + len - 4, ".sbi", 5);

    FILE *f = fopen(sbi_buf, "rb");
    if(f != NULL)
    {
        fclose(f);
        return; /* found */
    }

    sbi_buf[0] = '\0'; /* not found */
}
