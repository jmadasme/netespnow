/**
 * Firmware ESP8266 - Nodo Sensor Universal ESP-NOW (v1.1-fix1)
 * =========================================
 * Placa: NodeMCU v3 (ESP-12E, CP2102)
 *
 * Funcion:
 *   - Escucha peticiones del bridge (maestro) via ESP-NOW
 *   - Cuando recibe un request (tipo=0) dirigido a su NODE_ID,
 *     lee en ese instante la entrada digital (D2/GPIO4 con pull-up)
 *     y el ADC (A0, 0-1023), y responde (tipo=1) con:
 *       gpio_state + adc_value + contador correlativo + uptime_ms
 *   - LED_BUILTIN como heartbeat (parpadea cada 1 s)
 *
 * Protocolo ESP-NOW (struct de 13 bytes, identica al bridge):
 *   Request  (bridge -> nodo): {tipo=0, node_id=X, gpio_state=0,
 *                               adc_value=0, counter=0, uptime_ms=0}
 *   Response (nodo -> bridge): {tipo=1, node_id=X, gpio_state=D,
 *                               adc_value=A, counter=N, uptime_ms=MMM}
 *
 * NOTA: Los callbacks ESP-NOW se ejecutan en contexto ISR.
 *       NO hacer Serial.print/delay/memcpy lentos dentro de ISR.
 *       Aqui solo se copia la MAC del bridge + un flag; el resto
 *       (lectura de sensores, armado y envio) se hace en loop().
 *
 * Mapeo de sensores (por cableado, NO hardcodeado):
 *   - D2 (GPIO4) entrada digital INPUT_PULLUP -> reed/PIR
 *   - A0 (ADC)   entrada analogica 0-1023      -> LDR/MQ135/LM35
 *
 * PINOUT NodeMCU v3:
 *   - D4 (GPIO2) -> LED_BUILTIN (azul, activo en LOW)
 *   - D2 (GPIO4) -> Sensor digital (INPUT_PULLUP)
 *   - A0         -> Sensor analogico (ADC 0-1023)
 *   - D5 (GPIO14) -> RESERVADO para SoftwareSerial RX (Fase 2)
 *   - D6 (GPIO12) -> RESERVADO para SoftwareSerial TX (Fase 2)
 */

#include <ESP8266WiFi.h>
#include <espnow.h>

// ============================================================
// CONFIGURACION - ID DEL NODO (unica diferencia entre nodos)
// ============================================================
#define NODE_ID 2

#define MSG_TYPE_REQUEST  0
#define MSG_TYPE_RESPONSE 1
#define SERIAL_BAUD       115200
#define HEARTBEAT_MS      1000   // LED parpadea cada 1s

// LED (activo en LOW en NodeMCU)
#define LED_PIN   LED_BUILTIN
#define LED_ON    LOW
#define LED_OFF   HIGH

// Entrada digital con pull-up interno: D2 = GPIO4
#define GPIO_DIGITAL 4

// Entrada analogica: A0 (ADC 10 bits, 0-1023)
#define ADC_PIN A0

// Estructura del mensaje (debe coincidir con el bridge)
// 13 bytes exactos: msg_type(1) + node_id(1) + gpio_state(1)
//                   + adc_value(2) + counter(4) + uptime_ms(4)
typedef struct __attribute__((packed)) {
  uint8_t  msg_type;    // 0=request, 1=response
  uint8_t  node_id;     // ID del nodo destino/origen (0-5)
  uint8_t  gpio_state;  // Estado digital 0/1 (request: 0)
  uint16_t adc_value;   // Valor ADC 0-1023   (request: 0)
  uint32_t counter;     // Contador correlativo (nodo)
  uint32_t uptime_ms;   // Tiempo de actividad del nodo
} EspNowMessage;

// Variables compartidas ISR <-> loop
static volatile bool    g_request_pending = false;
static volatile uint8_t g_master_mac[6];
static uint32_t response_counter = 0;
static bool master_peer_added = false;
static unsigned long last_heartbeat = 0;

// ============================================================
// Callback ISR: datos recibidos
// ============================================================
void onDataReceived(uint8_t *mac, uint8_t *data, uint8_t len) {
  // Validacion de longitud: 13 bytes del struct ampliado
  if (len != sizeof(EspNowMessage)) return;

  EspNowMessage *msg = (EspNowMessage*)data;
  if (msg->msg_type == MSG_TYPE_REQUEST && msg->node_id == NODE_ID) {
    // Solo copiamos la MAC del bridge + flag (minimo en ISR)
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
// Lee los sensores EN EL INSTANTE de responder
// ============================================================
void sendResponse() {
  response_counter++;

  // Lectura fresca de sensores en el momento de responder
  uint8_t  gpio_state = digitalRead(GPIO_DIGITAL) == HIGH ? 1 : 0;
  uint16_t adc_value  = analogRead(ADC_PIN);

  EspNowMessage resp;
  resp.msg_type = MSG_TYPE_RESPONSE;
  resp.node_id = NODE_ID;
  resp.gpio_state = gpio_state;
  resp.adc_value = adc_value;
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
  Serial.print(" | gpio=");
  Serial.print(gpio_state);
  Serial.print(" | adc=");
  Serial.print(adc_value);
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
  Serial.println("v1.1-fix1 - Sensor digital + ADC");
  Serial.println("======================================");

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LED_OFF);

  // Entrada digital con pull-up interno (reed/PIR a GND)
  pinMode(GPIO_DIGITAL, INPUT_PULLUP);

  // Entrada analogica A0 (0-1023)
  // analogRead() no requiere pinMode(A0,...) en ESP8266

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);

  Serial.print("MAC: ");
  Serial.println(WiFi.macAddress());
  Serial.print("FW: ESP-NOW Nodo v1.1-fix1 ID=");
  Serial.println(NODE_ID);
  Serial.println("Entrada digital: D2 (GPIO4) INPUT_PULLUP");
  Serial.println("Entrada analogica: A0 (0-1023)");
  Serial.println();

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

  // Procesar request pendiente (lectura de sensores + respuesta)
  if (g_request_pending) {
    g_request_pending = false;
    sendResponse();
  }

  delay(1);
}
