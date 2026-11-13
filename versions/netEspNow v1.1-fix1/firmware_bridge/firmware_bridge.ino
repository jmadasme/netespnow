/**
 * Firmware ESP8266 - Maestro/Puente Central ESP-NOW (v1.1-fix1)
 * ====================================================
 * Placa: NodeMCU v3 (ESP-12E, CP2102)
 *
 * Funcion:
 *   - Interroga secuencialmente a los 5 nodos ESP8266 via ESP-NOW
 *   - Envia un request (tipo=0) con el node_id destino
 *   - Espera la respuesta (tipo=1) con estado digital + valor ADC
 *     + contador correlativo + uptime del nodo
 *   - Muestra los datos por Serial (actua como puente/salida)
 *   - Autodescubrimiento de MAC: si no conoce la MAC del nodo,
 *     envia por broadcast (FF:FF:FF:FF:FF:FF) y la aprende de la
 *     respuesta (la guarda en la tabla y registra el peer directo).
 *
 * Protocolo ESP-NOW (struct de 13 bytes, identica en nodos):
 *   Request  (bridge -> nodo): {tipo=0, node_id=X, gpio_state=0,
 *                               adc_value=0, counter=0, uptime_ms=0}
 *   Response (nodo -> bridge): {tipo=1, node_id=X, gpio_state=D,
 *                               adc_value=A, counter=N, uptime_ms=MMM}
 *
 * NOTA: Los callbacks ESP-NOW se ejecutan en contexto ISR.
 *       NO hacer Serial.print/delay/memcpy pesados dentro de ISR.
 *       Aqui solo se copian la MAC + 13 bytes a buffers volatile.
 *
 * PINOUT NodeMCU v3 (ESP8266):
 *   - D4 (GPIO2) -> LED_BUILTIN (azul, activo en LOW)  [confirmacion]
 *   - D9 (GPIO1) -> TX (Serial, 115200 baud)
 *   - D10(GPIO3) -> RX (Serial)
 *   - D5 (GPIO14) -> RESERVADO para SoftwareSerial RX (Fase 2)
 *   - D6 (GPIO12) -> RESERVADO para SoftwareSerial TX (Fase 2)
 *   - D2 / A0    -> Sin uso en el bridge (solo los nodos leen sensores)
 */

#include <ESP8266WiFi.h>
#include <espnow.h>

// ============================================================
// Configuracion de la red ESP-NOW
// ============================================================

// Intervalo entre polls (milisegundos)
#define POLL_INTERVAL_MS  3000

// Timeout para considerar perdida de respuesta
// (2000 ms para dar margen a la lectura DS18B20 del nodo 6, ~750 ms)
#define RESPONSE_TIMEOUT_MS 2000

// Baudrate del monitor serial
#define SERIAL_BAUD       115200

// LED indicador (activo en LOW en NodeMCU)
#define LED_PIN           LED_BUILTIN
#define LED_ON            LOW
#define LED_OFF           HIGH

