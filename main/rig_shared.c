#include "rig_shared.h"

//--- BOOT SETUP ---
static const char *BOOT = "BOOT";

esp_err_t init_nvs(void) {
    esp_err_t nvs_status = nvs_flash_init();

    if (nvs_status == ESP_ERR_NVS_NO_FREE_PAGES ||
        nvs_status == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(BOOT, "Failed to initialize NVS flash. Erasing and retrying.");
        nvs_status = nvs_flash_erase();
        if (nvs_status != ESP_OK) {
            ESP_LOGE(BOOT, "Failed to erase NVS flash.");
            return nvs_status;
        }
        nvs_status = nvs_flash_init();
    }

    if (nvs_status != ESP_OK) {
        ESP_LOGE(BOOT, "Failed to initialize NVS flash.");
        return nvs_status;
    }

    ESP_LOGI(BOOT, "NVS initialized successfully.");
    return ESP_OK;
}

esp_err_t init_wifi(void) {
    wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK) {
        ESP_LOGE(BOOT, "Failed to initialize netif.");
        return err;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK) {
        ESP_LOGE(BOOT, "Failed to create default event loop.");
        return err;
    }
    err = esp_wifi_init(&wifi_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(BOOT, "Failed to initialize Wi-Fi.");
        return err;
    }
    err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (err != ESP_OK) {
        ESP_LOGE(BOOT, "Failed to set Wi-Fi storage to RAM.");
        return err;
    }
    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        ESP_LOGE(BOOT, "Failed to set Wi-Fi mode to STA.");
        return err;
    }
    err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(BOOT, "Failed to start Wi-Fi.");
        return err;
    }
    err = esp_wifi_set_channel(DEFAULT_CHANNEL, SECONDARY_CHANNEL);
    if (err != ESP_OK) {
        ESP_LOGE(BOOT, "Failed to set Wi-Fi channel.");
        return err;
    }
    // Required for reliable ESP-NOW RX while idle. Default modem sleep
    // otherwise drops pairing broadcasts (TX-on-first often fails).
    err = esp_wifi_set_ps(WIFI_PS_NONE);
    if (err != ESP_OK) {
        ESP_LOGE(BOOT, "Failed to disable Wi-Fi power save.");
        return err;
    }
    ESP_LOGI(BOOT, "Wi-Fi ready on channel %d (PS none).", DEFAULT_CHANNEL);
    return ESP_OK;
}

esp_err_t init_espnow(esp_now_recv_cb_t recv_cb, esp_now_send_cb_t send_cb) {
    esp_err_t err = esp_now_init();
    if (err != ESP_OK) {
        ESP_LOGE(BOOT, "Failed to initialize ESP-NOW.");
        return err;
    }
    // Channel can be lost across esp_now_init on some IDF builds.
    err = esp_wifi_set_channel(DEFAULT_CHANNEL, SECONDARY_CHANNEL);
    if (err != ESP_OK) {
        ESP_LOGE(BOOT, "Failed to re-set Wi-Fi channel after esp_now_init.");
        return err;
    }
    err = esp_wifi_set_ps(WIFI_PS_NONE);
    if (err != ESP_OK) {
        ESP_LOGE(BOOT, "Failed to re-disable Wi-Fi power save.");
        return err;
    }
    err = esp_now_register_recv_cb(recv_cb);
    if (err != ESP_OK) {
        ESP_LOGE(BOOT, "Failed to register ESP-NOW recv callback.");
        return err;
    }
    err = esp_now_register_send_cb(send_cb);
    if (err != ESP_OK) {
        ESP_LOGE(BOOT, "Failed to register ESP-NOW send callback.");
        return err;
    }
    return ESP_OK;
}

void verify_espnow_ver(void) {
    uint32_t espnow_ver = 0;
    ESP_ERROR_CHECK(esp_now_get_version(&espnow_ver));
    ESP_LOGI("VERSION_CHECK",
             "ESP-NOW version %lu (need 2 for >250 B packets).",
             (unsigned long)espnow_ver);
    if (espnow_ver < 2) {
        ESP_LOGE("VERSION_CHECK",
                 "ESP-NOW v2 required for audio_packet_t (%u B).",
                 (unsigned)sizeof(audio_packet_t));
        abort();
    }
}
