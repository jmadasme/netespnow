/**
 * Firmware ESP8266 - Nodo 0 ESP-NOW (netEspNow v1.3)
 * ====================================================
 * Placa: NodeMCU v3 (ESP-12E, CP2102)
 *
 * Plantilla base I/O (NODE_FUNC_IO):
 *   - Lee entrada digital PIN_GPIO_IN (D5/GPIO14, pull-up)
 *   - Lee ADC PIN_ADC (A0, 0-1023)
 *   - Escribe salida digital PIN_GPIO_OUT (D1/GPIO5) on/off
 *     accionable remotamente desde el Bridge (CMD_SET_STATE)
 *   - Responde a requests (msg_type=0) y comandos (msg_type=2)
 *     con cabecera comun extendida de 21 B (msg_type=1)
 *   - Watchdog de red: reinicia si pierde comunicacion ESP-NOW
 *     con la Puerta de Enlace (> NODE_WDT_TIMEOUT_MS)
 *
 * Version: 1.3
 */

#include <ESP8266WiFi.h>
#include <espnow.h>
#include <EEPROM.h>
#include "netesponw_comun.h"

// ============================================================
// CONFIGURACION - ID DEL NODO
// ============================================================
#define NODE_ID 0

#define SERIAL_BAUD       115200
#define HEARTBEAT_MS      1000   // LED parpadea cada 1s

// Buffer ISR <-> loop para mensajes recibidos
static volatile bool    g_msg_pending = false;
static volatile uint8_t g_master_mac[6];
static volatile uint8_t g_rx_buffer[sizeof(EspNowNodeMessageV13)];
static uint32_t response_counter = 0;
static bool master_peer_added = false;
static unsigned long last_heartbeat = 0;
static unsigned long last_comm_ms = 0;

// Estado de la salida digital
static uint8_t gpio_out_state = 0;

// ============================================================
// EEPROM del nodo (configuracion en primer arranque)
// ============================================================
#define EEPROM_SIZE 512
#define EEPROM_ADDR_MAGIC 0
#define EEPROM_ADDR_CFG   4   // node_id, node_function, fw_version, res, res

void initNodeEEPROM() {
  EEPROM.begin(EEPROM_SIZE);
  uint32_t magic;
  EEPROM.get(EEPROM_ADDR_MAGIC, magic);
  if (magic != EEPROM_NODE_MAGIC) {
    // Primer arranque: escribir configuracion del nodo
    uint32_t m = EEPROM_NODE_MAGIC;
    EEPROM.put(EEPROM_ADDR_MAGIC, m);
    uint8_t cfg[5] = { (uint8_t)NODE_ID, NODE_FUNC_IO, NODE_FW_VERSION, 0, 0 };
    for (int i = 0; i < 5; i++) {
      EEPROM.write(EEPROM_ADDR_CFG + i, cfg[i]);
    }
    EEPROM.commit();
    Serial.println("[EEPROM] Configuracion inicial de nodo guardada (NS13)");
  } else {
    Serial.println("[EEPROM] Configuracion de nodo ya presente (NS13)");
  }
}

// ============================================================
// Callback ISR: datos recibidos
// ============================================================
void onDataReceived(uint8_t *mac, uint8_t *data, uint8_t len) {
  if (len != sizeof(EspNowNodeMessageV13)) return;

  EspNowNodeMessageV13 *msg = (EspNowNodeMessageV13*)data;
  if (msg->node_id != NODE_ID) return;
  if (msg->msg_type != MSG_TYPE_REQUEST && msg->msg_type != MSG_TYPE_COMMAND) return;

  memcpy((void*)g_master_mac, mac, 6);
  memcpy((void*)g_rx_buffer, data, len);
  g_msg_pending = true;
}

// ============================================================
// Callback ISR: datos enviados
// ============================================================
void onDataSent(uint8_t *mac, uint8_t status) {
  (void)mac; (void)status;
}

// ============================================================
// Leer estado I/O del nodo
// ============================================================
void readIO(uint8_t *gpio_in, uint16_t *adc_raw, uint8_t *gpio_out) {
  *gpio_in  = (uint8_t)digitalRead(PIN_GPIO_IN);
  *adc_raw  = (uint16_t)analogRead(PIN_ADC);
  *gpio_out = gpio_out_state;
}

// ============================================================
// Construir respuesta con cabecera comun extendida
// ============================================================
void buildResponse(EspNowNodeMessageV13 *resp) {
  uint8_t gpio_in;
  uint16_t adc_raw;

  resp->msg_type      = MSG_TYPE_RESPONSE;
  resp->node_id       = NODE_ID;
  resp->node_function = NODE_FUNC_IO;
  resp->fw_version    = NODE_FW_VERSION;
  resp->rssi_db       = 0;   // RESERVADO
  resp->counter       = response_counter;
  resp->uptime_ms     = (uint32_t)millis();
  memset(resp->payload, 0, 8);

  readIO(&gpio_in, &adc_raw, &gpio_out_state);
  resp->payload[PAYLOAD_IO_GPIO_IN]    = gpio_in;
  resp->payload[PAYLOAD_IO_ADC_RAW]    = (uint8_t)(adc_raw & 0xFF);
  resp->payload[PAYLOAD_IO_ADC_RAW+1]  = (uint8_t)((adc_raw >> 8) & 0xFF);
  resp->payload[PAYLOAD_IO_GPIO_OUT]   = gpio_out_state;
}

