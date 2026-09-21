#pragma once
/* =====================================================================
 *  CONFIG del videoportero — SIN SECRETOS (seguro para repo público)
 *  Los datos privados los genera GitHub Actions en main/secrets.h
 *  a partir de los Secrets del repo.
 * ===================================================================== */
#include "secrets.h"   /* ← generado en la nube: TG_TOKEN, TG_CHAT_ID, DOORBELL_PEER_ID */

/* Repo de actualizaciones (público por diseño) */
#define REPO_API   "https://api.github.com/repos/jorgefritz-arg/videopuerto/releases/latest"

/* Link de atención (el dato sensible es el peer id, que vive en secrets.h) */
#define URL_AYUDA  "Toca acá para atender: https://webrtc.espressif.com/join/" DOORBELL_PEER_ID