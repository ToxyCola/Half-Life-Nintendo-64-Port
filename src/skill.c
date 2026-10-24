// HL64 - skill.cfg (the game's per-difficulty damage / health values, gamerules.cpp RefreshSkillData)
#include <libdragon.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include "game.h"
#include "menu.h"

typedef struct { char name[36]; float v; } cvar_t;
static cvar_t *cvars;
static int numcvars;

void skill_init(void)
{
    FILE *f = fopen("rom:/skill.cfg", "rb");
    if (!f) return;
    char line[160];
    cvars = malloc(sizeof(cvar_t) * 300);
    while (fgets(line, sizeof(line), f) && numcvars < 300) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (strncmp(p, "sk_", 3)) continue;
        char *q = p;
        while (*q && *q != ' ' && *q != '\t') q++;
        if (!*q) continue;
        *q++ = 0;
        while (*q && *q != '"' && (*q < '0' || *q > '9') && *q != '-') q++;
        if (*q == '"') q++;
        strncpy(cvars[numcvars].name, p, 35);
        cvars[numcvars].v = atof(q);
        numcvars++;
    }
    fclose(f);
}

float skill_get(const char *name)
{
    int sk = settings.skill < 1 ? 1 : (settings.skill > 3 ? 3 : settings.skill);
    char n[40];
    snprintf(n, sizeof(n), "%s%d", name, sk);
    for (int i = 0; i < numcvars; i++) if (!strcasecmp(cvars[i].name, n)) return cvars[i].v;
    for (int i = 0; i < numcvars; i++) if (!strcasecmp(cvars[i].name, name)) return cvars[i].v;
    return 0;
}
