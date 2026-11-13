/**
 * Firmware ESP8266 - Nodo 2 ESP-NOW
 * =========================================
 * Placa: NodeMCU v3 (ESP-12E, CP2102)
 * 
 * Funcion:
 *   - Escucha peticiones del ESP32 maestro via ESP-NOW
 *   - Cuando recibe un request (tipo=0) dirigido a su ID,
 *     responde con su ID y un contador correlativo (tipo=1)
 *
 * Protocolo ESP-NOW:
 *   Request  (ESP32 -> ESP8266): {tipo=0, node_id=X, counter=0}
 *   Response (ESP8266 -> ESP32): {tipo=1, node_id=X, counter=N, uptime_ms}
 *
 * NOTA: Los callbacks ESP-NOW se ejecutan en contexto ISR.
 *       NO hacer Serial.print/delay/memcpy lentos dentro de ISR.
 *
 * PINOUT NodeMCU v3:
 *   - D4 (GPIO2) -> LED_BUILTIN (azul, activo en LOW)
 */

#include <ESP8266WiFi.h>
#include <espnow.h>

// ============================================================
// CONFIGURACION - ID DEL NODO
// ============================================================
#define NODE_ID 2

#define MSG_TYPE_REQUEST  0
#define MSG_TYPE_RESPONSE 1
#define SERIAL_BAUD       115200
#define HEARTBEAT_MS      1000   // LED parpadea cada 1s

// Estructura del mensaje (debe coincidir con ESP32)
typedef struct __attribute__((packed)) {
  uint8_t  msg_type;
  uint8_t  node_id;
  uint32_t counter;
  uint32_t uptime_ms;
} EspNowMessage;

// Variables compartidas ISR <-> loop
static volatile bool   g_request_pending = false;
static volatile uint8_t g_master_mac[6];
static uint32_t response_counter = 0;
static bool master_peer_added = false;
static unsigned long last_heartbeat = 0;

// ============================================================
// Callback ISR: datos recibidos
// ============================================================
void onDataReceived(uint8_t *mac, uint8_t *data, uint8_t len) {
  if (len != sizeof(EspNowMessage)) return;

  EspNowMessage *msg = (EspNowMessage*)data;
  if (msg->msg_type == MSG_TYPE_REQUEST && msg->node_id == NODE_ID) {
    memcpy((void*)g_master_mac, mac, 6);
    g_request_pending = true;   // flag atomico (1 byte, seguro)
  }
}

// ============================================================
// Callback ISR: datos enviados
// ============================================================
void onDataSent(uint8_t *mac, uint8_t status) {
  (void)mac; (void)status;  // No hacer nada en ISR
}

// ============================================================
// Enviar respuesta (llamado desde loop, NO desde ISR)
// ============================================================
void sendResponse() {
  response_counter++;

  EspNowMessage resp;
  resp.msg_type = MSG_TYPE_RESPONSE;
  resp.node_id = NODE_ID;
  resp.counter = response_counter;
  resp.uptime_ms = millis();

  uint8_t mac[6];
  memcpy(mac, (const void*)g_master_mac, 6);

  if (!master_peer_added) {
    esp_now_add_peer(mac, ESP_NOW_ROLE_COMBO, 1, NULL, 0);
    master_peer_added = true;
    delay(10);  // pequena pausa para estabilizar
  }

  int ret = esp_now_send(mac, (uint8_t*)&resp, sizeof(EspNowMessage));

  Serial.print("[OK] Respuesta nodo ");
  Serial.print(NODE_ID);
  Serial.print(" | contador=");
  Serial.print(response_counter);
  Serial.print(" | send=");
  Serial.println(ret == 0 ? "OK" : "FAIL");
}

// ============================================================
// Setup
// ============================================================
void setup() {
  Serial.begin(SERIAL_BAUD);
  Serial.println();
  Serial.println("======================================");
  Serial.print("ESP8266 Nodo ESP-NOW - ID: ");
  Serial.println(NODE_ID);
  Serial.println("======================================");

  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, HIGH);  // OFF

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);

  Serial.print("MAC: ");
  Serial.println(WiFi.macAddress());
  Serial.print("FW: ESP-NOW Nodo v1.0 ID=");
  Serial.println(NODE_ID);

  int ret = esp_now_init();
  if (ret != 0) {
    Serial.print("[ERROR] esp_now_init() = ");
    Serial.println(ret);
    return;
  }

  esp_now_set_self_role(ESP_NOW_ROLE_COMBO);
  esp_now_register_recv_cb(onDataReceived);
  esp_now_register_send_cb(onDataSent);

  Serial.println("[OK] ESP-NOW listo, esperando requests...");
  Serial.println();

  // Parpadeo de inicio: 3 parpadeos
  for (int i = 0; i < 3; i++) {
    digitalWrite(LED_BUILTIN, LOW); delay(100);
    digitalWrite(LED_BUILTIN, HIGH); delay(100);
  }

  last_heartbeat = millis();
}

// ============================================================
// Loop
// ============================================================
void loop() {
  unsigned long now = millis();

  // Heartbeat LED cada 1s
  if (now - last_heartbeat >= HEARTBEAT_MS) {
    last_heartbeat = now;
    digitalWrite(LED_BUILTIN, LOW);   // ON
    delay(20);
    digitalWrite(LED_BUILTIN, HIGH);  // OFF
  }

  // Procesar request pendiente
  if (g_request_pending) {
    g_request_pending = false;
    sendResponse();
  }

  delay(1);
}
