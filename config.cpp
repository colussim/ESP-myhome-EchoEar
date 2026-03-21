#include "config.h"

//#include <IPAddress.h>

// Remplacez par votre clé AccessKey de la console Picovoice

const char* ELEVENLABS_KEY = "sk_935f6f388888b82a79c822ed870dcf328e52b54fa7ca72f5";
const char* VOICE_ID = "6vTyAgAT8PncODBcLjRf"; 
const bool ENABLE_ELEVENLABS = false;  
const bool ENABLE_AUTH = true;
const char* API_TOKEN = "HA_MDkYWitLxHcKK1rDbA06YORu7j4KZ16FBVThG1m8DXUhv30RXWWi9ecOo0SGPMpw";

WiFiConfig myWiFi = {
  .ssid = "Gen_home3",
  .password = "Leclubdesmarins25@",
  .use_static_ip = true,
  .ip_addr = "192.168.0.11",
  .gateway = "192.168.0.254",
  .subnet = "255.255.255.0",
  .dns1 = "192.168.0.254"
};


