/*
 * dimg-tool info — display image metadata
 */

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "dimg.h"
#include "disc.h"
#include "aaru.h"
#include "aaruformat.h"
#include "aaruformat/structs/optical.h"

/* Map libaaruformat TrackType to lowercase display string */
static const char *track_type_str(uint8_t type)
{
    switch(type)
    {
        case kTrackTypeAudio:           return "audio";
        case kTrackTypeCdMode1:         return "mode1";
        case kTrackTypeCdMode2Formless: return "mode2";
        case kTrackTypeCdMode2Form1:    return "mode2";
        case kTrackTypeCdMode2Form2:    return "mode2";
        default:                        return "data";
    }
}

/* Map track type to uppercase for human display */
static const char *track_type_label(uint8_t type)
{
    switch(type)
    {
        case kTrackTypeAudio:           return "AUDIO";
        case kTrackTypeCdMode1:         return "MODE1";
        case kTrackTypeCdMode2Formless: return "MODE2";
        case kTrackTypeCdMode2Form1:    return "MODE2";
        case kTrackTypeCdMode2Form2:    return "MODE2";
        default:                        return "DATA";
    }
}

int cmd_info(int argc, char **argv)
{
    bool json_output = false;
    const char *path = NULL;

    /* Parse arguments: allow -j/--json before or after the path */
    for(int i = 1; i < argc; i++)
    {
        if(strcmp(argv[i], "-j") == 0 || strcmp(argv[i], "--json") == 0)
            json_output = true;
        else if(argv[i][0] != '-' && path == NULL)
            path = argv[i];
    }

    if(path == NULL)
    {
        fprintf(stderr, "Usage: dimg-tool info [-j|--json] <image>\n");
        return DIMG_ERR_ARGS;
    }

    void *ctx = aaruf_open(path, false, NULL);
    if(ctx == NULL)
    {
        fprintf(stderr, "Failed to open image: %s\n", path);
        return DIMG_ERR_IO;
    }

    ImageInfo info;
    int res = aaruf_get_image_info(ctx, &info);
    if(res != AARUF_STATUS_OK)
    {
        fprintf(stderr, "Failed to read image info (error %d)\n", res);
        aaruf_close(ctx);
        return DIMG_ERR_FORMAT;
    }

    DiscSystem system = (DiscSystem)info.MediaType;
    const char *codec = aaru_detect_codec(ctx);
    uint64_t media_size = (uint64_t)info.Sectors * info.SectorSize;
    int is_cd = disc_is_cd(system);

    /* Read tracks for CD-based systems */
    int track_count = 1;
    TrackEntry tracks[DISC_MAX_TRACKS];
    memset(tracks, 0, sizeof(tracks));

    if(is_cd)
    {
        size_t track_buf_len = sizeof(TrackEntry) * DISC_MAX_TRACKS;
        int32_t tres = aaruf_get_tracks(ctx, (uint8_t *)tracks, &track_buf_len);
        if(tres == AARUF_STATUS_OK && track_buf_len > 0)
            track_count = (int)(track_buf_len / sizeof(TrackEntry));
    }

    /* Read MCN/CATALOG */
    char catalog[14] = {0};
    if(is_cd)
    {
        uint32_t mcn_len = 13;
        uint8_t mcn_buf[13];
        if(aaruf_read_media_tag(ctx, mcn_buf, kMediaTagCdMcn, &mcn_len) == AARUF_STATUS_OK
           && mcn_len == 13)
        {
            memcpy(catalog, mcn_buf, 13);
            catalog[13] = '\0';
        }
    }

    if(json_output)
    {
        printf("{\n");
        printf("  \"format\": \"aaru\",\n");
        printf("  \"system\": \"%s\",\n", disc_system_cli_name(system));
        printf("  \"media_type\": %u,\n", info.MediaType);
        printf("  \"tracks\": %d,\n", track_count);
        printf("  \"sectors\": %" PRIu64 ",\n", info.Sectors);
        printf("  \"sector_size\": %u,\n", info.SectorSize);
        printf("  \"media_size\": %" PRIu64 ",\n", media_size);
        printf("  \"application\": \"%s\",\n", info.Application);
        printf("  \"application_version\": \"%s\",\n", info.ApplicationVersion);
        printf("  \"codec\": \"%s\"", codec);

        if(catalog[0] != '\0')
        {
            printf(",\n");
            printf("  \"catalog\": \"%s\"", catalog);
        }

        if(is_cd && track_count > 0)
        {
            printf(",\n");
            printf("  \"track_list\": [\n");
            for(int i = 0; i < track_count; i++)
            {
                const TrackEntry *te = &tracks[i];
                int64_t sectors = te->end - te->start + 1;
                int64_t size = sectors * (int64_t)info.SectorSize;

                printf("    {\"number\": %d, \"type\": \"%s\", "
                       "\"start\": %" PRId64 ", \"end\": %" PRId64 ", "
                       "\"pregap\": %" PRId64 ", \"session\": %d, "
                       "\"sectors\": %" PRId64 ", \"size\": %" PRId64 "}",
                       te->sequence, track_type_str(te->type),
                       te->start, te->end, te->pregap, te->session,
                       sectors, size);
                printf("%s\n", (i + 1 < track_count) ? "," : "");
            }
            printf("  ]");
        }

        printf("\n}\n");
    }
    else
    {
        printf("Image:       %s\n", path);
        printf("System:      %s\n", disc_system_name(system));
        printf("Media type:  %u\n", info.MediaType);
        printf("Tracks:      %d\n", track_count);
        printf("Sectors:     %" PRIu64 "\n", info.Sectors);
        printf("Sector size: %u\n", info.SectorSize);
        printf("Media size:  %" PRIu64 " bytes\n", media_size);
        printf("Application: %s\n", info.Application);
        printf("Codec:       %s\n", codec);

        if(catalog[0] != '\0')
            printf("Catalog:     %s\n", catalog);

        /* Per-track listing */
        if(is_cd && track_count > 0)
        {
            printf("\n");
            for(int i = 0; i < track_count; i++)
            {
                const TrackEntry *te = &tracks[i];
                int64_t sectors = te->end - te->start + 1;
                double mib = (double)(sectors * info.SectorSize) / (1024.0 * 1024.0);

                printf("  %02d  %-5s  sectors %-10" PRId64 "-%-10" PRId64 "  %6.1f MiB",
                       te->sequence, track_type_label(te->type),
                       te->start, te->end, mib);
                if(te->pregap > 0)
                    printf("  pregap %" PRId64, te->pregap);
                if(te->session > 1)
                    printf("  session %d", te->session);
                printf("\n");
            }
        }
    }

    aaruf_close(ctx);
    return DIMG_OK;
}
