#include "rig_shared.h"

//--- BOOT SETUP ---
static const char *BOOT = "BOOT";

void init_nvs(void) {
    esp_err_t nvs_status = nvs_flash_init();

    if (nvs_status == ESP_ERR_NVS_NO_FREE_PAGES ||
        nvs_status == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(BOOT, "Failed to initialize NVS flash. Erasing and retrying.");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    ESP_ERROR_CHECK(nvs_status);
    ESP_LOGI(BOOT, "NVS initialized successfully.");
}

void init_wifi(void) {
    wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_channel(DEFAULT_CHANNEL, SECONDARY_CHANNEL));
}

void init_espnow(esp_now_recv_cb_t recv_cb, esp_now_send_cb_t send_cb) {
    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_recv_cb(recv_cb));
    ESP_ERROR_CHECK(esp_now_register_send_cb(send_cb));
}
