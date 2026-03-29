/*
 * dimg-tool — disc image preservation and conversion tool
 *
 * Links libaaruformat (LGPL-2.1) for .aaru container support.
 */

#ifndef DIMG_H
#define DIMG_H

#include <stdint.h>
#include <stdio.h>

/* Exit codes */
#define DIMG_OK              0
#define DIMG_ERR_ARGS        1
#define DIMG_ERR_IO          2
#define DIMG_ERR_FORMAT      3
#define DIMG_ERR_VERIFY      4
#define DIMG_ERR_UNSUPPORTED 5

/* Version (set at build time, fallback here) */
#ifndef DIMG_VERSION
#define DIMG_VERSION "0.3.4"
#endif

/* Subcommands */
int cmd_info(int argc, char **argv);
int cmd_convert(int argc, char **argv);
int cmd_verify(int argc, char **argv);

/* Detect the data compression codec used in an opened .aaru image.
 * Returns a static string: "lzma", "zstd", "flac", or "none". */
const char *aaru_detect_codec(void *aaru_ctx);

/* Read sectors [first_lba..last_lba] from aaru and write to an open BIN file.
 * total_for_progress is used for stderr progress display. */
int write_sectors_to_bin(FILE *bf, void *aaru_ctx,
                         int64_t first_lba, int64_t last_lba,
                         int64_t total_for_progress);

#endif /* DIMG_H */
