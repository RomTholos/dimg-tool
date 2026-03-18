/*
 * fmt_cue.c — CUE/BIN disc image parser and writer
 *
 * Handles CUE sheet parsing for multi-track CD images and
 * rendering .aaru back to CUE/BIN format.
 *
 * Supports: single-BIN and multi-BIN CUE sheets,
 * MODE1/2352, MODE2/2352, and AUDIO tracks.
 */

#include <assert.h>
#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dimg.h"
#include "disc.h"
#include "aaru.h"
#include "aaruformat.h"

/* MSF (MM:SS:FF) → frame count */
static int64_t msf_to_frames(int m, int s, int f)
{
    return (int64_t)m * 60 * 75 + (int64_t)s * 75 + f;
}

/* Frame count → MSF */
static void frames_to_msf(int64_t frames, int *m, int *s, int *f)
{
    *f = (int)(frames % 75);
    *s = (int)((frames / 75) % 60);
    *m = (int)(frames / (75 * 60));
}

/* Map CUE track type string to DiscTrackType */
static int parse_track_type(const char *line, DiscTrackType *type)
{
    if(strstr(line, "MODE1/2352"))
    {
        *type = DISC_TRACK_MODE1;
        return 0;
    }
    if(strstr(line, "MODE2/2352"))
    {
        *type = DISC_TRACK_MODE2;
        return 0;
    }
    if(strstr(line, "AUDIO"))
    {
        *type = DISC_TRACK_AUDIO;
        return 0;
    }
    return -1;
}

/* Map DiscTrackType to CUE type string */
static const char *track_type_to_cue(DiscTrackType type)
{
    switch(type)
    {
        case DISC_TRACK_MODE1: return "MODE1/2352";
        case DISC_TRACK_MODE2: return "MODE2/2352";
        case DISC_TRACK_AUDIO: return "AUDIO";
        default:               return "MODE1/2352";
    }
}

/* Extract directory from a file path into buf */
static void path_dirname(const char *path, char *buf, size_t bufsize)
{
    const char *last_sep = strrchr(path, '/');
    if(last_sep == NULL)
    {
        buf[0] = '.';
        buf[1] = '\0';
        return;
    }
    size_t len = (size_t)(last_sep - path);
    if(len >= bufsize) len = bufsize - 1;
    memcpy(buf, path, len);
    buf[len] = '\0';
}

/* Extract filename (without directory) from a path */
static const char *path_basename(const char *path)
{
    const char *last_sep = strrchr(path, '/');
    return last_sep != NULL ? last_sep + 1 : path;
}

