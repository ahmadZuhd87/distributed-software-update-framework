/* ===========================================================================
 * config.c  -  "key = value" config file parser implementation.
 * ===========================================================================
 */

#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* Trim leading/trailing whitespace in place; returns pointer to trimmed start. */
static char *trim(char *s)
{
    while (*s && isspace((unsigned char)*s)) s++;
    if (*s == '\0') return s;
    char *end = s + strlen(s) - 1;
    while (end > s && isspace((unsigned char)*end)) *end-- = '\0';
    return s;
}

int config_load(const char *path, config_t *cfg)
{
    FILE *fp = fopen(path, "r");
    if (!fp) {
        fprintf(stderr, "config: cannot open '%s'\n", path);
        return -1;
    }

    cfg->count = 0;
    char line[512];

    while (fgets(line, sizeof(line), fp)) {
        /* Strip trailing newline. */
        line[strcspn(line, "\r\n")] = '\0';

        char *p = trim(line);
        if (*p == '\0' || *p == '#') continue;   /* blank / comment */

        char *eq = strchr(p, '=');
        if (!eq) continue;                        /* malformed, skip */

        *eq = '\0';
        char *k = trim(p);
        char *v = trim(eq + 1);

        if (cfg->count >= CFG_MAX_ENTRIES) {
            fprintf(stderr, "config: too many entries, ignoring rest\n");
            break;
        }

        strncpy(cfg->entries[cfg->count].key, k, CFG_MAX_KEY - 1);
        cfg->entries[cfg->count].key[CFG_MAX_KEY - 1] = '\0';
        strncpy(cfg->entries[cfg->count].val, v, CFG_MAX_VAL - 1);
        cfg->entries[cfg->count].val[CFG_MAX_VAL - 1] = '\0';
        cfg->count++;
    }

    fclose(fp);
    return 0;
}

const char *config_get_str(const config_t *cfg, const char *key, const char *def)
{
    for (int i = 0; i < cfg->count; i++)
        if (strcmp(cfg->entries[i].key, key) == 0)
            return cfg->entries[i].val;
    return def;
}

int config_get_int(const config_t *cfg, const char *key, int def)
{
    const char *s = config_get_str(cfg, key, NULL);
    if (!s) return def;
    char *endp = NULL;
    long v = strtol(s, &endp, 10);
    if (endp == s) return def;                /* not a number */
    return (int)v;
}
