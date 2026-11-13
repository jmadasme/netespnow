/**
 * Firmware ESP8266 - Nodo 7 GY-906 (MLX90614) — netEspNow v1.1-fix1
 * ==================================================================
 * Placa: NodeMCU v3 (ESP-12E, CP2102)
 *
 * Funcion:
 *   - Nodo sensor de TEMPERATURA INFRARROJA SIN CONTACTO (GY-906 / MLX90614).
 *   - Escucha requests del bridge (msg_type=0, node_id=7) via ESP-NOW.
 *   - Al recibir el request, lee el MLX90614 por I2C y responde con un
 *     mensaje IR (msg_type=3) conteniendo:
 *       obj_temp_x10 (°C * 10) + amb_temp_x10 (°C * 10) + contador + uptime
 *
 * Protocolo ESP-NOW:
 *   Request  (bridge -> nodo) : EspNowMessage (13 B) {msg_type=0, node_id=7, ...}
 *   Response (nodo -> bridge): EspNowIrMessage (14 B) {msg_type=3, node_id=7,
 *                                obj_temp_x10, amb_temp_x10, counter, uptime_ms}
 *
 * PINOUT NodeMCU v3:
 *   - D2 (GPIO4) -> SDA del GY-906 (I2C)
 *   - D1 (GPIO5) -> SCL del GY-906 (I2C)
 *   - D4 (GPIO2) -> LED_BUILTIN (heartbeat, activo en LOW)
 *   - D5 (GPIO14)/D6 (GPIO12) -> RESERVADOS (SoftwareSerial Fase 2)
 *
 * Alimentacion GY-906: VIN -> 3.3V (el modulo trae regulador y pull-ups I2C),
 *   GND -> GND, SDA -> D2, SCL -> D1.
 *
 * Direccion I2C del MLX90614: 0x5A (default).
 */

#include <ESP8266WiFi.h>
#include <espnow.h>
#include <Wire.h>
#include <Adafruit_MLX90614.h>

// ============================================================
// CONFIGURACION - ID DEL NODO
// ============================================================
#define NODE_ID 7

#define MSG_TYPE_REQUEST     0
#define MSG_TYPE_RESPONSE    1   // (no usado por este nodo, solo universal)
#define MSG_TYPE_IR_RESPONSE 3   // respuesta de este nodo (IR temp)

#define SERIAL_BAUD      115200
#define HEARTBEAT_MS     1000

// LED (activo en LOW en NodeMCU)
#define LED_PIN   LED_BUILTIN
#define LED_ON    LOW
#define LED_OFF   HIGH

// Marcador de "lectura invalida" (temperaturas en *10)
#define SENSOR_INVALID  -9999

// ============================================================
// Objeto MLX90614
// ============================================================
Adafruit_MLX90614 mlx = Adafruit_MLX90614();

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

// Respuesta IR de este nodo (14 bytes)
typedef struct __attribute__((packed)) {
  uint8_t  msg_type;       // 3 = IR response
  uint8_t  node_id;        // 7
  int16_t  obj_temp_x10;   // temperatura del objeto en °C * 10
  int16_t  amb_temp_x10;   // temperatura ambiente en °C * 10
  uint32_t counter;        // contador correlativo
  uint32_t uptime_ms;      // tiempo activo del nodo
} EspNowIrMessage;         // 14 bytes

// ============================================================
// Variables compartidas ISR <-> loop
// ============================================================
static volatile bool    g_request_pending = false;
static volatile uint8_t g_master_mac[6];
static uint32_t response_counter = 0;
static bool master_peer_added = false;
static unsigned long last_heartbeat = 0;

// ============================================================
// Callback ISR: datos recibidos via ESP-NOW
// ============================================================
void onDataReceived(uint8_t *mac, uint8_t *data, uint8_t len) {
  // Solo nos interesa el request: msg_type=0 y node_id == NODE_ID.
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
// Enviar respuesta IR (llamado desde loop, NO desde ISR)
// ============================================================
void sendIrResponse() {
  response_counter++;

  EspNowIrMessage resp;
  resp.msg_type = MSG_TYPE_IR_RESPONSE;
  resp.node_id = NODE_ID;

  // Leer MLX90614 (objeto + ambiente), con reintentos ante I2C flaky
  float obj = NAN, amb = NAN;
  for (int intento = 0; intento < 3; intento++) {
    obj = mlx.readObjectTempC();
    amb = mlx.readAmbientTempC();
    if (!isnan(obj) && !isnan(amb)) break;   // lectura valida
    delay(100);  // pausa antes de reintentar
  }

  if (isnan(obj) || isnan(amb)) {
    resp.obj_temp_x10 = SENSOR_INVALID;
    resp.amb_temp_x10 = SENSOR_INVALID;
  } else {
    resp.obj_temp_x10 = (int16_t)(obj * 10.0f);
    resp.amb_temp_x10 = (int16_t)(amb * 10.0f);
  }
  resp.counter   = response_counter;
  resp.uptime_ms = millis();

  uint8_t mac[6];
  memcpy(mac, (const void*)g_master_mac, 6);

  if (!master_peer_added) {
    esp_now_add_peer(mac, ESP_NOW_ROLE_COMBO, 1, NULL, 0);
    master_peer_added = true;
    delay(10);  // pequena pausa para estabilizar
  }

  int ret = esp_now_send(mac, (uint8_t*)&resp, sizeof(EspNowIrMessage));

  Serial.print("[OK] Respuesta nodo ");
  Serial.print(NODE_ID);
  Serial.print(" | obj=");
  if (resp.obj_temp_x10 == SENSOR_INVALID) Serial.print("ERR");
  else Serial.print(resp.obj_temp_x10 / 10.0f, 1);
  Serial.print("C amb=");
  if (resp.amb_temp_x10 == SENSOR_INVALID) Serial.print("ERR");
  else Serial.print(resp.amb_temp_x10 / 10.0f, 1);
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
  Serial.println("v1.1-fix1 - Sensor GY-906 (IR temp)");
  Serial.println("======================================");

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LED_OFF);

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);

  Serial.print("MAC: ");
  Serial.println(WiFi.macAddress());
  Serial.print("FW: ESP-NOW Nodo v1.1-fix1 ID=");
  Serial.println(NODE_ID);
  Serial.println("GY-906 I2C: SDA=D2 (GPIO4), SCL=D1 (GPIO5)");

  // Inicializar I2C (pines por defecto del NodeMCU: SDA=D2, SCL=D1)
  Wire.begin();

  // Inicializar MLX90614
  if (mlx.begin()) {
    Serial.println("[MLX90614] Sensor inicializado correctamente (0x5A)");
  } else {
    Serial.println("[MLX90614] ERROR: no se encontro el sensor en I2C");
  }

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
    digitalWrite(LED_PIN, LED_ON);  delay(100);
    digitalWrite(LED_PIN, LED_OFF); delay(100);
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
    digitalWrite(LED_PIN, LED_ON);
    delay(20);
    digitalWrite(LED_PIN, LED_OFF);
  }

  // Procesar request pendiente (lectura MLX90614 + respuesta)
  if (g_request_pending) {
    g_request_pending = false;
    sendIrResponse();
  }

  delay(1);
}
