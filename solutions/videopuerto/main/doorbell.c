/* ================================================================
 * DOORBELL — Videoportero Goouuu ESP32-S3-CAM
 *  Hardware: MAX98357A (GPIO38) + INMP441 (GPIO2), bus en GPIO14/21
 *            Pulsador GPIO47 · LED verde GPIO48
 *  Al pulsar: ♪ tono al visitante → 📸 foto → 📲 Telegram (foto+link)
 *             → sesión WebRTC → LED: parpadea=llamando, fijo=hablando
 *  Timeout 30 s sin atención → 📲 aviso + ♪ "mensaje registrado"
 *  TEST de audio: mantener el pulsador presionado 3 segundos.
 * ================================================================ */
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_timer.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_netif.h"
#include "esp_camera.h"
#include "esp_log.h"
#include "camera_pins.h"
#include "config.h"
#include "doorbell.h"
#include "ota.h"

static const char *TAG = "doorbell";

#define PIN_BCLK   GPIO_NUM_14   /* reloj compartido amplificador+micrófono */
#define PIN_WS     GPIO_NUM_21   /* sincronía compartida                    */
#define PIN_DOUT   GPIO_NUM_38   /* → MAX98357A (parlante)                  */
#define PIN_DIN    GPIO_NUM_2    /* ← INMP441 (micrófono)                   */
#define PIN_BOTON  GPIO_NUM_47
#define PIN_LED    GPIO_NUM_48

#define TIMEOUT_LLAMADA_S 30
#define VOL 9000                 /* volumen de tonos (máx 32767) */

/* ---------- eventos / estados ---------- */
#define EV_ANSWERED BIT0
#define EV_CLOSED   BIT1
#define EV_TIMEOUT  BIT2
typedef enum { ST_IDLE, ST_CALLING, ST_TALKING } state_t;
static volatile state_t s_state = ST_IDLE;
static TaskHandle_t s_task;
static esp_timer_handle_t s_timer;
static uint8_t *s_foto = NULL;
static size_t   s_foto_len = 0;

/* Ganchos: las versiones FUERTES están en main.c (app_start_call /
 * app_stop_call) y el linker las usa automáticamente. Estas versiones
 * débiles solo aplican si main.c no las define. app_tone_attended
 * queda opcional (ningún archivo la define: es un no-op). */
__attribute__((weak)) void app_start_call(void) {
    ESP_LOGW(TAG, "app_start_call no definida en main.c: la sesion no iniciara sola");
}
__attribute__((weak)) void app_stop_call(void) {}
__attribute__((weak)) void app_tone_attended(void) {}

#ifndef MIN
#define MIN(a,b) ((a)<(b)?(a):(b))
#endif

/* ================== ¿HAY WIFI? ================== */
static bool wifi_ok(void) {
    esp_netif_t *net = esp_netif_get_default_netif();
    esp_netif_ip_info_t ip;
    if (!net || esp_netif_get_ip_info(net, &ip) != ESP_OK) return false;
    return ip.ip.addr != 0;
}

/* ================== AUDIO ==================
 * I2S1 con TX y RX en el mismo bus (dúplex). Se usa SOLO fuera de sesión
 * WebRTC (tonos + TEST). Se libera antes de cada llamada para que el
 * pipeline de audio del WebRTC tome el bus sin conflictos. */
static i2s_chan_handle_t s_tx = NULL, s_rx = NULL;

static void speaker_init(void) {
    if (s_tx) return;
    i2s_chan_config_t ch = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    ch.auto_clear = true;
    ESP_ERROR_CHECK(i2s_new_channel(&ch, &s_tx, &s_rx));
    i2s_std_config_t cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(16000),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = { .mclk = I2S_GPIO_UNUSED, .bclk = PIN_BCLK,
                      .ws = PIN_WS, .dout = PIN_DOUT, .din = PIN_DIN },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_tx, &cfg));
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_rx, &cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(s_tx));
    ESP_ERROR_CHECK(i2s_channel_enable(s_rx));
}

static void speaker_release(void) {
    if (s_tx) { i2s_channel_disable(s_tx); i2s_del_channel(s_tx); s_tx = NULL; }
    if (s_rx) { i2s_channel_disable(s_rx); i2s_del_channel(s_rx); s_rx = NULL; }
}

