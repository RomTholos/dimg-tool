/*
 * fmt_iso.c — ISO image parser and writer
 *
 * Handles plain ISO files (2048-byte sector dumps) for DVD-based systems.
 * Single track, no subchannel, no raw framing.
 */

#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "dimg.h"
#include "disc.h"
#include "aaru.h"
#include "aaruformat.h"

int iso_parse(const char *iso_path, DiscSystem system, DiscLayout *layout)
{
    assert(iso_path != NULL);
    assert(layout != NULL);

    FILE *f = fopen(iso_path, "rb");
    if(f == NULL)
    {
        fprintf(stderr, "Cannot open ISO: %s\n", iso_path);
        return DIMG_ERR_IO;
    }

    fseeko(f, 0, SEEK_END);
    int64_t file_size = ftello(f);
    fclose(f);

    if(file_size <= 0)
    {
        fprintf(stderr, "Empty or unreadable ISO: %s\n", iso_path);
        return DIMG_ERR_FORMAT;
    }

    if(file_size % SECTOR_USER != 0)
    {
        fprintf(stderr, "ISO size (%" PRId64 ") is not a multiple of %d bytes: %s\n",
                file_size, SECTOR_USER, iso_path);
        return DIMG_ERR_FORMAT;
    }

    int64_t sectors = file_size / SECTOR_USER;

    memset(layout, 0, sizeof(*layout));
    layout->system        = system;
    layout->source_format = DISC_FMT_ISO;
    layout->track_count   = 1;
    layout->total_sectors = sectors;

    DiscTrack *t   = &layout->tracks[0];
    t->number      = 1;
    t->type        = DISC_TRACK_DVD;
    t->start       = 0;
    t->end         = sectors - 1;
    t->pregap      = 0;
    t->session     = 1;
    t->sector_size = SECTOR_USER;
    t->bin_offset  = 0;

    size_t len = strlen(iso_path);
    if(len >= sizeof(t->bin_path))
    {
        fprintf(stderr, "ISO path too long: %s\n", iso_path);
        return DIMG_ERR_ARGS;
    }
    memcpy(t->bin_path, iso_path, len + 1);

    return DIMG_OK;
}

int iso_write(const char *iso_path, const DiscLayout *layout, void *aaru_ctx)
{
    assert(iso_path != NULL);
    assert(layout != NULL);
    assert(aaru_ctx != NULL);

    FILE *f = fopen(iso_path, "wb");
    if(f == NULL)
    {
        fprintf(stderr, "Cannot create ISO: %s\n", iso_path);
        return DIMG_ERR_IO;
    }

    uint8_t buf[SECTOR_USER];
    int64_t total = layout->total_sectors;

    for(int64_t s = 0; s < total; s++)
    {
        uint32_t rlen   = SECTOR_USER;
        uint8_t  status = 0;
        int32_t  res    = aaruf_read_sector(aaru_ctx, (uint64_t)s, false,
                                            buf, &rlen, &status);

        if(res < 0)
        {
            fprintf(stderr, "Read error at sector %" PRId64 ": %d\n", s, res);
            fclose(f);
            return DIMG_ERR_IO;
        }

        /* status 1 = not dumped (zeros) — write zeros */
        if(res == 1)
            memset(buf, 0, SECTOR_USER);

        if(fwrite(buf, 1, SECTOR_USER, f) != SECTOR_USER)
        {
            fprintf(stderr, "Write error at sector %" PRId64 "\n", s);
            fclose(f);
            return DIMG_ERR_IO;
        }

        /* Progress every 10000 sectors */
        if(s > 0 && s % 10000 == 0)
            fprintf(stderr, "\r  %" PRId64 "/%" PRId64 " sectors", s, total);
    }

    if(total > 10000)
        fprintf(stderr, "\r  %" PRId64 "/%" PRId64 " sectors\n", total, total);

    fclose(f);
    return DIMG_OK;
}
