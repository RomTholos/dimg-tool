/*
 * fmt_aaru.c — bridge between DiscLayout and libaaruformat
 *
 * aaru_write():       DiscLayout + source files → .aaru image
 * aaru_read_layout(): .aaru image → DiscLayout + opened context
 */

#include <assert.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "dimg.h"
#include "disc.h"
#include "aaru.h"
#include "aaruformat.h"
#include "aaruformat/enums.h"
#include "aaruformat/structs/optical.h"

/* Map DiscTrackType → libaaruformat TrackType */
static uint8_t track_type_to_aaru(DiscTrackType type)
{
    switch(type)
    {
        case DISC_TRACK_AUDIO: return kTrackTypeAudio;
        case DISC_TRACK_MODE1: return kTrackTypeCdMode1;
        case DISC_TRACK_MODE2: return kTrackTypeCdMode2Formless;
        case DISC_TRACK_DVD:   return kTrackTypeData;
        default:               return kTrackTypeData;
    }
}

/* Map libaaruformat TrackType → DiscTrackType */
static DiscTrackType track_type_from_aaru(uint8_t aaru_type)
{
    switch(aaru_type)
    {
        case kTrackTypeAudio:           return DISC_TRACK_AUDIO;
        case kTrackTypeCdMode1:         return DISC_TRACK_MODE1;
        case kTrackTypeCdMode2Formless: return DISC_TRACK_MODE2;
        case kTrackTypeCdMode2Form1:    return DISC_TRACK_MODE2;
        case kTrackTypeCdMode2Form2:    return DISC_TRACK_MODE2;
        default:                        return DISC_TRACK_MODE1;
    }
}

int aaru_write(const char *aaru_path, const DiscLayout *layout, const char *options)
{
    assert(aaru_path != NULL);
    assert(layout != NULL);
    assert(layout->track_count > 0);

    int      is_cd       = disc_is_cd(layout->system);
    uint32_t sector_size = is_cd ? SECTOR_RAW : SECTOR_USER;

    /* Create .aaru image */
    void *ctx = aaruf_create(aaru_path,
                             (uint32_t)layout->system,
                             sector_size,
                             (uint64_t)layout->total_sectors,
                             0, 0,
                             options,
                             (const uint8_t *)"dimg-tool", 9,
                             0, 1,
                             false);
    if(ctx == NULL)
    {
        fprintf(stderr, "Failed to create .aaru image: %s\n", aaru_path);
        return DIMG_ERR_IO;
    }

    /* Set track layout for CD-based media */
    if(is_cd)
    {
        TrackEntry te[DISC_MAX_TRACKS];
        memset(te, 0, sizeof(te));

        for(int i = 0; i < layout->track_count; i++)
        {
            const DiscTrack *dt = &layout->tracks[i];
            te[i].sequence = dt->number;
            te[i].type     = track_type_to_aaru(dt->type);
            te[i].start    = dt->start;
            te[i].end      = dt->end;
            te[i].pregap   = dt->pregap;
            te[i].session  = dt->session;
        }

        int32_t res = aaruf_set_tracks(ctx, te, layout->track_count);
        if(res != 0)
        {
            fprintf(stderr, "Failed to set tracks: %d\n", res);
            aaruf_close(ctx);
            return DIMG_ERR_FORMAT;
        }
    }

    /* Write sectors from source files */
    uint8_t buf[SECTOR_RAW]; /* large enough for both 2352 and 2048 */

    for(int i = 0; i < layout->track_count; i++)
    {
        const DiscTrack *dt = &layout->tracks[i];
        int64_t count = dt->end - dt->start + 1;

        FILE *f = fopen(dt->bin_path, "rb");
        if(f == NULL)
        {
            fprintf(stderr, "Cannot open source: %s\n", dt->bin_path);
            aaruf_close(ctx);
            return DIMG_ERR_IO;
        }

        if(dt->bin_offset > 0)
            fseeko(f, dt->bin_offset, SEEK_SET);

        fprintf(stderr, "  Track %d/%d [%s] sectors %" PRId64 "-%" PRId64 "\n",
                dt->number, layout->track_count,
                dt->type == DISC_TRACK_AUDIO ? "AUDIO" :
                dt->type == DISC_TRACK_MODE1 ? "MODE1" :
                dt->type == DISC_TRACK_MODE2 ? "MODE2" : "DATA",
                dt->start, dt->end);

        for(int64_t s = 0; s < count; s++)
        {
            size_t read = fread(buf, 1, dt->sector_size, f);
            if(read != dt->sector_size)
            {
                fprintf(stderr, "Short read at sector %" PRId64 " of track %d\n",
                        s, dt->number);
                fclose(f);
                aaruf_close(ctx);
                return DIMG_ERR_IO;
            }

            int32_t res;
            if(is_cd)
                res = aaruf_write_sector_long(ctx, (uint64_t)(dt->start + s),
                                              false, buf, SectorStatusDumped,
                                              dt->sector_size);
            else
                res = aaruf_write_sector(ctx, (uint64_t)(dt->start + s),
                                         false, buf, SectorStatusDumped,
                                         dt->sector_size);

            if(res != 0)
            {
                fprintf(stderr, "Write error at LBA %" PRId64 ": %d\n",
                        dt->start + s, res);
                fclose(f);
                aaruf_close(ctx);
                return DIMG_ERR_IO;
            }

            if(s > 0 && s % 10000 == 0)
                fprintf(stderr, "\r    %" PRId64 "/%" PRId64, s, count);
        }

        if(count > 10000)
            fprintf(stderr, "\r    %" PRId64 "/%" PRId64 "\n", count, count);

        fclose(f);
    }

    int32_t cres = aaruf_close(ctx);
    if(cres != 0)
    {
        fprintf(stderr, "Failed to close .aaru image: %d\n", cres);
        return DIMG_ERR_IO;
    }

    return DIMG_OK;
}

