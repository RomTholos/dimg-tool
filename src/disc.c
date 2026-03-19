/*
 * disc.c — shared disc image helpers
 */

#include <string.h>
#include <strings.h>

#include "disc.h"

DiscFormat disc_detect_format(const char *path)
{
    const char *dot = strrchr(path, '.');
    if(dot == NULL)
        return DISC_FMT_UNKNOWN;

    if(strcasecmp(dot, ".cue") == 0)
        return DISC_FMT_CUE;
    if(strcasecmp(dot, ".iso") == 0)
        return DISC_FMT_ISO;
    if(strcasecmp(dot, ".aaru") == 0 ||
       strcasecmp(dot, ".aaruf") == 0 ||
       strcasecmp(dot, ".dicf") == 0)
        return DISC_FMT_AARU;

    return DISC_FMT_UNKNOWN;
}

int disc_parse_system(const char *name)
{
    if(strcmp(name, "dc") == 0)        return DISC_SYS_DREAMCAST;
    if(strcmp(name, "saturn") == 0)    return DISC_SYS_SATURN;
    if(strcmp(name, "megacd") == 0)    return DISC_SYS_MEGACD;
    if(strcmp(name, "pce") == 0)       return DISC_SYS_PCE;
    if(strcmp(name, "neogeo") == 0)    return DISC_SYS_NEOGEOCD;
    if(strcmp(name, "ps1") == 0)       return DISC_SYS_PS1;
    if(strcmp(name, "ps2cd") == 0)     return DISC_SYS_PS2CD;
    if(strcmp(name, "ps2dvd") == 0)    return DISC_SYS_PS2DVD;
    if(strcmp(name, "cd") == 0)        return DISC_SYS_CD;
    if(strcmp(name, "dvd") == 0)       return DISC_SYS_DVD;
    return -1;
}

int disc_is_cd(DiscSystem system)
{
    switch(system)
    {
        case DISC_SYS_DVD:
        case DISC_SYS_PS2DVD:
            return 0;
        default:
            return 1;
    }
}

uint32_t disc_sector_size(DiscSystem system)
{
    return disc_is_cd(system) ? SECTOR_RAW : SECTOR_USER;
}

const char *disc_system_name(DiscSystem system)
{
    switch(system)
    {
        case DISC_SYS_CD:        return "Generic CD";
        case DISC_SYS_DVD:       return "Generic DVD";
        case DISC_SYS_PS1:       return "PlayStation";
        case DISC_SYS_PS2CD:     return "PlayStation 2 (CD)";
        case DISC_SYS_PS2DVD:    return "PlayStation 2 (DVD)";
        case DISC_SYS_MEGACD:    return "Mega CD";
        case DISC_SYS_SATURN:    return "Sega Saturn";
        case DISC_SYS_DREAMCAST: return "Dreamcast (GD-ROM)";
        case DISC_SYS_PCE:       return "PC Engine CD";
        case DISC_SYS_NEOGEOCD:  return "Neo Geo CD";
        default:                 return "Unknown";
    }
}

const char *disc_system_cli_name(DiscSystem system)
{
    switch(system)
    {
        case DISC_SYS_CD:        return "cd";
        case DISC_SYS_DVD:       return "dvd";
        case DISC_SYS_PS1:       return "ps1";
        case DISC_SYS_PS2CD:     return "ps2cd";
        case DISC_SYS_PS2DVD:    return "ps2dvd";
        case DISC_SYS_MEGACD:    return "megacd";
        case DISC_SYS_SATURN:    return "saturn";
        case DISC_SYS_DREAMCAST: return "dc";
        case DISC_SYS_PCE:       return "pce";
        case DISC_SYS_NEOGEOCD:  return "neogeo";
        default:                 return "unknown";
    }
}
