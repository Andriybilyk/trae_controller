#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool enabled;
    bool require_signed_commands;
    char uri[160];
    char username[64];
    char password[96];
    char device_id[48];
    char auth_key[96];
} remote_access_config_t;

void remote_access_init(void);
void remote_access_loop(void);
bool remote_access_get_config(remote_access_config_t *out);
bool remote_access_set_config(const remote_access_config_t *cfg);
bool remote_access_is_connected(void);
bool remote_access_set_ca_pem(const char *pem);
bool remote_access_clear_ca_pem(void);
bool remote_access_has_ca_pem(void);

#ifdef __cplusplus
}
#endif
