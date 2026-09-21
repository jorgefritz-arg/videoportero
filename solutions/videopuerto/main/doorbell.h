#pragma once
#include <stdbool.h>

void doorbell_init(void);            /* arranca botón + LED + tonos + foto + OTA */
void doorbell_notify_answered(void); /* llamar cuando la sesión WebRTC conecta   */
void doorbell_notify_closed(void);   /* llamar cuando la sesión WebRTC se corta  */
bool doorbell_busy(void);            /* true durante una llamada                 */