/**
 * Firmware ESP8266 - Nodo 6 DS18B20 (netEspNow v1.1-fix1)
 * ========================================================
 * Placa: NodeMCU v3 (ESP-12E, CP2102)
 *
 * Funcion:
 *   - Nodo sensor de TEMPERATURA (DS18B20, sonda impermeable de acero).
 *   - Escucha requests del bridge (msg_type=0, node_id=6) via ESP-NOW.
 *   - Al recibir el request, lee el DS18B20 y responde con un mensaje
 *     de sensor (msg_type=2) conteniendo:
 *       temp_x10 (°C * 10) + hum_x10 (N/A, este sensor no mide humedad)
 *       + contador + uptime_ms
 *
 * Protocolo ESP-NOW:
 *   Request  (bridge -> nodo) : EspNowMessage (13 B) {msg_type=0, node_id=6, ...}
 *   Response (nodo -> bridge): EspNowSensorMessage (14 B) {msg_type=2, node_id=6,
 *                                temp_x10, hum_x10, counter, uptime_ms}
 *
 * NOTA: La lectura del DS18B20 bloquea ~750 ms (resolucion 12 bits).
 *       Se hace en loop() (NUNCA en el ISR de ESP-NOW). El bridge tiene
 *       timeout de 2000 ms para dar margen.
 *
 * PINOUT NodeMCU v3:
 *   - D2 (GPIO4) -> DATA del DS18B20 (con pull-up 4.7k en el cable/sonda)
 *   - D5 (GPIO14)/D6 (GPIO12) -> RESERVADOS (SoftwareSerial Fase 2)
 *
 * Alimentacion DS18B20: VCC -> 3.3V, GND -> GND (sonda de 3 cables:
 *   rojo=VCC, negro=GND, amarillo/blanco=DATA).
 */

#include <ESP8266WiFi.h>
#include <espnow.h>
#include <OneWire.h>
#include <DallasTemperature.h>

// ============================================================
// CONFIGURACION - ID DEL NODO
// ============================================================
#define NODE_ID 6

#define MSG_TYPE_REQUEST       0
#define MSG_TYPE_RESPONSE      1   // (no usado por este nodo, solo universal)
#define MSG_TYPE_SENSOR_RESP   2   // respuesta de este nodo (temp + hum)

#define SERIAL_BAUD      115200

// Pin DATA del DS18B20: D2 = GPIO4
#define ONE_WIRE_BUS 4

// Marcador de "lectura invalida" (temp/hum en *10)
#define SENSOR_INVALID  -9999

// ============================================================
// Objetos OneWire + DallasTemperature
// ============================================================
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);

// ============================================================
// Estructuras de mensaje (deben coincidir con el bridge)
// ============================================================

// Request del bridge (misma struct de 13 B que los nodos universales)
typedef struct __attribute__((packed)) {
  uint8_t  msg_type;    // 0=request
  uint8_t  node_id;     // ID destino/origen
  uint8_t  gpio_state;  // no usado aqui
  uint16_t adc_value;   // no usado aqui
  uint32_t counter;
  uint32_t uptime_ms;
} EspNowMessage;        // 13 bytes

// Respuesta de sensor (temp + hum) de este nodo (14 bytes)
typedef struct __attribute__((packed)) {
  uint8_t  msg_type;    // 2 = sensor response
  uint8_t  node_id;     // 6
  int16_t  temp_x10;    // temperatura en °C * 10 (ej: 234 = 23.4°C)
  int16_t  hum_x10;     // humedad en % * 10 (N/A para DS18B20)
  uint32_t counter;     // contador correlativo
  uint32_t uptime_ms;   // tiempo activo del nodo
} EspNowSensorMessage;  // 14 bytes

// ============================================================
// Variables compartidas ISR <-> loop
// ============================================================
static volatile bool    g_request_pending = false;
static volatile uint8_t g_master_mac[6];
static uint32_t response_counter = 0;
static bool master_peer_added = false;

// ============================================================
// Callback ISR: datos recibidos via ESP-NOW
// ============================================================
void onDataReceived(uint8_t *mac, uint8_t *data, uint8_t len) {
  // Solo nos interesa el request: msg_type=0 y node_id == NODE_ID.
  // El request es la struct de 13 B, pero basta con leer los 2 primeros bytes.
  if (len < 2) return;
  uint8_t type = data[0];
  uint8_t id   = data[1];
  if (type == MSG_TYPE_REQUEST && id == NODE_ID) {
    memcpy((void*)g_master_mac, mac, 6);
    g_request_pending = true;   // flag atomico (1 byte, seguro)
  }
}

// ============================================================
// Callback ISR: confirmacion de envio
// ============================================================
void onDataSent(uint8_t *mac, uint8_t status) {
  (void)mac; (void)status;  // No hacer nada en ISR
}

// ============================================================
// Enviar respuesta (llamado desde loop, NO desde ISR)
// ============================================================
void sendSensorResponse() {
  response_counter++;

  EspNowSensorMessage resp;
  resp.msg_type = MSG_TYPE_SENSOR_RESP;
  resp.node_id = NODE_ID;

  // Leer DS18B20 (bloquea ~750 ms en resolucion 12 bits)
  sensors.requestTemperatures();
  float tempC = sensors.getTempCByIndex(0);

  if (tempC == DEVICE_DISCONNECTED_C || isnan(tempC)) {
    resp.temp_x10 = SENSOR_INVALID;
  } else {
    resp.temp_x10 = (int16_t)(tempC * 10.0f);
  }
  resp.hum_x10  = SENSOR_INVALID;   // DS18B20 no mide humedad
  resp.counter   = response_counter;
  resp.uptime_ms = millis();

  uint8_t mac[6];
  memcpy(mac, (const void*)g_master_mac, 6);

  if (!master_peer_added) {
    esp_now_add_peer(mac, ESP_NOW_ROLE_COMBO, 1, NULL, 0);
    master_peer_added = true;
    delay(10);  // pequena pausa para estabilizar
  }

  int ret = esp_now_send(mac, (uint8_t*)&resp, sizeof(EspNowSensorMessage));

  Serial.print("[OK] Respuesta nodo ");
  Serial.print(NODE_ID);
  Serial.print(" | t=");
  if (resp.temp_x10 == SENSOR_INVALID) Serial.print("ERR");
  else Serial.print(resp.temp_x10 / 10.0f, 1);
  Serial.print("C | contador=");
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
  Serial.println("v1.1-fix1 - Sensor DS18B20 (temperatura)");
  Serial.println("======================================");

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);

  Serial.print("MAC: ");
  Serial.println(WiFi.macAddress());
  Serial.print("FW: ESP-NOW Nodo v1.1-fix1 ID=");
  Serial.println(NODE_ID);
  Serial.print("DS18B20 DATA: D2 (GPIO");
  Serial.print(ONE_WIRE_BUS);
  Serial.println(")");

  // Inicializar bus OneWire y buscar sensores
  sensors.begin();
  int n = sensors.getDeviceCount();
  Serial.print("DS18B20 sensors found: ");
  Serial.println(n);

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
}

// ============================================================
// Loop
// ============================================================
void loop() {
  // Procesar request pendiente (lectura DS18B20 + respuesta)
  if (g_request_pending) {
    g_request_pending = false;
    sendSensorResponse();
  }

  delay(1);
}
