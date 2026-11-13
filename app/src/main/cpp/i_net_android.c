// Android replacement for linuxdoom-1.10/i_net.c: single player only for now.

#include <stdlib.h>
#include <string.h>

#include "doomstat.h"
#include "i_system.h"
#include "d_net.h"
#include "m_argv.h"
#include "i_net.h"

void I_InitNetwork(void)
{
    int i;

    doomcom = malloc(sizeof(*doomcom));
    memset(doomcom, 0, sizeof(*doomcom));

    i = M_CheckParm("-dup");
    if (i && i < myargc - 1)
    {
        doomcom->ticdup = myargv[i + 1][0] - '0';
        if (doomcom->ticdup < 1)
            doomcom->ticdup = 1;
        if (doomcom->ticdup > 9)
            doomcom->ticdup = 9;
    }
    else
        doomcom->ticdup = 1;

    doomcom->extratics = M_CheckParm("-extratic") ? 1 : 0;

    if (M_CheckParm("-net"))
        I_Error("Network games are not supported on Android yet");

    netgame = false;
    doomcom->id = DOOMCOM_ID;
    doomcom->numplayers = doomcom->numnodes = 1;
    doomcom->deathmatch = false;
    doomcom->consoleplayer = 0;
}

void I_NetCmd(void)
{
    if (doomcom->command == CMD_SEND || doomcom->command == CMD_GET)
        doomcom->remotenode = -1;   // no other nodes
    else
        I_Error("Bad net cmd: %i\n", doomcom->command);
}
