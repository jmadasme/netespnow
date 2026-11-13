/**
 * Firmware ESP8266 - Nodo 8 GY-61 (ADXL335) — netEspNow v1.1-fix1
 * =================================================================
 * Placa: NodeMCU v3 (ESP-12E, CP2102)
 *
 * Funcion:
 *   - Nodo de ACELERACION / VIBRACION (GY-61 / ADXL335), eje X.
 *   - MUESTREA CONTINUAMENTE el eje X (cada 10 ms, ventana de 50 muestras)
 *     y CLASIFICA en tiempo real el estado de vibracion:
 *         0 = REPOSO
 *         1 = VIBRACION BAJA
 *         2 = VIBRACION MEDIA
 *         3 = VIBRACION ALTA
 *   - La clasificacion se calcula SIEMPRE en loop() (en segundo plano).
 *   - Solo cuando el bridge pregunta (request msg_type=0, node_id=8),
 *     responde (msg_type=4) con la categorizacion actual + el valor de X
 *     + el rango de vibracion.
 *
 * CABLEADO:
 *   GY-61 VCC   -> 3.3V
 *   GY-61 GND   -> GND
 *   GY-61 X_OUT -> A0   (eje X)
 *   - D4 (GPIO2) -> LED_BUILTIN (heartbeat, activo en LOW)
 *
 * COMO CLASIFICA:
 *   - Mantiene un buffer circular de 50 lecturas del ADC.
 *   - Calcula "rango" = max - min de las ultimas 50 lecturas.
 *   - Si el rango es chico => reposo; si es grande => vibracion.
 *   - Umbrales (calibrables):
 *       rango <  12  -> REPOSO
 *       12 <= rango < 40  -> VIBRACION BAJA
 *       40 <= rango < 100 -> VIBRACION MEDIA
 *       rango >= 100 -> VIBRACION ALTA
 */

#include <ESP8266WiFi.h>
#include <espnow.h>

// ============================================================
// CONFIGURACION - ID DEL NODO
// ============================================================
#define NODE_ID 8

#define MSG_TYPE_REQUEST        0
#define MSG_TYPE_RESPONSE       1   // (no usado por este nodo)
#define MSG_TYPE_ACCEL_RESPONSE 4   // respuesta de este nodo

#define SERIAL_BAUD      115200
#define HEARTBEAT_MS     1000

// LED (activo en LOW en NodeMCU)
#define LED_PIN   LED_BUILTIN
#define LED_ON    LOW
#define LED_OFF   HIGH

// Entrada analogica (eje X del GY-61)
#define ADC_PIN A0

// ---- Parametros del algoritmo de clasificacion ----
#define N_SAMPLES          50      // tamano de la ventana (50 lecturas)
#define SAMPLE_INTERVAL_MS 10      // intervalo de muestreo (10 ms = 100 Hz)

// Umbrales de vibracion (rango = max - min), calibrables
#define VIB_TH_REST 12    // por debajo => reposo
#define VIB_TH_LOW  40    // por debajo => vibracion baja
#define VIB_TH_MED  100   // por debajo => vibracion media (arriba => alta)

// Niveles de clasificacion
#define VIB_REPOSO 0
#define VIB_BAJA   1
#define VIB_MEDIA  2
#define VIB_ALTA   3

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

// Respuesta de aceleracion/vibracion (16 bytes)
typedef struct __attribute__((packed)) {
  uint8_t  msg_type;    // 4 = accel response
  uint8_t  node_id;     // 8
  uint16_t accel_x;     // valor X actual (promedio de la ventana)
  uint16_t vib_range;   // rango de vibracion (max - min, 0-1023)
  uint8_t  vib_level;   // 0=reposo, 1=baja, 2=media, 3=alta
  uint8_t  reserved;    // reservado (0)
  uint32_t counter;     // contador correlativo
  uint32_t uptime_ms;   // tiempo activo del nodo
} EspNowAccelMessage;   // 16 bytes

// ============================================================
// Variables del algoritmo de muestreo/clasificacion
// ============================================================
static uint16_t buf[N_SAMPLES];   // buffer circular de lecturas
static uint16_t buf_count = 0;    // cuantas muestras validas hay
static uint8_t  buf_idx = 0;      // indice de escritura

static uint16_t current_avg   = 512;   // promedio actual
static uint16_t current_range = 0;     // rango actual (max - min)
static uint8_t  current_level = VIB_REPOSO;  // clasificacion actual

