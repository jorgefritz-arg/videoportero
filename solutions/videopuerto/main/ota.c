/* ================================================================
 * OTA — Actualización automática desde GitHub Releases
 *  · Chequeo automático cada 6 h (nunca durante una llamada)
 *  · Comando "/update" por Telegram = chequeo inmediato
 *  · Instala solo si la versión publicada es MAYOR a la actual
 *  · Rollback: si el firmware nuevo falla, el bootloader vuelve al anterior
 * ================================================================ */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "config.h"
#include "doorbell.h"
#include "ota.h"

static const char *TAG = "ota";

#define ASSET_NAME          "videopuerto.bin"
#define POLL_TELEGRAM_MS    30000
#define CHEQUEO_CADA_CICLOS 720      /* 720 x 30 s = 6 horas */

#ifndef MIN
#define MIN(a,b) ((a)<(b)?(a):(b))
#endif

/* ---------- utilidades HTTP/JSON ---------- */
static bool json_buscar_string(const char *json, const char *clave, char *out, size_t out_len) {
    char patron[48];
    snprintf(patron, sizeof(patron), "\"%s\"", clave);
    const char *p = strstr(json, patron);
    if (!p) return false;
    p = strchr(p + strlen(patron), ':');
    if (!p) return false;
    p = strchr(p, '"');
    if (!p) return false;
    p++;
    const char *fin = strchr(p, '"');
    if (!fin || (size_t)(fin - p) >= out_len) return false;
    memcpy(out, p, fin - p);
    out[fin - p] = '\0';
    return true;
}

/* Extrae el browser_download_url del asset cuyo nombre contenga ASSET_NAME */
static void extraer_url_binario(const char *json, char *out, size_t out_len) {
    out[0] = '\0';
    const char *p = json;
    while ((p = strstr(p, "\"browser_download_url\"")) != NULL) {
        p = strchr(p + 22, ':');
        if (!p) return;
        p = strchr(p, '"');
        if (!p) return;
        p++;
        const char *fin = strchr(p, '"');
        if (!fin) return;
        const char *nombre = strstr(p, ASSET_NAME);
        if (nombre && nombre < fin) {
            size_t l = (size_t)(fin - p);
            if (l < out_len) { memcpy(out, p, l); out[l] = '\0'; }
            return;
        }
        p = fin;
    }
}