int cue_parse(const char *cue_path, DiscSystem system, DiscLayout *layout)
{
    assert(cue_path != NULL);
    assert(layout != NULL);

    FILE *f = fopen(cue_path, "r");
    if(f == NULL)
    {
        fprintf(stderr, "Cannot open CUE: %s\n", cue_path);
        return DIMG_ERR_IO;
    }

    char dir[512];
    path_dirname(cue_path, dir, sizeof(dir));

    memset(layout, 0, sizeof(*layout));
    layout->system        = system;
    layout->source_format = DISC_FMT_CUE;

    char line[1024];
    char current_file[512] = {0};
    int  count             = 0;

    /* Temporary storage for INDEX 01 positions (MSF frames from FILE start) */
    int64_t index01[DISC_MAX_TRACKS];
    memset(index01, 0, sizeof(index01));

    while(fgets(line, sizeof(line), f))
    {
        /* Strip leading whitespace */
        char *p = line;
        while(*p == ' ' || *p == '\t') p++;

        /* FILE directive */
        if(strncmp(p, "FILE ", 5) == 0)
        {
            p += 5;
            while(*p == ' ') p++;

            /* Extract quoted filename */
            if(*p == '"')
            {
                p++;
                char *end = strchr(p, '"');
                if(end == NULL) continue;
                size_t len = (size_t)(end - p);
                if(len >= sizeof(current_file)) len = sizeof(current_file) - 1;
                memcpy(current_file, p, len);
                current_file[len] = '\0';
            }
            else
            {
                /* Unquoted — take until whitespace */
                char *end = p;
                while(*end && *end != ' ' && *end != '\t' && *end != '\n') end++;
                size_t len = (size_t)(end - p);
                if(len >= sizeof(current_file)) len = sizeof(current_file) - 1;
                memcpy(current_file, p, len);
                current_file[len] = '\0';
            }
            continue;
        }

        /* TRACK directive */
        if(strncmp(p, "TRACK ", 6) == 0)
        {
            if(count >= DISC_MAX_TRACKS)
            {
                fprintf(stderr, "Too many tracks in CUE\n");
                fclose(f);
                return DIMG_ERR_FORMAT;
            }

            DiscTrack *t = &layout->tracks[count];
            memset(t, 0, sizeof(*t));

            t->number = (uint8_t)atoi(p + 6);
            t->session  = 1;

            DiscTrackType type;
            if(parse_track_type(p, &type) != 0)
            {
                fprintf(stderr, "Unknown track type in CUE: %s", line);
                fclose(f);
                return DIMG_ERR_FORMAT;
            }
            t->type        = type;
            t->sector_size = SECTOR_RAW;

            /* Resolve BIN path relative to CUE directory */
            snprintf(t->bin_path, sizeof(t->bin_path), "%s/%s", dir, current_file);

            count++;
            continue;
        }

        /* INDEX directive */
        if(strncmp(p, "INDEX ", 6) == 0 && count > 0)
        {
            int idx_num, mm, ss, ff;
            if(sscanf(p + 6, "%d %d:%d:%d", &idx_num, &mm, &ss, &ff) == 4)
            {
                if(idx_num == 1)
                    index01[count - 1] = msf_to_frames(mm, ss, ff);
            }
            continue;
        }
    }

    fclose(f);

    if(count == 0)
    {
        fprintf(stderr, "No tracks found in CUE: %s\n", cue_path);
        return DIMG_ERR_FORMAT;
    }

    /*
     * Compute track sector ranges.
     *
     * For multi-file CUE (one BIN per track):
     *   Each track's sectors = file_size / sector_size.
     *   LBAs assigned sequentially.
     *
     * For single-file CUE (all tracks in one BIN):
     *   Track boundaries from INDEX 01 positions.
     *   Last track extends to EOF.
     */

    /* Check if all tracks reference the same file */
    int single_bin = 1;
    for(int i = 1; i < count; i++)
    {
        if(strcmp(layout->tracks[0].bin_path, layout->tracks[i].bin_path) != 0)
        {
            single_bin = 0;
            break;
        }
    }

    int64_t running_lba = 0;

    if(single_bin)
    {
        /* Single BIN file — use INDEX 01 positions for boundaries */
        FILE *bf = fopen(layout->tracks[0].bin_path, "rb");
        if(bf == NULL)
        {
            fprintf(stderr, "Cannot open BIN: %s\n", layout->tracks[0].bin_path);
            return DIMG_ERR_IO;
        }
        fseeko(bf, 0, SEEK_END);
        int64_t file_sectors = ftello(bf) / SECTOR_RAW;
        fclose(bf);

        for(int i = 0; i < count; i++)
        {
            DiscTrack *t = &layout->tracks[i];

            /* Track starts at its INDEX 01 position in the BIN */
            t->start      = index01[i];
            t->bin_offset = index01[i] * SECTOR_RAW;

            /* Track ends at next track's start or EOF */
            if(i + 1 < count)
                t->end = index01[i + 1] - 1;
            else
                t->end = file_sectors - 1;

            t->pregap = 0;
        }

        running_lba = file_sectors;
    }
    else
    {
        /* Multi-file CUE — each track in its own BIN */
        for(int i = 0; i < count; i++)
        {
            DiscTrack *t = &layout->tracks[i];

            FILE *bf = fopen(t->bin_path, "rb");
            if(bf == NULL)
            {
                fprintf(stderr, "Cannot open BIN: %s\n", t->bin_path);
                return DIMG_ERR_IO;
            }
            fseeko(bf, 0, SEEK_END);
            int64_t track_sectors = ftello(bf) / SECTOR_RAW;
            fclose(bf);

            t->start      = running_lba;
            t->end        = running_lba + track_sectors - 1;
            t->bin_offset = 0;
            t->pregap     = 0;

            running_lba += track_sectors;
        }
    }

    layout->track_count   = count;
    layout->total_sectors = running_lba;

    return DIMG_OK;
}