static void tono(float freq, uint32_t ms, int16_t vol) {
    if (!s_tx) return;
    const uint32_t fs = 16000, fade = 160;
    uint32_t n = fs * ms / 1000, i = 0;
    static int16_t buf[320];
    while (i < n) {
        uint32_t chunk = MIN(320u, n - i);
        for (uint32_t j = 0; j < chunk; j++) {
            uint32_t p = i + j;
            float env = 1.0f;
            if (p < fade)     env = (float)p / fade;
            if (n - p < fade) env = MIN(env, (float)(n - p) / fade);
            buf[j] = (int16_t)(vol * env * sinf(2.0f * (float)M_PI * freq * p / fs));
        }
        size_t w;
        i2s_channel_write(s_tx, buf, chunk * 2, &w, portMAX_DELAY);
        i += chunk;
    }
}
static void s_llamada_enviada(void)    { tono(1250,120,VOL); vTaskDelay(pdMS_TO_TICKS(60)); tono(950,220,VOL); }
static void s_mensaje_registrado(void) { tono(880,150,VOL);  vTaskDelay(pdMS_TO_TICKS(60)); tono(660,300,VOL); }
static void s_fin(void)                { tono(700,250,VOL); }
static void s_error(void)              { tono(400,600,VOL); }

/* TEST de hardware: tono → graba 1 s del micrófono en PSRAM → reproduce el eco. */
static void test_audio(void) {
    ESP_LOGI(TAG, "TEST: tono de prueba...");
    tono(1000, 400, 12000);
    const uint32_t bytes = 16000 * 2;              /* 1 segundo = 32 KB */
    int16_t *eco = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
    if (!eco) { ESP_LOGE(TAG, "TEST: sin memoria"); return; }
    ESP_LOGI(TAG, "TEST: habla al microfono (1 s)...");
    size_t total = 0, leidos;
    while (total < bytes) {
        if (i2s_channel_read(s_rx, (uint8_t*)eco + total, 512, &leidos, portMAX_DELAY) != ESP_OK) break;
        total += leidos;
    }
    ESP_LOGI(TAG, "TEST: reproduciendo (%u bytes)...", (unsigned)total);
    size_t w;
    i2s_channel_write(s_tx, eco, total, &w, portMAX_DELAY);
    free(eco);
    ESP_LOGI(TAG, "TEST: audio OK");
}

/* ================== TELEGRAM ================== */
static bool telegram_send_photo(const uint8_t *jpg, size_t jpg_len, const char *caption) {
    const char *B = "esp32puerta";
    char p1[128], p2[420], p3[192], tail[48];
    int l1 = snprintf(p1, sizeof(p1),
        "--%s\r\nContent-Disposition: form-data; name=\"chat_id\"\r\n\r\n%s\r\n", B, TG_CHAT_ID);
    int l2 = snprintf(p2, sizeof(p2),
        "--%s\r\nContent-Disposition: form-data; name=\"caption\"\r\n\r\n%s\r\n", B, caption);
    int l3 = snprintf(p3, sizeof(p3),
        "--%s\r\nContent-Disposition: form-data; name=\"photo\"; filename=\"visitante.jpg\"\r\n"
        "Content-Type: image/jpeg\r\n\r\n", B);
    int l4 = snprintf(tail, sizeof(tail), "\r\n--%s--\r\n", B);
    long total = l1 + l2 + l3 + (long)jpg_len + l4;

    char url[160];
    snprintf(url, sizeof(url), "https://api.telegram.org/bot%s/sendPhoto", TG_TOKEN);
    esp_http_client_config_t cfg = { .url = url, .method = HTTP_METHOD_POST,
        .crt_bundle_attach = esp_crt_bundle_attach, .timeout_ms = 15000, .buffer_size = 4096 };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    esp_http_client_set_header(c, "Content-Type", "multipart/form-data; boundary=" B);
    bool ok = false;
    if (esp_http_client_open(c, total) == ESP_OK) {
        esp_http_client_write(c, p1, l1);
        esp_http_client_write(c, p2, l2);
        esp_http_client_write(c, p3, l3);
        for (size_t off = 0; off < jpg_len; off += 2048) {
            size_t chunk = MIN(2048u, jpg_len - off);
            if (esp_http_client_write(c, (const char*)jpg + off, chunk) <= 0) break;
        }
        esp_http_client_write(c, tail, l4);
        esp_http_client_fetch_headers(c);
        ok = esp_http_client_get_status_code(c) == 200;
    }
    esp_http_client_cleanup(c);
    ESP_LOGI(TAG, "sendPhoto: %s", ok ? "OK" : "FALLO");
    return ok;
}