// ============================================================
// Variables compartidas ISR <-> loop
// ============================================================
static volatile bool    g_request_pending = false;
static volatile uint8_t g_master_mac[6];
static uint32_t response_counter = 0;
static bool master_peer_added = false;
static unsigned long last_heartbeat = 0;
static unsigned long last_sample = 0;

// ============================================================
// Clasificar el estado segun el rango de vibracion
// ============================================================
uint8_t categorizeVibration(uint16_t range) {
  if (range < VIB_TH_REST) return VIB_REPOSO;
  if (range < VIB_TH_LOW)  return VIB_BAJA;
  if (range < VIB_TH_MED)  return VIB_MEDIA;
  return VIB_ALTA;
}

// ============================================================
// Tomar una muestra y actualizar estadisticas + clasificacion
// ============================================================
void sampleAccel() {
  buf[buf_idx] = analogRead(ADC_PIN);
  buf_idx = (buf_idx + 1) % N_SAMPLES;
  if (buf_count < N_SAMPLES) buf_count++;

  // Recalcular min, max y suma sobre la ventana
  uint16_t mn = 1023, mx = 0;
  uint32_t sum = 0;
  for (uint16_t i = 0; i < buf_count; i++) {
    uint16_t v = buf[i];
    if (v < mn) mn = v;
    if (v > mx) mx = v;
    sum += v;
  }

  current_avg   = (uint16_t)(sum / buf_count);
  current_range = (uint16_t)(mx - mn);
  current_level = categorizeVibration(current_range);
}

// ============================================================
// Callback ISR: datos recibidos via ESP-NOW
// ============================================================
void onDataReceived(uint8_t *mac, uint8_t *data, uint8_t len) {
  if (len < 2) return;
  uint8_t type = data[0];
  uint8_t id   = data[1];
  if (type == MSG_TYPE_REQUEST && id == NODE_ID) {
    memcpy((void*)g_master_mac, mac, 6);
    g_request_pending = true;
  }
}

// ============================================================
// Callback ISR: confirmacion de envio
// ============================================================
void onDataSent(uint8_t *mac, uint8_t status) {
  (void)mac; (void)status;
}

// ============================================================
// Enviar respuesta (desde loop, NO desde ISR)
// ============================================================
void sendAccelResponse() {
  response_counter++;

  EspNowAccelMessage resp;
  resp.msg_type   = MSG_TYPE_ACCEL_RESPONSE;
  resp.node_id    = NODE_ID;
  resp.accel_x    = current_avg;    // valor X actual
  resp.vib_range  = current_range;  // rango de vibracion
  resp.vib_level  = current_level;  // clasificacion
  resp.reserved   = 0;
  resp.counter    = response_counter;
  resp.uptime_ms  = millis();

  uint8_t mac[6];
  memcpy(mac, (const void*)g_master_mac, 6);

  if (!master_peer_added) {
    esp_now_add_peer(mac, ESP_NOW_ROLE_COMBO, 1, NULL, 0);
    master_peer_added = true;
    delay(10);
  }

  int ret = esp_now_send(mac, (uint8_t*)&resp, sizeof(EspNowAccelMessage));

  Serial.print("[OK] Respuesta nodo ");
  Serial.print(NODE_ID);
  Serial.print(" | x=");
  Serial.print(resp.accel_x);
  Serial.print(" rango=");
  Serial.print(resp.vib_range);
  Serial.print(" nivel=");
  Serial.print(resp.vib_level);
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
  Serial.println("v1.1-fix1 - Sensor GY-61 (vibracion)");
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
  Serial.println("GY-61: X_OUT -> A0 (clasificacion continua de vibracion)");

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
  last_sample = millis();
}

// ============================================================
// Loop
// ============================================================
void loop() {
  unsigned long now = millis();

  // Muestreo continuo del acelerometro (cada SAMPLE_INTERVAL_MS)
  if (now - last_sample >= SAMPLE_INTERVAL_MS) {
    last_sample = now;
    sampleAccel();
  }

  // Heartbeat LED cada 1s (blink corto, sin bloquear el muestreo)
  if (now - last_heartbeat >= HEARTBEAT_MS) {
    last_heartbeat = now;
    digitalWrite(LED_PIN, LED_ON);
    delayMicroseconds(5000);   // 5 ms de pulso
    digitalWrite(LED_PIN, LED_OFF);
  }

  // Procesar request pendiente (entrega la clasificacion actual)
  if (g_request_pending) {
    g_request_pending = false;
    sendAccelResponse();
  }

  delay(1);
}
