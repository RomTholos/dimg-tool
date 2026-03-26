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
#include <inttypes.h>
#include <stdbool.h>
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
    int  current_session   = 1;

    /* Temporary storage for INDEX positions (MSF frames from FILE start) */
    int64_t index00[DISC_MAX_TRACKS];
    int64_t index01[DISC_MAX_TRACKS];
    memset(index00, 0xFF, sizeof(index00)); /* -1 = no INDEX 00 present */
    memset(index01, 0, sizeof(index01));

    while(fgets(line, sizeof(line), f))
    {
        /* Strip leading whitespace */
        char *p = line;
        while(*p == ' ' || *p == '\t') p++;

        /* CATALOG directive (CD MCN) */
        if(strncmp(p, "CATALOG ", 8) == 0)
        {
            const char *cat = p + 8;
            size_t clen = 0;
            while(cat[clen] >= '0' && cat[clen] <= '9' && clen < 13)
                clen++;
            if(clen == 13)
            {
                memcpy(layout->catalog, cat, 13);
                layout->catalog[13] = '\0';
            }
            continue;
        }

        /* REM SESSION directive (multi-session discs) */
        if(strncmp(p, "REM SESSION ", 12) == 0)
        {
            current_session = atoi(p + 12);
            if(current_session < 1)
                current_session = 1;
            continue;
        }

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

            t->number  = (uint8_t)atoi(p + 6);
            t->session = (uint8_t)current_session;

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
                if(idx_num == 0)
                    index00[count - 1] = msf_to_frames(mm, ss, ff);
                else if(idx_num == 1)
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
        /* Single BIN file — use INDEX positions for boundaries.
         * start includes pregap per aaru TrackEntry semantics. */
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

            /* Track starts at INDEX 00 if present, else INDEX 01 */
            int64_t track_start = (index00[i] >= 0) ? index00[i] : index01[i];

            t->start      = track_start;
            t->bin_offset = track_start * SECTOR_RAW;
            t->pregap     = index01[i] - track_start;

            /* Track ends at next track's earliest index or EOF */
            if(i + 1 < count)
            {
                int64_t next_start = (index00[i + 1] >= 0) ? index00[i + 1] : index01[i + 1];
                t->end = next_start - 1;
            }
            else
                t->end = file_sectors - 1;
        }

        layout->is_multi_bin = 0;
        running_lba = file_sectors;
    }
    else
    {
        /* Multi-file CUE — each track in its own BIN.
         * start includes pregap per aaru TrackEntry semantics.
         * Pregap = INDEX 01 offset within the file (INDEX 00 is always 00:00:00). */
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

            if(index00[i] >= 0 && index01[i] > index00[i])
                t->pregap = index01[i] - index00[i];
            else
                t->pregap = 0;

            running_lba += track_sectors;
        }

        layout->is_multi_bin = 1;
    }

    layout->track_count   = count;
    layout->total_sectors = running_lba;

    return DIMG_OK;
}

/* Read sectors from aaru and write to a BIN file.
 * Returns DIMG_OK on success. */
int write_sectors_to_bin(FILE *bf, void *aaru_ctx,
                                int64_t first_lba, int64_t last_lba,
                                int64_t total_for_progress)
{
    uint8_t buf[SECTOR_RAW];

    for(int64_t s = first_lba; s <= last_lba; s++)
    {
        uint32_t rlen   = SECTOR_RAW;
        uint8_t  status = 0;
        int32_t  res    = aaruf_read_sector_long(aaru_ctx, (uint64_t)s, false,
                                                  buf, &rlen, &status);

        if(res < 0)
        {
            fprintf(stderr, "Read error at sector %" PRId64 ": %d\n", s, res);
            return DIMG_ERR_IO;
        }

        /* status 1 = not dumped — write zeros */
        if(res == 1)
            memset(buf, 0, SECTOR_RAW);

        if(fwrite(buf, 1, SECTOR_RAW, bf) != SECTOR_RAW)
        {
            fprintf(stderr, "Write error at sector %" PRId64 "\n", s);
            return DIMG_ERR_IO;
        }

        if(s > 0 && s % 10000 == 0)
            fprintf(stderr, "\r  %" PRId64 "/%" PRId64 " sectors",
                    s - first_lba, total_for_progress);
    }

    if((last_lba - first_lba + 1) > 10000)
        fprintf(stderr, "\r  %" PRId64 "/%" PRId64 " sectors\n",
                last_lba - first_lba + 1, total_for_progress);

    return DIMG_OK;
}

/* Check if any track spans multiple sessions */
static int is_multi_session(const DiscLayout *layout)
{
    for(int i = 1; i < layout->track_count; i++)
        if(layout->tracks[i].session != layout->tracks[0].session)
            return 1;
    return 0;
}

/* Write a CUE sheet header line for CATALOG if present */
static void cue_emit_catalog(FILE *cf, const DiscLayout *layout)
{
    if(layout->catalog[0] != '\0')
        fprintf(cf, "CATALOG %s\n", layout->catalog);
}