static bool telegram_send_message(const char *texto) {
    char url[160], body[512];
    snprintf(url, sizeof(url), "https://api.telegram.org/bot%s/sendMessage", TG_TOKEN);
    int len = snprintf(body, sizeof(body), "{\"chat_id\":\"%s\",\"text\":\"%s\"}", TG_CHAT_ID, texto);
    esp_http_client_config_t cfg = { .url = url, .method = HTTP_METHOD_POST,
        .crt_bundle_attach = esp_crt_bundle_attach, .timeout_ms = 10000 };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
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

/* ================== CÁMARA (para la foto) ================== */
static void asegurar_camara(void) {
    if (esp_camera_sensor_get() != NULL) return;   /* ya iniciada: no tocar */
    camera_config_t config = {
        .ledc_channel = LEDC_CHANNEL_0, .ledc_timer = LEDC_TIMER_0,
        .pin_d0 = CAM_PIN_D0, .pin_d1 = CAM_PIN_D1, .pin_d2 = CAM_PIN_D2, .pin_d3 = CAM_PIN_D3,
        .pin_d4 = CAM_PIN_D4, .pin_d5 = CAM_PIN_D5, .pin_d6 = CAM_PIN_D6, .pin_d7 = CAM_PIN_D7,
        .pin_xclk = CAM_PIN_XCLK, .pin_pclk = CAM_PIN_PCLK,
        .pin_vsync = CAM_PIN_VSYNC, .pin_href = CAM_PIN_HREF,
        .pin_sccb_sda = CAM_PIN_SIOD, .pin_sccb_scl = CAM_PIN_SIOC,
        .pin_pwdn = CAM_PIN_PWDN, .pin_reset = CAM_PIN_RESET,
        .xclk_freq_hz = 20000000,
        .pixel_format = PIXFORMAT_JPEG, .frame_size = FRAMESIZE_VGA,
        .jpeg_quality = 12, .fb_count = 2,
        .fb_location = CAMERA_FB_IN_PSRAM, .grab_mode = CAMERA_GRAB_LATEST,
    };
    esp_err_t err = esp_camera_init(&config);
    ESP_LOGI(TAG, "Camara: %s", err == ESP_OK ? "OK" : "FALLO");
}

static void capturar_foto(void) {
    asegurar_camara();
    sensor_t *s = esp_camera_sensor_get();
    if (!s) return;
    s->set_framesize(s, FRAMESIZE_SXGA);       /* foto grande 1280x1024 */
    vTaskDelay(pdMS_TO_TICKS(350));            /* la exposición se estabiliza */
    camera_fb_t *fb = esp_camera_fb_get();
    if (fb) {
        s_foto = heap_caps_malloc(fb->len, MALLOC_CAP_SPIRAM);
        if (s_foto) { memcpy(s_foto, fb->buf, fb->len); s_foto_len = fb->len; }
        esp_camera_fb_return(fb);
    }
    s->set_framesize(s, FRAMESIZE_VGA);
    ESP_LOGI(TAG, "Foto: %u bytes", (unsigned)s_foto_len);
}
static void soltar_foto(void) { if (s_foto) { free(s_foto); s_foto = NULL; s_foto_len = 0; } }

/* ================== timer / LED ================== */
static void cb_timer(void *arg) { xTaskNotify(s_task, EV_TIMEOUT, eSetBits); }
static void set_led(bool on)    { gpio_set_level(PIN_LED, on ? 1 : 0); }

/* ================== SECUENCIAS DE LLAMADA ================== */
static void iniciar_llamada(void) {
    s_state = ST_CALLING;
    ESP_LOGI(TAG, "PULSO -> llamada iniciada");
    if (!wifi_ok()) {
        speaker_init(); s_error(); speaker_release();
        s_state = ST_IDLE;
        ESP_LOGW(TAG, "Sin WiFi -> tono de error");
        return;
    }
    speaker_init();
    s_llamada_enviada();                       /* ♪ confirmación al visitante */
    capturar_foto();

    /* URL_AYUDA (config.h) ya incluye el link directo de atención */
    char cap[400];
    snprintf(cap, sizeof(cap), "🔔 Alguien está en la puerta. %s", URL_AYUDA);
    bool ok = (s_foto && s_foto_len)
        ? telegram_send_photo(s_foto, s_foto_len, cap)   /* foto + aviso JUNTOS */
        : telegram_send_message(cap);
    soltar_foto();
    if (!ok) ESP_LOGW(TAG, "Telegram fallo (la llamada sigue igual)");

    speaker_release();                         /* el audio pasa al WebRTC */
    app_start_call();
    esp_timer_start_once(s_timer, TIMEOUT_LLAMADA_S * 1000000ULL);
    ESP_LOGI(TAG, "Llamando (timeout %d s)...", TIMEOUT_LLAMADA_S);
}

static void finalizar_no_atendida(void) {
    app_stop_call();
    telegram_send_message("⏱ Nadie atendió la puerta.");
    speaker_init(); s_mensaje_registrado(); speaker_release();
    s_state = ST_IDLE;
    ESP_LOGI(TAG, "Timeout: no atendieron");
}

static void finalizar_llamada(void) {
    app_stop_call(); soltar_foto();
    speaker_init(); s_fin(); speaker_release();
    s_state = ST_IDLE;
    ESP_LOGI(TAG, "Llamada finalizada");
}

/* ================== TAREA PRINCIPAL ================== */
static void doorbell_task(void *arg) {
    uint32_t ev; TickType_t t0 = 0; bool blink = false;
    for (;;) {
        xTaskNotifyWait(0, UINT32_MAX, &ev, pdMS_TO_TICKS(50));

        /* pulsador: pulso corto = llamada · presionado 3 s = TEST de audio */
        if (s_state == ST_IDLE && gpio_get_level(PIN_BOTON) == 0) {
            vTaskDelay(pdMS_TO_TICKS(40));
            if (gpio_get_level(PIN_BOTON) == 0) {
                TickType_t t_pulso = xTaskGetTickCount();
                while (gpio_get_level(PIN_BOTON) == 0) vTaskDelay(pdMS_TO_TICKS(50));
                if (xTaskGetTickCount() - t_pulso >= pdMS_TO_TICKS(3000)) {
                    speaker_init(); test_audio(); speaker_release();
                } else {
                    iniciar_llamada();
                }
            }
        }

        switch (s_state) {
            case ST_CALLING:
                if (ev & EV_ANSWERED) {
                    esp_timer_stop(s_timer);
                    app_tone_attended();
                    s_state = ST_TALKING;
                    ESP_LOGI(TAG, "ATENDIDA -> LED fijo");
                } else if (ev & (EV_TIMEOUT | EV_CLOSED)) finalizar_no_atendida();
                break;
            case ST_TALKING:
                if (ev & EV_CLOSED) finalizar_llamada();
                break;
            default: break;
        }

        /* LED verde: parpadea=llamando · fijo=hablando · apagado=reposo */
        TickType_t now = xTaskGetTickCount();
        if (s_state == ST_CALLING) {
            if (now - t0 >= pdMS_TO_TICKS(250)) { blink = !blink; t0 = now; set_led(blink); }
        } else set_led(s_state == ST_TALKING);
    }
}

/* ================== INICIO ================== */
void doorbell_init(void) {
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << PIN_BOTON) | (1ULL << PIN_LED),
        .mode = GPIO_MODE_INPUT_OUTPUT, .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&io);
    set_led(false);
    esp_timer_create_args_t ta = { .callback = cb_timer, .name = "timeout" };
    esp_timer_create(&ta, &s_timer);
    xTaskCreatePinnedToCore(doorbell_task, "doorbell", 8192, NULL, 5, &s_task, 1);
    ota_start();   /* actualizaciones automáticas + comando /update */
    ESP_LOGI(TAG, "Listo: boton=GPIO%d LED=GPIO%d | TEST audio: mantener puls