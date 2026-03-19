/*
 * disc.h — disc image layout types shared across format modules
 */

#ifndef DISC_H
#define DISC_H

#include <stdint.h>

#define DISC_MAX_TRACKS 99
#define SECTOR_RAW  2352  /* CD: sync + header + user data + ECC/EDC */
#define SECTOR_USER 2048  /* DVD/ISO: user data only */

/* Track type classification */
typedef enum {
    DISC_TRACK_AUDIO,  /* CD-DA audio */
    DISC_TRACK_MODE1,  /* CD Mode 1 data (raw 2352) */
    DISC_TRACK_MODE2,  /* CD Mode 2 formless (raw 2352) */
    DISC_TRACK_DVD     /* DVD 2048-byte sectors */
} DiscTrackType;

/* Source/target format */
typedef enum {
    DISC_FMT_UNKNOWN,
    DISC_FMT_CUE,
    DISC_FMT_ISO,
    DISC_FMT_AARU
} DiscFormat;

/*
 * System / media type.
 * Values match libaaruformat MediaType enum directly —
 * cast to uint32_t for aaruf_create().
 */
typedef enum {
    DISC_SYS_CD        = 10,   /* Generic CD */
    DISC_SYS_DVD       = 40,   /* Generic DVD-ROM */
    DISC_SYS_PS1       = 112,  /* PS1CD */
    DISC_SYS_PS2CD     = 113,  /* PS2CD */
    DISC_SYS_PS2DVD    = 114,  /* PS2DVD */
    DISC_SYS_MEGACD    = 150,  /* Sega Mega CD */
    DISC_SYS_SATURN    = 151,  /* Sega Saturn */
    DISC_SYS_DREAMCAST = 152,  /* Sega Dreamcast (GD-ROM) */
    DISC_SYS_PCE       = 171,  /* PC Engine / TurboGrafx CD */
    DISC_SYS_NEOGEOCD  = 175   /* Neo Geo CD */
} DiscSystem;

/* Single track descriptor */
typedef struct {
    uint8_t       number;       /* 1-based track sequence */
    DiscTrackType type;
    int64_t       start;        /* first LBA (inclusive) */
    int64_t       end;          /* last LBA (inclusive) */
    int64_t       pregap;       /* pregap sectors */
    uint8_t       session;      /* session number (1-based) */
    uint32_t      sector_size;  /* bytes per sector (2048 or 2352) */
    char          bin_path[512]; /* source file path (empty for .aaru source) */
    int64_t       bin_offset;   /* byte offset in source file */
} DiscTrack;

/* Complete disc layout — intermediate representation */
typedef struct {
    DiscSystem  system;
    DiscFormat  source_format;
    int         track_count;
    DiscTrack   tracks[DISC_MAX_TRACKS];
    int64_t     total_sectors;
} DiscLayout;

/* Detect format from file extension */
DiscFormat disc_detect_format(const char *path);

/* Parse system name string to DiscSystem, returns -1 on unknown */
int disc_parse_system(const char *name);

/* Is this a CD-based system (raw 2352-byte sectors)? */
int disc_is_cd(DiscSystem system);

/* Return sector size for system type */
uint32_t disc_sector_size(DiscSystem system);

/* Map system name for display */
const char *disc_system_name(DiscSystem system);

/* Map system to machine-readable CLI name ("ps1", "dc", etc.) */
const char *disc_system_cli_name(DiscSystem system);

#endif /* DISC_H */