int cue_write(const char *cue_path, const DiscLayout *layout, void *aaru_ctx)
{
    assert(cue_path != NULL);
    assert(layout != NULL);
    assert(aaru_ctx != NULL);
    assert(layout->track_count > 0);

    /*
     * Generate single-BIN output (all tracks concatenated).
     * BIN filename derived from CUE filename: game.cue → game.bin
     */
    char bin_path[512];
    size_t cue_len = strlen(cue_path);

    if(cue_len < 4 || cue_len >= sizeof(bin_path))
    {
        fprintf(stderr, "Invalid CUE output path: %s\n", cue_path);
        return DIMG_ERR_ARGS;
    }

    memcpy(bin_path, cue_path, cue_len - 4);
    memcpy(bin_path + cue_len - 4, ".bin", 5);

    /* Write BIN file — all sectors sequentially */
    FILE *bf = fopen(bin_path, "wb");
    if(bf == NULL)
    {
        fprintf(stderr, "Cannot create BIN: %s\n", bin_path);
        return DIMG_ERR_IO;
    }

    uint8_t buf[SECTOR_RAW];
    int64_t total = layout->total_sectors;

    for(int64_t s = 0; s < total; s++)
    {
        uint32_t rlen   = SECTOR_RAW;
        uint8_t  status = 0;
        int32_t  res    = aaruf_read_sector_long(aaru_ctx, (uint64_t)s, false,
                                                  buf, &rlen, &status);

        if(res < 0)
        {
            fprintf(stderr, "Read error at sector %" PRId64 ": %d\n", s, res);
            fclose(bf);
            return DIMG_ERR_IO;
        }

        /* status 1 = not dumped — write zeros */
        if(res == 1)
            memset(buf, 0, SECTOR_RAW);

        if(fwrite(buf, 1, SECTOR_RAW, bf) != SECTOR_RAW)
        {
            fprintf(stderr, "Write error at sector %" PRId64 "\n", s);
            fclose(bf);
            return DIMG_ERR_IO;
        }

        if(s > 0 && s % 10000 == 0)
            fprintf(stderr, "\r  %" PRId64 "/%" PRId64 " sectors", s, total);
    }

    if(total > 10000)
        fprintf(stderr, "\r  %" PRId64 "/%" PRId64 " sectors\n", total, total);

    fclose(bf);

    /* Write CUE sheet */
    FILE *cf = fopen(cue_path, "w");
    if(cf == NULL)
    {
        fprintf(stderr, "Cannot create CUE: %s\n", cue_path);
        return DIMG_ERR_IO;
    }

    const char *bin_name = path_basename(bin_path);
    fprintf(cf, "FILE \"%s\" BINARY\n", bin_name);

    for(int i = 0; i < layout->track_count; i++)
    {
        const DiscTrack *t = &layout->tracks[i];
        int mm, ss, ff;

        fprintf(cf, "  TRACK %02d %s\n", t->number, track_type_to_cue(t->type));

        /* INDEX 01 at track start offset within the BIN */
        frames_to_msf(t->start, &mm, &ss, &ff);
        fprintf(cf, "    INDEX 01 %02d:%02d:%02d\n", mm, ss, ff);
    }

    fclose(cf);
    return DIMG_OK;
}
