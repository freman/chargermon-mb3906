#include "ota.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"

static const char *TAG = "ota";

// Confirm the running image healthy after this long without a crash/reboot.
#define VERIFY_DELAY_MS 15000

static volatile bool s_busy;

// ==========================================================================
// Rollback confirmation
// ==========================================================================
static void verify_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(VERIFY_DELAY_MS));
    if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
        ESP_LOGI(TAG, "new image confirmed healthy; rollback cancelled");
    } else {
        ESP_LOGW(TAG, "failed to mark image valid");
    }
    vTaskDelete(NULL);
}

void ota_init(void)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    if (esp_ota_get_state_partition(run, &st) == ESP_OK &&
        st == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGW(TAG, "running a pending-verify image; will confirm in %d s",
                 VERIFY_DELAY_MS / 1000);
        xTaskCreate(verify_task, "ota_verify", 3072, NULL, 4, NULL);
    }
}

// ==========================================================================
// Upload handler
// ==========================================================================
static esp_err_t fail(httpd_req_t *req, esp_ota_handle_t h, const char *msg)
{
    if (h) {
        esp_ota_abort(h);
    }
    s_busy = false;
    ESP_LOGE(TAG, "OTA failed: %s", msg);
    httpd_resp_set_status(req, "500 Internal Server Error");
    httpd_resp_sendstr(req, msg);
    return ESP_FAIL;
}

static void reboot_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

esp_err_t ota_perform(httpd_req_t *req)
{
    if (s_busy) {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_sendstr(req, "OTA already in progress");
        return ESP_FAIL;                       // no concurrent uploads
    }
    s_busy = true;

    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) {
        return fail(req, 0, "no OTA partition");
    }
    ESP_LOGI(TAG, "OTA -> partition '%s' (offset 0x%lx)", part->label,
             (unsigned long)part->address);

    esp_ota_handle_t h = 0;
    if (esp_ota_begin(part, OTA_SIZE_UNKNOWN, &h) != ESP_OK) {
        return fail(req, 0, "ota_begin failed");
    }

    char *buf = malloc(4096);
    if (!buf) {
        return fail(req, h, "no memory");
    }

    int received = 0;
    while (1) {
        int r = httpd_req_recv(req, buf, 4096);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;                          // slow client; keep waiting
        }
        if (r < 0) {
            free(buf);
            return fail(req, h, "recv error");
        }
        if (r == 0) {
            break;                             // end of body
        }
        if (esp_ota_write(h, buf, r) != ESP_OK) {
            free(buf);
            return fail(req, h, "flash write failed");
        }
        received += r;
    }
    free(buf);

    if (received == 0) {
        return fail(req, h, "empty upload");
    }
    // esp_ota_end validates the image (magic/size); a bad .bin is rejected here
    // and the running image is left untouched.
    if (esp_ota_end(h) != ESP_OK) {
        s_busy = false;
        ESP_LOGE(TAG, "image validation failed");
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "image validation failed");
        return ESP_FAIL;
    }
    if (esp_ota_set_boot_partition(part) != ESP_OK) {
        return fail(req, 0, "set_boot failed");
    }

    ESP_LOGW(TAG, "OTA ok (%d bytes) -> booting '%s'", received, part->label);
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "OK: received, rebooting into new image\n");
    xTaskCreate(reboot_task, "ota_reboot", 2048, NULL, 5, NULL);
    return ESP_OK;
}
