
#pragma once
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#define WAKE_WORD_MODEL_NAME "wn9_heykira_tts3" /* Name of the wake word model to use (must be present in the "model" folder of the SPIFFS partition) */

 
#ifdef __cplusplus
extern "C" {
#endif
 
// Bits of the EventGroup shared between wake_local and voice_cmd
#define WAKE_WORD_DETECTED_BIT  BIT0   // wake_local → voice_cmd : wake detected
#define WAKE_WORD_DONE_BIT      BIT1   // voice_cmd → wake_local : command finished, resume
 
// EventGroup global — created in main.c before any init
extern EventGroupHandle_t g_wake_event_group;
 
bool wake_local_init(void);
void wake_feed_task(void *arg);
void wake_fetch_task(void *arg);

 
#ifdef __cplusplus
}
#endif