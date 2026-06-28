#ifndef CONFIG_H
#define CONFIG_H

#define CFG_MAX_ENTRIES 64
#define CFG_MAX_KEY     64
#define CFG_MAX_VAL     256

typedef struct {
    char key[CFG_MAX_KEY];
    char val[CFG_MAX_VAL];
} cfg_entry_t;

typedef struct {
    cfg_entry_t entries[CFG_MAX_ENTRIES];
    int         count;
} config_t;

/* Load `path` into `cfg`. Returns 0 on success, -1 on failure. */
int  config_load(const char *path, config_t *cfg);

/* Look up a string value; returns `def` if the key is absent. */
const char *config_get_str(const config_t *cfg, const char *key, const char *def);

/* Look up an integer value; returns `def` if absent or not numeric. */
int  config_get_int(const config_t *cfg, const char *key, int def);

#endif /* CONFIG_H */