// MAC de broadcast
const uint8_t ESP_NOW_BROADCAST_MAC[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// ============================================================
// Tipos de mensaje (debe coincidir con firmware de nodos)
// ============================================================
#define MSG_TYPE_REQUEST       0
#define MSG_TYPE_RESPONSE      1   // nodo universal (gpio + adc)
#define MSG_TYPE_DHT_RESPONSE  2   // nodo 6 DS18B20 (temp; hum N/A)
#define MSG_TYPE_IR_RESPONSE   3   // nodo 7 GY-906 (temp objeto + ambiente)
#define MSG_TYPE_ACCEL_RESPONSE 4  // nodo 8 GY-61 (acelerometro 3 ejes)

// Marcador de "lectura invalida" (valores en *10)
#define SENSOR_INVALID  -9999

// Marcador de "eje no conectado" (acelerometro)
#define ACCEL_INVALID  0xFFFF

// ============================================================
// Estructura del mensaje (debe coincidir con nodos ESP8266)
// 13 bytes exactos: msg_type(1) + node_id(1) + gpio_state(1)
//                   + adc_value(2) + counter(4) + uptime_ms(4)
// ============================================================
typedef struct __attribute__((packed)) {
  uint8_t  msg_type;    // 0=request, 1=response
  uint8_t  node_id;     // ID del nodo destino/origen (0-5)
  uint8_t  gpio_state;  // Estado digital 0/1 (request: 0)
  uint16_t adc_value;   // Valor ADC 0-1023   (request: 0)
  uint32_t counter;     // Contador correlativo (nodo)
  uint32_t uptime_ms;   // Tiempo de actividad del nodo
} EspNowMessage;

// Respuesta DS18B20 (nodo 6): 14 bytes
typedef struct __attribute__((packed)) {
  uint8_t  msg_type;    // 2 = sensor response (temp)
  uint8_t  node_id;     // 6
  int16_t  temp_x10;    // temperatura en °C * 10 (ej: 234 = 23.4°C)
  int16_t  hum_x10;     // humedad en % * 10   (N/A para DS18B20)
  uint32_t counter;     // Contador correlativo (nodo)
  uint32_t uptime_ms;   // Tiempo de actividad del nodo
} EspNowDhtMessage;

// Respuesta IR GY-906 (nodo 7): 14 bytes
typedef struct __attribute__((packed)) {
  uint8_t  msg_type;       // 3 = IR response
  uint8_t  node_id;        // 7
  int16_t  obj_temp_x10;   // temperatura del objeto en °C * 10
  int16_t  amb_temp_x10;   // temperatura ambiente en °C * 10
  uint32_t counter;        // Contador correlativo (nodo)
  uint32_t uptime_ms;      // Tiempo de actividad del nodo
} EspNowIrMessage;

// Respuesta acelerometro/vibracion GY-61 (nodo 8): 16 bytes
typedef struct __attribute__((packed)) {
  uint8_t  msg_type;    // 4 = accel response
  uint8_t  node_id;     // 8
  uint16_t accel_x;     // valor X actual (promedio de la ventana)
  uint16_t vib_range;   // rango de vibracion (max - min)
  uint8_t  vib_level;   // 0=reposo, 1=baja, 2=media, 3=alta
  uint8_t  reserved;    // reservado (0)
  uint32_t counter;     // Contador correlativo (nodo)
  uint32_t uptime_ms;   // Tiempo de actividad del nodo
} EspNowAccelMessage;

// ============================================================
// Tabla de nodos ESP8266 en la red
// ============================================================
#define NUM_NODES 9   // 0-4 universales, 5 bridge (self), 6 DS18B20, 7 IR, 8 accel

typedef struct {
  uint8_t  id;
  uint8_t  mac[6];
  bool     known;       // true si la MAC ha sido aprendida
} EspNowNode;

// Tabla de nodos: los slots 0-4 arrancan DESCONOCIDOS (MAC en cero)
// y se aprenden por autodescubrimiento (broadcast) cuando la tarjeta
// nueva responde. El slot 5 queda reservado para el propio bridge.
EspNowNode nodes[NUM_NODES] = {
  { 0, {0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, false },
  { 1, {0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, false },
  { 2, {0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, false },
  { 3, {0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, false },
  { 4, {0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, false },
  { 5, {0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, false },  // reservado bridge (self)
  { 6, {0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, false },  // nodo DS18B20
  { 7, {0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, false },  // nodo GY-906 (IR)
  { 8, {0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, false },  // nodo GY-61 (acelerometro)
};

// Indice del nodo actual que se esta interrogando
uint8_t current_node_idx = 0;

// ID del nodo al que se le envio el ultimo request (para timeout)
uint8_t last_polled_node_id = 0;

// Interrogacion automatica activada/desactivada
bool auto_poll_enabled = true;

// Timestamp del ultimo poll
unsigned long last_poll_time = 0;

// Flag: esperando respuesta
volatile bool waiting_response = false;

// Timestamp del envio del request en vuelo
unsigned long request_sent_time = 0;

// Estadisticas
uint32_t total_requests = 0;
uint32_t total_responses = 0;
uint32_t total_timeouts = 0;

// MAC del ultimo nodo que respondio (para aprendizaje)
// Usamos volatile porque se escribe desde ISR
static volatile uint8_t g_last_response_mac[6];
static volatile bool    g_response_received = false;
static volatile uint8_t g_response_data[sizeof(EspNowAccelMessage)];  // 16 B (cubre el mayor)
static volatile uint8_t g_response_len = 0;

// MAC de esta placa (bridge) para no auto-interrogarse
static uint8_t my_mac[6];

// ============================================================
// Callback ISR: datos recibidos via ESP-NOW
// ============================================================
void onDataReceived(uint8_t *mac, uint8_t *data, uint8_t len) {
  // Acepta respuestas universales (13 B), DS18B20/IR (14 B) y accel (16 B)
  bool valid = false;
  if (len == sizeof(EspNowMessage)) {
    EspNowMessage *msg = (EspNowMessage*)data;
    valid = (msg->msg_type == MSG_TYPE_RESPONSE);
  } else if (len == sizeof(EspNowDhtMessage)) {
    // EspNowDhtMessage y EspNowIrMessage tienen el mismo tamaño (14 B)
    EspNowDhtMessage *msg = (EspNowDhtMessage*)data;
    valid = (msg->msg_type == MSG_TYPE_DHT_RESPONSE ||
             msg->msg_type == MSG_TYPE_IR_RESPONSE);
  } else if (len == sizeof(EspNowAccelMessage)) {
    EspNowAccelMessage *msg = (EspNowAccelMessage*)data;
    valid = (msg->msg_type == MSG_TYPE_ACCEL_RESPONSE);
  }

  if (!valid) return;

  // Copiar datos (minimo para no bloquear ISR mucho tiempo)
  memcpy((void*)g_last_response_mac, mac, 6);
  memcpy((void*)g_response_data, data, len);
  g_response_len = len;
  g_response_received = true;
  // Si es un request (de otro nodo que nos confunde con el),
  // lo ignoramos - no respondemos como esclavo
}

// ============================================================
// Callback ISR: confirmacion de envio
// ============================================================
void onDataSent(uint8_t *mac, uint8_t status) {
  (void)mac;
  // status: 0=exito, otro=fallo
  if (status != 0) {
    waiting_response = false;
  }
}

// ============================================================
// Enviar request a un nodo especifico
// ============================================================
void sendRequest(uint8_t node_id) {
  EspNowMessage request;
  request.msg_type = MSG_TYPE_REQUEST;
  request.node_id = node_id;
  request.gpio_state = 0;   // el nodo lee sus sensores al responder
  request.adc_value = 0;    // idem
  request.counter = 0;
  request.uptime_ms = 0;

  const uint8_t *target_mac;
  bool use_broadcast;

  if (nodes[node_id].known) {
    // Envio directo a la MAC conocida
    target_mac = nodes[node_id].mac;
    use_broadcast = false;

    // Asegurar que el peer esta registrado
    if (!esp_now_is_peer_exist((uint8_t*)target_mac)) {
      esp_now_add_peer((uint8_t*)target_mac, ESP_NOW_ROLE_COMBO, 1, NULL, 0);
    }
  } else {
    // MAC desconocida -> broadcast para autodescubrimiento
    target_mac = ESP_NOW_BROADCAST_MAC;
    use_broadcast = true;
  }

  int result = esp_now_send((uint8_t*)target_mac, (uint8_t*)&request, sizeof(EspNowMessage));

  if (result == 0) {
    total_requests++;
    waiting_response = true;
    request_sent_time = millis();

    if (use_broadcast) {
      Serial.printf("[ENVIO] Request broadcast para nodo %d\n", node_id);
    } else {
      Serial.printf("[ENVIO] Request -> Nodo %d [%02X:%02X:%02X:%02X:%02X:%02X]\n",
                    node_id,
                    target_mac[0], target_mac[1], target_mac[2],
                    target_mac[3], target_mac[4], target_mac[5]);
    }
  } else {
    Serial.printf("[ERROR] Fallo al enviar request a nodo %d (codigo=%d)\n", node_id, result);
    waiting_response = false;
  }
}

// ============================================================
// Procesar respuesta recibida (llamado desde loop, NO desde ISR)
// ============================================================
void processResponse() {
  if (!g_response_received) return;
  g_response_received = false;

  uint8_t mac[6];
  uint8_t len = g_response_len;
  uint8_t data[sizeof(EspNowAccelMessage)];

  // Copiar datos del buffer ISR
  memcpy(mac, (const void*)g_last_response_mac, 6);
  memcpy(data, (const void*)g_response_data, len);

  uint8_t type = data[0];   // msg_type (primer byte en todas las structs)
  uint8_t id   = data[1];   // node_id  (segundo byte en todas las structs)

  // Validar ID
  if (id >= NUM_NODES) {
    Serial.printf("[WARN] ID de nodo invalido: %d\n", id);
    return;
  }

  // Actualizar MAC en la tabla si no la conocemos (autodescubrimiento)
  if (!nodes[id].known) {
    memcpy(nodes[id].mac, mac, 6);
    nodes[id].known = true;
    Serial.printf("[APRENDIZAJE] MAC del nodo %d: %02X:%02X:%02X:%02X:%02X:%02X\n",
                  id, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    // Agregar peer con MAC conocida
    if (!esp_now_is_peer_exist(mac)) {
      esp_now_add_peer(mac, ESP_NOW_ROLE_COMBO, 1, NULL, 0);
    }
  }

  total_responses++;
  waiting_response = false;

  // Calcular RTT aproximado
  uint32_t rtt = millis() - request_sent_time;

  Serial.println("----------------------------------------");
  Serial.printf("RESPUESTA RECIBIDA - Nodo #%d\n", id);
  Serial.printf("  MAC                 : %02X:%02X:%02X:%02X:%02X:%02X\n",
                mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

  if (type == MSG_TYPE_DHT_RESPONSE) {
    // Respuesta del nodo 6 (DS18B20: temperatura; humedad N/A)
    EspNowDhtMessage *dht = (EspNowDhtMessage*)data;
    if (dht->temp_x10 == SENSOR_INVALID) {
      Serial.println("  Temperatura (DS18B20): ERROR LECTURA");
    } else {
      Serial.printf("  Temperatura (DS18B20): %.1f C\n", dht->temp_x10 / 10.0f);
    }
    if (dht->hum_x10 == SENSOR_INVALID) {
      Serial.println("  Humedad              : N/A");
    } else {
      Serial.printf("  Humedad              : %.1f %%\n", dht->hum_x10 / 10.0f);
    }
    Serial.printf("  Contador correlativo: %u\n", dht->counter);
    Serial.printf("  Tiempo activo nodo  : %u ms (%u s)\n",
                  dht->uptime_ms, dht->uptime_ms / 1000);
  } else if (type == MSG_TYPE_IR_RESPONSE) {
    // Respuesta del nodo 7 (GY-906: temp objeto + ambiente)
    EspNowIrMessage *ir = (EspNowIrMessage*)data;
    if (ir->obj_temp_x10 == SENSOR_INVALID) {
      Serial.println("  Temp. objeto (IR)   : ERROR LECTURA");
    } else {
      Serial.printf("  Temp. objeto (IR)   : %.1f C\n", ir->obj_temp_x10 / 10.0f);
    }
    if (ir->amb_temp_x10 == SENSOR_INVALID) {
      Serial.println("  Temp. ambiente      : ERROR LECTURA");
    } else {
      Serial.printf("  Temp. ambiente      : %.1f C\n", ir->amb_temp_x10 / 10.0f);
    }
    Serial.printf("  Contador correlativo: %u\n", ir->counter);
    Serial.printf("  Tiempo activo nodo  : %u ms (%u s)\n",
                  ir->uptime_ms, ir->uptime_ms / 1000);
  } else if (type == MSG_TYPE_ACCEL_RESPONSE) {
    // Respuesta del nodo 8 (GY-61: acelerometro + clasificacion de vibracion)
    EspNowAccelMessage *acc = (EspNowAccelMessage*)data;
    Serial.printf("  Acel. X (A0)        : %u\n", acc->accel_x);
    Serial.printf("  Rango vibracion     : %u\n", acc->vib_range);
    Serial.print("  Estado              : ");
    switch (acc->vib_level) {
      case 0: Serial.println("REPOSO"); break;
      case 1: Serial.println("VIBRACION BAJA"); break;
      case 2: Serial.println("VIBRACION MEDIA"); break;
      case 3: Serial.println("VIBRACION ALTA"); break;
      default: Serial.println("DESCONOCIDO"); break;
    }
    Serial.printf("  Contador correlativo: %u\n", acc->counter);
    Serial.printf("  Tiempo activo nodo  : %u ms (%u s)\n",
                  acc->uptime_ms, acc->uptime_ms / 1000);
  } else {
    // Respuesta de nodo universal (gpio + adc)
    EspNowMessage *msg = (EspNowMessage*)data;
    Serial.printf("  Estado digital (D2) : %d  (%s)\n",
                  msg->gpio_state,
                  msg->gpio_state == 1 ? "HIGH" : "LOW");
    Serial.printf("  Valor ADC (A0)      : %u (0-1023)\n", msg->adc_value);
    Serial.printf("  Contador correlativo: %u\n", msg->counter);
    Serial.printf("  Tiempo activo nodo  : %u ms (%u s)\n",
                  msg->uptime_ms, msg->uptime_ms / 1000);
  }

  Serial.printf("  RTT                 : %u ms\n", rtt);
  Serial.println("----------------------------------------");

  // Parpadeo de confirmacion
  digitalWrite(LED_PIN, LED_ON);
  delay(20);
  digitalWrite(LED_PIN, LED_OFF);
}

// ============================================================
// Setup
// ============================================================
void setup() {
  Serial.begin(SERIAL_BAUD);
  Serial.println();
  Serial.println("============================================");
  Serial.println("  ESP8266 MAESTRO - Red ESP-NOW (Bridge)");
  Serial.println("  v1.1-fix1 - Sensores digital + ADC");
  Serial.println("============================================");
  Serial.println();

  // LED indicador
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LED_OFF);

  // Inicializar WiFi en modo Station
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);

  Serial.print("MAC del ESP8266 maestro (bridge): ");
  Serial.println(WiFi.macAddress());
  Serial.println();

  // Guardar MAC para no auto-interrogarse
  WiFi.macAddress(my_mac);
  Serial.printf("  MAC bridge: %02X:%02X:%02X:%02X:%02X:%02X\n",
                my_mac[0], my_mac[1], my_mac[2],
                my_mac[3], my_mac[4], my_mac[5]);

  // Reservar el slot 5 para el propio bridge (self-identificacion).
  // Los slots 0-4 permanecen DESCONOCIDOS y se aprenden por broadcast
  // a medida que cada nodo nuevo responda (autodescubrimiento).
  memcpy(nodes[5].mac, my_mac, 6);
  nodes[5].known = true;
  Serial.printf("  * Slot Nodo 5 reservado para el bridge (self)\n");

  // Inicializar ESP-NOW
  int ret = esp_now_init();
  if (ret != 0) {
    Serial.println("[FATAL] Error al inicializar ESP-NOW");
    Serial.print("esp_now_init() retorno: ");
    Serial.println(ret);
    Serial.println("Reiniciando en 5 segundos...");
    delay(5000);
    ESP.restart();
  }

  // Configurar rol: COMBO (envia y recibe)
  esp_now_set_self_role(ESP_NOW_ROLE_COMBO);

  // Registrar callbacks
  esp_now_register_recv_cb(onDataReceived);
  esp_now_register_send_cb(onDataSent);

  // Agregar peer broadcast (para descubrimiento de nodos)
  ret = esp_now_add_peer((uint8_t*)ESP_NOW_BROADCAST_MAC, ESP_NOW_ROLE_COMBO, 1, NULL, 0);
  if (ret != 0) {
    Serial.printf("[WARN] No se pudo agregar peer broadcast (codigo=%d)\n", ret);
  }

  // Agregar peers conocidos (envio directo, saltando el bridge)
  for (int i = 0; i < NUM_NODES; i++) {
    if (nodes[i].known && !isSelfNode(i)) {
      ret = esp_now_add_peer(nodes[i].mac, ESP_NOW_ROLE_COMBO, 1, NULL, 0);
      if (ret == 0) {
        Serial.printf("  + Peer directo: Nodo %d [%02X:%02X:%02X:%02X:%02X:%02X]\n",
                      i,
                      nodes[i].mac[0], nodes[i].mac[1], nodes[i].mac[2],
                      nodes[i].mac[3], nodes[i].mac[4], nodes[i].mac[5]);
      } else {
        Serial.printf("  ! Peer Nodo %d no agregado (codigo=%d)\n", i, ret);
      }
    }
  }

  Serial.println();
  Serial.println("[OK] ESP-NOW inicializado correctamente");
  Serial.println();
  Serial.println("Iniciando interrogacion de nodos...");
  Serial.printf("Intervalo entre polls: %d ms\n", POLL_INTERVAL_MS);
  Serial.printf("Timeout de respuesta:  %d ms\n", RESPONSE_TIMEOUT_MS);
  Serial.println("----------------------------------------");
  Serial.println();

  // Parpadeo de inicio: 3 parpadeos
  for (int i = 0; i < 3; i++) {
    digitalWrite(LED_PIN, LED_ON);
    delay(100);
    digitalWrite(LED_PIN, LED_OFF);
    delay(100);
  }

  last_poll_time = millis();
}

// ============================================================
// Buffer para comandos seriales
// ============================================================
#define SERIAL_BUF_SIZE 32
static char serial_buffer[SERIAL_BUF_SIZE];
static uint8_t serial_idx = 0;

// ============================================================
// Procesar comandos enviados por el usuario via Serial
// ============================================================
void processSerialCommand(const char *cmd) {
  // Saltar espacios iniciales
  while (*cmd == ' ') cmd++;

  if (strcmp(cmd, "help") == 0 || strcmp(cmd, "?") == 0) {
    Serial.println();
    Serial.println("=== COMANDOS DISPONIBLES ===");
    Serial.println("  ping <id>  : Preguntar a un nodo especifico (0-4, 6-8)");
    Serial.println("  ping all   : Preguntar a todos los nodos");
    Serial.println("  list       : Mostrar tabla de nodos conocidos");
    Serial.println("  stats      : Mostrar estadisticas");
    Serial.println("  help / ?   : Mostrar esta ayuda");
    Serial.println("  auto on    : Activar interrogacion automatica");
    Serial.println("  auto off   : Desactivar interrogacion automatica");
    Serial.println("=============================");
    Serial.println();
    return;
  }

  if (strcmp(cmd, "list") == 0) {
    Serial.println("=== TABLA DE NODOS ===");
    for (int i = 0; i < NUM_NODES; i++) {
      const char *estado;
      if (isSelfNode(i)) {
        estado = "BRIDGE";
      } else if (nodes[i].known) {
        estado = "CONOCIDO";
      } else {
        estado = "DESCONOCIDO";
      }
      Serial.printf("  Nodo %d: %s [%02X:%02X:%02X:%02X:%02X:%02X]\n",
                    nodes[i].id, estado,
                    nodes[i].mac[0], nodes[i].mac[1], nodes[i].mac[2],
                    nodes[i].mac[3], nodes[i].mac[4], nodes[i].mac[5]);
    }
    Serial.println("========================");
    return;
  }

  if (strcmp(cmd, "stats") == 0) {
    printStats();
    return;
  }

  if (strcmp(cmd, "auto on") == 0) {
    auto_poll_enabled = true;
    last_poll_time = millis() - POLL_INTERVAL_MS; // enviar inmediato
    Serial.println("[OK] Interrogacion automatica ACTIVADA");
    return;
  }

  if (strcmp(cmd, "auto off") == 0) {
    auto_poll_enabled = false;
    Serial.println("[OK] Interrogacion automatica DESACTIVADA");
    Serial.println("(Solo responde a comandos: ping, list, stats)");
    return;
  }

  // Comando: ping <id> o ping all
  if (strncmp(cmd, "ping", 4) == 0) {
    cmd += 4;
    while (*cmd == ' ') cmd++;

    if (strcmp(cmd, "all") == 0) {
      if (waiting_response) {
        Serial.println("[WARN] Esperando respuesta, intente en un momento");
        return;
      }
      Serial.println("[CMD] Preguntando a TODOS los nodos...");
      // Activar barrido temporal desde el inicio
      auto_poll_enabled = true;
      current_node_idx = 0;
      last_poll_time = millis() - POLL_INTERVAL_MS; // enviar inmediato
      return;
    }

    // ping <id>
    int id = atoi(cmd);
    if (id >= 0 && id < NUM_NODES) {
      if (isSelfNode(id)) {
        Serial.printf("[INFO] Nodo %d es el bridge, no puede interrogarse a si mismo\n", id);
        return;
      }
      if (waiting_response) {
        Serial.printf("[WARN] Esperando respuesta, intente en un momento\n");
        return;
      }
      Serial.printf("[CMD] Preguntando a Nodo %d...\n", id);
      last_polled_node_id = id;
      sendRequest(id);
      // El auto-polling seguira desde donde iba despues
      return;
    }

    Serial.printf("[ERROR] ID invalido: %s (use 0-%d)\n", cmd, NUM_NODES - 1);
    return;
  }

  // Comando no reconocido
  Serial.printf("[ERROR] Comando desconocido: '%s' (escriba 'help' para ayuda)\n", cmd);
}

// ============================================================
// Leer comando desde serial (linea completa)
// ============================================================
void readSerialCommands() {
  while (Serial.available()) {
    char c = Serial.read();

    if (c == '\n' || c == '\r') {
      if (serial_idx > 0) {
        serial_buffer[serial_idx] = '\0';
        processSerialCommand(serial_buffer);
        serial_idx = 0;
      }
    } else if (serial_idx < SERIAL_BUF_SIZE - 1) {
      serial_buffer[serial_idx++] = c;
    }
  }
}

// ============================================================
// Verificar si un nodo es esta misma placa (bridge)
// ============================================================
bool isSelfNode(uint8_t node_id) {
  if (node_id >= NUM_NODES) return false;
  return (memcmp(nodes[node_id].mac, my_mac, 6) == 0);
}

// ============================================================
// Imprimir estadisticas
// ============================================================
void printStats() {
    Serial.println("=== ESTADISTICAS ===");
    Serial.printf("  Requests enviados:  %u\n", total_requests);
    Serial.printf("  Respuestas OK:      %u\n", total_responses);
    Serial.printf("  Timeouts:           %u\n", total_timeouts);
    if (total_requests > 0) {
      Serial.printf("  Tasa de exito:      %u%%\n",
                    (total_responses * 100) / total_requests);
    }
    Serial.println("  Nodos conocidos:");
    for (int i = 0; i < NUM_NODES; i++) {
      Serial.printf("    Nodo %d: %s [%02X:%02X:%02X:%02X:%02X:%02X]\n",
                    nodes[i].id,
                    nodes[i].known ? "CONOCIDO" : "DESCONOCIDO",
                    nodes[i].mac[0], nodes[i].mac[1], nodes[i].mac[2],
                    nodes[i].mac[3], nodes[i].mac[4], nodes[i].mac[5]);
    }
    Serial.println("====================");
    Serial.println();
}

// ============================================================
// Loop principal
// ============================================================
void loop() {
  unsigned long now = millis();

  // Procesar respuestas recibidas (desde buffer ISR)
  processResponse();

  // Leer y procesar comandos del usuario por Serial
  readSerialCommands();

  // Verificar timeout de respuesta.
  // Se usa millis() fresco aqui (no el `now` capturado al inicio del loop)
  // para evitar el subdesbordamiento que ocurre cuando el request se envia
  // en esta misma iteracion desde readSerialCommands()/ping (entonces
  // request_sent_time > now y la resta se desborda a 0xFFFFFFFF).
  unsigned long now_timeout = millis();
  if (waiting_response && (now_timeout - request_sent_time > RESPONSE_TIMEOUT_MS)) {
    waiting_response = false;
    total_timeouts++;
    Serial.printf("[TIMEOUT] Nodo %d - No se recibio respuesta (%u ms)\n",
                  last_polled_node_id,
                  (unsigned int)(now_timeout - request_sent_time));
    Serial.println();
  }

  // Enviar nuevo request (si auto-polling activo y ha pasado el intervalo)
  if (auto_poll_enabled && !waiting_response && (now - last_poll_time >= POLL_INTERVAL_MS)) {
    last_poll_time = now;

    // Saltar nodos que sean esta misma placa (bridge)
    int intentos = 0;
    while (isSelfNode(current_node_idx) && intentos < NUM_NODES) {
      current_node_idx = (current_node_idx + 1) % NUM_NODES;
      intentos++;
    }

    // Guardar ID antes de avanzar (para timeout)
    last_polled_node_id = nodes[current_node_idx].id;

    // Enviar request al nodo actual
    sendRequest(last_polled_node_id);

    // Avanzar al siguiente nodo
    current_node_idx = (current_node_idx + 1) % NUM_NODES;
  }

  // Mostrar estadisticas cada 30 segundos (solo si auto-poll activo)
  static unsigned long last_stats_time = 0;
  if (auto_poll_enabled && (now - last_stats_time >= 30000)) {
    last_stats_time = now;
    printStats();
  }
}