// ============================================================
// Enviar respuesta (llamado desde loop, NO desde ISR)
// ============================================================
void sendResponse() {
  response_counter++;

  EspNowNodeMessageV13 resp;
  buildResponse(&resp);

  uint8_t mac[6];
  memcpy(mac, (const void*)g_master_mac, 6);

  if (!master_peer_added) {
    esp_now_add_peer(mac, ESP_NOW_ROLE_COMBO, 1, NULL, 0);
    master_peer_added = true;
    delay(10);
  }

  int ret = esp_now_send(mac, (uint8_t*)&resp, sizeof(EspNowNodeMessageV13));

  Serial.print("[OK] Respuesta nodo ");
  Serial.print(NODE_ID);
  Serial.print(" | c=");
  Serial.print(response_counter);
  Serial.print(" | i=");
  Serial.print(resp.payload[PAYLOAD_IO_GPIO_IN]);
  Serial.print(" a=");
  Serial.print(resp.payload[PAYLOAD_IO_ADC_RAW] |
               (resp.payload[PAYLOAD_IO_ADC_RAW+1] << 8));
  Serial.print(" o=");
  Serial.print(resp.payload[PAYLOAD_IO_GPIO_OUT]);
  Serial.print(" | send=");
  Serial.println(ret == 0 ? "OK" : "FAIL");
}

// ============================================================
// Procesar mensaje recibido (request o comando)
// ============================================================
void processReceivedMessage() {
  g_msg_pending = false;
  last_comm_ms = millis();

  EspNowNodeMessageV13 msg;
  memcpy(&msg, (const void*)g_rx_buffer, sizeof(EspNowNodeMessageV13));

  if (msg.msg_type == MSG_TYPE_COMMAND) {
    uint8_t cmd = msg.payload[0];
    if (cmd == CMD_SET_STATE) {
      // payload[1]: 0=off, 1=on
      uint8_t valor = msg.payload[1];
      gpio_out_state = (valor == 0) ? 0 : 1;
      digitalWrite(PIN_GPIO_OUT, gpio_out_state);
      Serial.printf("[CMD] SET_STATE=%u -> out=%u\n", valor, gpio_out_state);
    } else if (cmd == CMD_GET_STATE) {
      Serial.println("[CMD] GET_STATE");
    } else {
      Serial.printf("[CMD] Comando desconocido: 0x%02X\n", cmd);
    }
    // Responder siempre con estado completo
    sendResponse();
  } else {
    // MSG_TYPE_REQUEST: peticion de estado
    sendResponse();
  }
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
  Serial.println("  netEspNow v1.3 (plantilla base I/O)");
  Serial.println("======================================");

  // Configurar pines de la plantilla I/O
  pinMode(PIN_GPIO_IN, INPUT_PULLUP);
  pinMode(PIN_GPIO_OUT, OUTPUT);
  digitalWrite(PIN_GPIO_OUT, gpio_out_state);

  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, HIGH);  // OFF

  initNodeEEPROM();

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);

  Serial.print("MAC: ");
  Serial.println(WiFi.macAddress());
  Serial.print("FW: ESP-NOW Nodo v1.3 ID=");
  Serial.println(NODE_ID);
  Serial.printf("Pines: IN=D5/GPIO14, ADC=A0, OUT=D1/GPIO5\n");

  int ret = esp_now_init();
  if (ret != 0) {
    Serial.print("[ERROR] esp_now_init() = ");
    Serial.println(ret);
    return;
  }

  esp_now_set_self_role(ESP_NOW_ROLE_COMBO);
  esp_now_register_recv_cb(onDataReceived);
  esp_now_register_send_cb(onDataSent);

  Serial.println("[OK] ESP-NOW listo, esperando requests/comandos...");
  Serial.println();

  // Parpadeo de inicio: 3 parpadeos
  for (int i = 0; i < 3; i++) {
    digitalWrite(LED_BUILTIN, LOW); delay(100);
    digitalWrite(LED_BUILTIN, HIGH); delay(100);
  }

  last_heartbeat = millis();
  last_comm_ms = millis();
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

  // Procesar request/comando pendiente
  if (g_msg_pending) {
    processReceivedMessage();
  }

  // Watchdog de red: reiniciar si no hay comunicacion ESP-NOW
  if (now - last_comm_ms > NODE_WDT_TIMEOUT_MS) {
    Serial.println("[WDT-RED] Sin comunicacion ESP-NOW, reiniciando...");
    delay(100);
    ESP.restart();
  }

  delay(1);
}
