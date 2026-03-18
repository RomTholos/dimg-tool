/*
 * dimg-tool — disc image preservation and conversion tool
 *
 * Links libaaruformat (LGPL-2.1) for .aaru container support.
 */

#ifndef DIMG_H
#define DIMG_H

#include <stdint.h>

/* Exit codes */
#define DIMG_OK              0
#define DIMG_ERR_ARGS        1
#define DIMG_ERR_IO          2
#define DIMG_ERR_FORMAT      3
#define DIMG_ERR_VERIFY      4
#define DIMG_ERR_UNSUPPORTED 5

/* Subcommands */
int cmd_info(int argc, char **argv);
int cmd_convert(int argc, char **argv);
int cmd_verify(int argc, char **argv);

#endif /* DIMG_H */