/* Write single-BIN output: all tracks concatenated into one .bin file */
static int cue_write_single_bin(const char *cue_path, const DiscLayout *layout,
                                void *aaru_ctx)
{
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

    int rc = write_sectors_to_bin(bf, aaru_ctx, 0,
                                  layout->total_sectors - 1,
                                  layout->total_sectors);
    fclose(bf);
    if(rc != DIMG_OK)
        return rc;

    /* Write CUE sheet */
    FILE *cf = fopen(cue_path, "w");
    if(cf == NULL)
    {
        fprintf(stderr, "Cannot create CUE: %s\n", cue_path);
        return DIMG_ERR_IO;
    }

    cue_emit_catalog(cf, layout);

    const char *bin_name = path_basename(bin_path);
    int multi_sess = is_multi_session(layout);

    uint8_t last_session = 0;

    if(!multi_sess)
    {
        fprintf(cf, "FILE \"%s\" BINARY\n", bin_name);
        last_session = layout->tracks[0].session;
    }

    for(int i = 0; i < layout->track_count; i++)
    {
        const DiscTrack *t = &layout->tracks[i];
        int mm, ss, ff;

        if(multi_sess && t->session != last_session)
        {
            fprintf(cf, "REM SESSION %02d\n", t->session);
            fprintf(cf, "FILE \"%s\" BINARY\n", bin_name);
            last_session = t->session;
        }

        fprintf(cf, "  TRACK %02d %s\n", t->number, track_type_to_cue(t->type));

        if(t->pregap > 0)
        {
            /* INDEX 00 at track start (includes pregap) */
            frames_to_msf(t->start, &mm, &ss, &ff);
            fprintf(cf, "    INDEX 00 %02d:%02d:%02d\n", mm, ss, ff);

            /* INDEX 01 after pregap */
            frames_to_msf(t->start + t->pregap, &mm, &ss, &ff);
            fprintf(cf, "    INDEX 01 %02d:%02d:%02d\n", mm, ss, ff);
        }
        else
        {
            frames_to_msf(t->start, &mm, &ss, &ff);
            fprintf(cf, "    INDEX 01 %02d:%02d:%02d\n", mm, ss, ff);
        }
    }

    fclose(cf);
    return DIMG_OK;
}

/* Write multi-BIN output: one .bin file per track (Redump convention) */
static int cue_write_multi_bin(const char *cue_path, const DiscLayout *layout,
                               void *aaru_ctx)
{
    size_t cue_len = strlen(cue_path);

    if(cue_len < 4 || cue_len >= 480)
    {
        fprintf(stderr, "Invalid CUE output path: %s\n", cue_path);
        return DIMG_ERR_ARGS;
    }

    /* Derive stem (CUE path without .cue extension) */
    char stem[512];
    memcpy(stem, cue_path, cue_len - 4);
    stem[cue_len - 4] = '\0';

    /* Derive stem basename for CUE FILE directives */
    const char *stem_base = path_basename(stem);

    /* Write per-track BIN files */
    for(int i = 0; i < layout->track_count; i++)
    {
        const DiscTrack *t = &layout->tracks[i];

        char track_bin[512];
        snprintf(track_bin, sizeof(track_bin), "%s (Track %02d).bin",
                 stem, t->number);

        fprintf(stderr, "  Track %d/%d [%s] → %s\n",
                t->number, layout->track_count,
                t->type == DISC_TRACK_AUDIO ? "AUDIO" :
                t->type == DISC_TRACK_MODE1 ? "MODE1" :
                t->type == DISC_TRACK_MODE2 ? "MODE2" : "DATA",
                path_basename(track_bin));

        FILE *bf = fopen(track_bin, "wb");
        if(bf == NULL)
        {
            fprintf(stderr, "Cannot create BIN: %s\n", track_bin);
            return DIMG_ERR_IO;
        }

        int rc = write_sectors_to_bin(bf, aaru_ctx, t->start, t->end,
                                      t->end - t->start + 1);
        fclose(bf);
        if(rc != DIMG_OK)
            return rc;
    }

    /* Write CUE sheet */
    FILE *cf = fopen(cue_path, "w");
    if(cf == NULL)
    {
        fprintf(stderr, "Cannot create CUE: %s\n", cue_path);
        return DIMG_ERR_IO;
    }

    cue_emit_catalog(cf, layout);

    int multi_sess = is_multi_session(layout);
    uint8_t last_session = 0;

    for(int i = 0; i < layout->track_count; i++)
    {
        const DiscTrack *t = &layout->tracks[i];

        if(multi_sess && t->session != last_session)
        {
            fprintf(cf, "REM SESSION %02d\n", t->session);
            last_session = t->session;
        }

        /* FILE directive per track */
        char track_bin_name[256];
        snprintf(track_bin_name, sizeof(track_bin_name),
                 "%s (Track %02d).bin", stem_base, t->number);
        fprintf(cf, "FILE \"%s\" BINARY\n", track_bin_name);

        fprintf(cf, "  TRACK %02d %s\n", t->number, track_type_to_cue(t->type));

        if(t->pregap > 0)
        {
            int mm, ss, ff;
            fprintf(cf, "    INDEX 00 00:00:00\n");
            frames_to_msf(t->pregap, &mm, &ss, &ff);
            fprintf(cf, "    INDEX 01 %02d:%02d:%02d\n", mm, ss, ff);
        }
        else
        {
            fprintf(cf, "    INDEX 01 00:00:00\n");
        }
    }

    fclose(cf);
    return DIMG_OK;
}

int cue_write(const char *cue_path, const DiscLayout *layout, void *aaru_ctx,
              bool multi_bin)
{
    assert(cue_path != NULL);
    assert(layout != NULL);
    assert(aaru_ctx != NULL);
    assert(layout->track_count > 0);

    if(multi_bin)
        return cue_write_multi_bin(cue_path, layout, aaru_ctx);
    else
        return cue_write_single_bin(cue_path, layout, aaru_ctx);
}
