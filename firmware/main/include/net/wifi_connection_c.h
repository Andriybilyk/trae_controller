#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

bool wifi_backend_available(void);
bool wifi_init_sta(void);
void wifi_init_softap(void);
bool wifi_is_configured(void);
bool wifi_is_connected(void);
bool wifi_ap_active(void);
void wifi_disconnect_and_forget(void);

void wifi_disconnect_sta(void);

bool wifi_is_user_enabled(void);
void wifi_set_user_enabled(bool enabled);

void wifi_get_ap_ssid_c(char *out, int32_t out_len);
void wifi_get_server_url_c(char *out, int32_t out_len);

void wifi_set_sta_only_mode(void);

#ifdef __cplusplus
}
#endif