static char *http_get(const char *url) {
    esp_http_client_config_t cfg = {
        .url = url, .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 15000, .buffer_size = 2048,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return NULL;
    if (esp_http_client_open(c, 0) != ESP_OK) { esp_http_client_cleanup(c); return NULL; }
    esp_http_client_fetch_headers(c);
    const size_t cap = 32768;
    char *buf = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
    if (!buf) { esp_http_client_cleanup(c); return NULL; }
    size_t len = 0;
    int n;
    while (len < cap - 1 && (n = esp_http_client_read(c, buf + len, cap - 1 - len)) > 0) len += n;
    buf[len] = '\0';
    esp_http_client_cleanup(c);
    if (len == 0) { free(buf); return NULL; }
    return buf;
}

/* ---------- Telegram ---------- */
static bool tg_send(const char *texto) {
    char url[160], body[384];
    snprintf(url, sizeof(url), "https://api.telegram.org/bot%s/sendMessage", TG_TOKEN);
    int len = snprintf(body, sizeof(body), "{\"chat_id\":\"%s\",\"text\":\"%s\"}", TG_CHAT_ID, texto);
    esp_http_client_config_t cfg = { .url = url, .method = HTTP_METHOD_POST,
        .crt_bundle_attach = esp_crt_bundle_attach, .timeout_ms = 10000 };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return false;
    esp_http_client_set_header(c, "Content-Type", "application/json");
    bool ok = false;
    if (esp_http_client_open(c, len) == ESP_OK) {
        esp_http_client_write(c, body, len);
        esp_http_client_fetch_headers(c);
        ok = esp_http_client_get_status_code(c) == 200;
    }
    esp_http_client_cleanup(c);
    return ok;
}

static bool tg_leer_ultimo(int *update_id, char *texto, size_t tlen) {
    char url[160];
    snprintf(url, sizeof(url),
        "https://api.telegram.org/bot%s/getUpdates?timeout=0&offset=-1&limit=1", TG_TOKEN);
    char *json = http_get(url);
    if (!json) return false;
    texto[0] = '\0';
    json_buscar_string(json, "text", texto, tlen);
    bool ok = false;
    const char *p = strstr(json, "\"update_id\":");
    if (p) { *update_id = atoi(p + strlen("\"update_id\":")); ok = true; }
    free(json);
    return ok;
}

static void tg_confirmar(int update_id) {
    char url[160];
    snprintf(url, sizeof(url),
        "https://api.telegram.org/bot%s/getUpdates?timeout=0&offset=%d", TG_TOKEN, update_id + 1);
    char *json = http_get(url);
    if (json) free(json);
}

/* ---------- versiones ---------- */
static bool version_parsear(const char *s, int v[3]) {
    if (*s == 'v') s++;
    return sscanf(s, "%d.%d.%d", &v[0], &v[1], &v[2]) == 3;
}
static bool version_mayor(const char *nueva, const char *actual) {
    int n[3] = {0,0,0}, a[3] = {0,0,0};
    if (!version_parsear(nueva, n) || !version_parsear(actual, a)) return false;
    for (int i = 0; i < 3; i++) if (n[i] != a[i]) return n[i] > a[i];
    return false;
}

/* ---------- chequeo + instalación ---------- */
static void chequear_y_actualizar(bool comando) {
    if (doorbell_busy()) { ESP_LOGW(TAG, "Llamada en curso: actualizo despues"); return; }

    char *json = http_get(REPO_API);
    if (!json) { ESP_LOGW(TAG, "No pude consultar GitHub (¿internet? ¿repo publico?)"); return; }

    char tag[32] = {0}, url_bin[512] = {0};
    bool hay_tag = json_buscar_string(json, "tag_name", tag, sizeof(tag));
    extraer_url_binario(json, url_bin, sizeof(url_bin));
    free(json);

    if (!hay_tag || !url_bin[0]) {
        ESP_LOGW(TAG, "No encontre release con '%s'", ASSET_NAME);
        if (comando) tg_send("❌ No encontré una versión publicada en GitHub.");
        return;
    }

    const esp_app_desc_t *app = esp_app_get_description();
    ESP_LOGI(TAG, "Firmware actual: %s | Publicado: %s", app->version, tag);

    if (!version_mayor(tag, app->version)) {
        ESP_LOGI(TAG, "Ya estas en la ultima version");
        if (comando) tg_send("✅ Ya tenés la última versión.");
        return;
    }

    ESP_LOGI(TAG, "Nueva version %s -> descargando...", tag);
    tg_send("⬆️ Nueva versión encontrada. Actualizando... (no cortes la energía)");

    esp_http_client_config_t http = {
        .url = url_bin, .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 25000, .buffer_size = 4096,
    };
    esp_https_ota_config_t ota_cfg = { .http_config = &http };
    esp_err_t err = esp_https_ota(&ota_cfg);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "OTA OK -> reinicio");
        tg_send("✅ Actualización instalada. Reiniciando...");
        vTaskDelay(pdMS_TO_TICKS(1500));
        esp_restart();
    }
    ESP_LOGE(TAG, "OTA fallo: %s (sigo con la version actual)", esp_err_to_name(err));
    tg_send("❌ La actualización falló; el videoportero sigue funcionando normal.");
}

static bool wifi_ok(void) {
    esp_netif_t *net = esp_netif_get_default_netif();
    esp_netif_ip_info_t ip;
    if (!net || esp_netif_get_ip_info(net, &ip) != ESP_OK) return false;
    return ip.ip.addr != 0;
}

/* ---------- tarea ---------- */
static void ota_task(void *arg) {
    /* Firmware recién instalado: probarlo 3 min en silencio y validarlo */
    esp_ota_img_states_t st;
    if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &st) == ESP_OK
        && st == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGW(TAG, "Firmware recien instalado: periodo de prueba de 3 min...");
        vTaskDelay(pdMS_TO_TICKS(180000));
        esp_ota_mark_app_valid_cancel_rollback();
        ESP_LOGI(TAG, "Firmware nuevo validado (rollback cancelado)");
    }

    vTaskDelay(pdMS_TO_TICKS(60000));
    int ciclos = 0, ultimo_id = -1;

    for (;;) {
        if (!doorbell_busy() && wifi_ok()) {
            int id = 0;
            char texto[96] = {0};
            if (tg_leer_ultimo(&id, texto, sizeof(texto)) && id != ultimo_id) {
                ultimo_id = id;
                if (strlen(texto) > 0) {
                    ESP_LOGI(TAG, "Mensaje de Telegram recibido");
                    if (strstr(texto, "/update")) {
                        tg_send("🔎 Buscando actualización...");
                        chequear_y_actualizar(true);
                    }
                    tg_confirmar(id);
                }
            }
            if (++ciclos >= CHEQUEO_CADA_CICLOS) {
                ciclos = 0;
                chequear_y_actualizar(false);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(POLL_TELEGRAM_MS));
    }
}

void ota_start(void) {
    xTaskCreatePinnedToCore(ota_task, "ota", 10240, NULL, 3, NULL, 0);
    ESP_LOGI(TAG, "Modulo OTA iniciado (auto cada 6 h · /update por Telegram)");
}