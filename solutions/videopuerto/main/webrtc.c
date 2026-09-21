/* WebRTC peer connection — VIDEOPORTERO
   Basado en el demo peer de Espressif (Public Domain / CC0).

   MODIFICADO para el videoportero:
   - Al conectar (ESP_PEER_STATE_CONNECTED) avisa a doorbell (LED fijo,
     cancela timeout) vía doorbell_notify_answered().
   - Al desconectarse (ESP_PEER_STATE_DISCONNECTED) avisa a doorbell
     (tono de fin, LED apagado) vía doorbell_notify_closed().
   - Se eliminó el audio sintético y el chatbot del demo (send_cb,
     chat_content): no corresponden a un videoportero.
*/

#include "media_lib_os.h"
#include "esp_log.h"
#include "esp_webrtc_defaults.h"
#include "esp_peer_default.h"
#include "common.h"
#include "doorbell.h"

#define TAG "PEER_DEMO"

static esp_peer_signaling_handle_t signaling = NULL;
static esp_peer_handle_t peer = NULL;
static bool peer_running = false;

/* ---- Estado de la conexión: notifica al módulo doorbell ---- */
static int peer_state_handler(esp_peer_state_t state, void* ctx)
{
    if (state == ESP_PEER_STATE_CONNECTED) {
        ESP_LOGI(TAG, "Peer CONNECTED");
        doorbell_notify_answered();          /* Edición 2: LED fijo + cancela timeout */
    } else if (state == ESP_PEER_STATE_DISCONNECTED) {
        ESP_LOGI(TAG, "Peer DISCONNECTED");
        doorbell_notify_closed();            /* Edición 3: tono de fin + LED apagado */
    }
    return 0;
}

/* ---- Retransmite el SDP local hacia el servidor de señalización (esencial) ---- */
static int peer_msg_handler(esp_peer_msg_t* msg, void* ctx)
{
    if (msg->type == ESP_PEER_MSG_TYPE_SDP) {
        esp_peer_signaling_send_msg(signaling, (esp_peer_signaling_msg_t *)msg);
    }
    return 0;
}

static int peer_video_info_handler(esp_peer_video_stream_info_t* info, void* ctx)
{
    return 0;
}

static int peer_audio_info_handler(esp_peer_audio_stream_info_t* info, void* ctx)
{
    return 0;
}

/* Audio que llega DESDE el teléfono (por ahora solo log; el pipeline
 * que lo reproduce por el MAX98357A es el próximo paso del proyecto) */
static int peer_audio_data_handler(esp_peer_audio_frame_t* frame, void* ctx)
{
    ESP_LOGI(TAG, "Audio recibido: %d bytes", (int)frame->size);
    return 0;
}

static int peer_video_data_handler(esp_peer_video_frame_t* frame, void* ctx)
{
    return 0;
}

/* Mensajes del data channel: solo se registran en el log */
static int peer_data_handler(esp_peer_data_frame_t* frame, void* ctx)
{
    ESP_LOGI(TAG, "Data channel: %d bytes", (int)frame->size);
    return 0;
}

static void pc_task(void *arg)
{
    while (peer_running) {
        esp_peer_main_loop(peer);
        media_lib_thread_sleep(20);
    }
    media_lib_thread_destroy(NULL);
}

static int signaling_ice_info_handler(esp_peer_signaling_ice_info_t* info, void* ctx)
{
    if (peer == NULL) {
        esp_peer_default_cfg_t peer_cfg = {
            .tcp_support = true,
            .agent_recv_timeout = 500,
            /* Lab/testing: the bundled coturn uses a self-signed cert for TURNS. */
            .insecure_skip_turn_cert_verify = true,
        };
        esp_peer_cfg_t cfg = {
            .server_lists = &info->server_info,
            .server_num = 1,
            .audio_dir = ESP_PEER_MEDIA_DIR_SEND_RECV,
            .audio_info = {
                .codec = ESP_PEER_AUDIO_CODEC_G711A,
            },
            .enable_data_channel = true,
            .role = info->is_initiator ? ESP_PEER_ROLE_CONTROLLING : ESP_PEER_ROLE_CONTROLLED,
            .on_state = peer_state_handler,
            .on_msg = peer_msg_handler,
            .on_video_info = peer_video_info_handler,
            .on_audio_info = peer_audio_info_handler,
            .on_video_data = peer_video_data_handler,
            .on_audio_data = peer_audio_data_handler,
            .on_data = peer_data_handler,
            .ctx = ctx,
            .extra_cfg = &peer_cfg,
            .extra_size = sizeof(esp_peer_default_cfg_t),
        };
        int ret = esp_peer_open(&cfg, esp_peer_get_default_impl(), &peer);
        if (ret != ESP_PEER_ERR_NONE) {
            return ret;
        }
        media_lib_thread_handle_t thread = NULL;
        peer_running = true;
        media_lib_thread_create_from_scheduler(&thread, "pc_task", pc_task, NULL);
        if (thread == NULL) {
            peer_running = false;
        }
    }
    return 0;
}

static int signaling_connected_handler(void* ctx)
{
    if (peer) {
        return esp_peer_new_connection(peer);
    }
    return 0;
}

static int signaling_msg_handler(esp_peer_signaling_msg_t* msg, void* ctx)
{
    if (msg->type == ESP_PEER_SIGNALING_MSG_BYE) {
        esp_peer_close(peer);
        peer = NULL;
    } else if (msg->type == ESP_PEER_SIGNALING_MSG_SDP) {
        // Receive remote SDP
        if (peer) {
            esp_peer_send_msg(peer, (esp_peer_msg_t*)msg);
        }
    } else if (msg->type == ESP_PEER_SIGNALING_MSG_CANDIDATE) {
        if (peer) {
            esp_peer_send_msg(peer, (esp_peer_msg_t*)msg);
        }
    }
    return 0;
}

static int signaling_close_handler(void *ctx)
{
    return 0;
}

static int start_signaling(char* url)
{
    esp_peer_signaling_cfg_t cfg = {
        .signal_url = url,
        .on_ice_info = signaling_ice_info_handler,
        .on_connected = signaling_connected_handler,
        .on_msg = signaling_msg_handler,
        .on_close = signaling_close_handler,
    };
    // Use APPRTC signaling
    return esp_peer_signaling_start(&cfg, esp_signaling_get_apprtc_impl(), &signaling);
}

int start_webrtc(char *url)
{
    if (network_is_connected() == false) {
        ESP_LOGE(TAG, "Wifi not connected yet");
        return -1;
    }
    stop_webrtc();
    return start_signaling(url);
}

void query_webrtc(void)
{
    if (peer) {
        esp_peer_query(peer);
    }
}

int stop_webrtc(void)
{
    peer_running = false;
    if (peer) {
        esp_peer_close(peer);
        peer = NULL;
    }
    if (signaling) {
        esp_peer_signaling_stop(signaling);
        signaling = NULL;
    }
    return 0;
}