int aaru_read_layout(const char *aaru_path, DiscLayout *layout, void **ctx_out)
{
    assert(aaru_path != NULL);
    assert(layout != NULL);
    assert(ctx_out != NULL);

    void *ctx = aaruf_open(aaru_path, false, NULL);
    if(ctx == NULL)
    {
        fprintf(stderr, "Failed to open .aaru image: %s\n", aaru_path);
        return DIMG_ERR_IO;
    }

    ImageInfo info;
    int32_t res = aaruf_get_image_info(ctx, &info);
    if(res != AARUF_STATUS_OK)
    {
        fprintf(stderr, "Failed to read image info: %d\n", res);
        aaruf_close(ctx);
        return DIMG_ERR_FORMAT;
    }

    memset(layout, 0, sizeof(*layout));
    layout->system        = (DiscSystem)info.MediaType;
    layout->source_format = DISC_FMT_AARU;
    layout->total_sectors = (int64_t)info.Sectors;

    int is_cd = disc_is_cd(layout->system);

    if(is_cd)
    {
        /* Read optical track list */
        size_t track_buf_len = sizeof(TrackEntry) * DISC_MAX_TRACKS;
        uint8_t track_buf[sizeof(TrackEntry) * DISC_MAX_TRACKS];

        res = aaruf_get_tracks(ctx, track_buf, &track_buf_len);
        if(res != AARUF_STATUS_OK)
        {
            fprintf(stderr, "Failed to read tracks: %d\n", res);
            aaruf_close(ctx);
            return DIMG_ERR_FORMAT;
        }

        int count = (int)(track_buf_len / sizeof(TrackEntry));
        if(count <= 0 || count > DISC_MAX_TRACKS)
        {
            fprintf(stderr, "Invalid track count: %d\n", count);
            aaruf_close(ctx);
            return DIMG_ERR_FORMAT;
        }

        const TrackEntry *entries = (const TrackEntry *)track_buf;
        layout->track_count = count;

        for(int i = 0; i < count; i++)
        {
            DiscTrack *dt    = &layout->tracks[i];
            dt->number       = entries[i].sequence;
            dt->type         = track_type_from_aaru(entries[i].type);
            dt->start        = entries[i].start;
            dt->end          = entries[i].end;
            dt->pregap       = entries[i].pregap;
            dt->session      = entries[i].session;
            dt->sector_size  = SECTOR_RAW;
            dt->bin_path[0]  = '\0';
            dt->bin_offset   = 0;
        }
    }
    else
    {
        /* DVD / block media — single track, no track table */
        layout->track_count = 1;
        DiscTrack *dt    = &layout->tracks[0];
        dt->number       = 1;
        dt->type         = DISC_TRACK_DVD;
        dt->start        = 0;
        dt->end          = layout->total_sectors - 1;
        dt->pregap       = 0;
        dt->session      = 1;
        dt->sector_size  = SECTOR_USER;
        dt->bin_path[0]  = '\0';
        dt->bin_offset   = 0;
    }

    *ctx_out = ctx;
    return DIMG_OK;
}
