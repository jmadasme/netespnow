/**
 * Firmware ESP8266 - Maestro/Puente Central ESP-NOW
 * ====================================================
 * Placa: NodeMCU v3 (ESP-12E, CP2102)
 * 
 * Funcion:
 *   - Reemplaza al ESP32 como maestro central de la red ESP-NOW
 *   - Interroga secuencialmente a cada nodo ESP8266 via ESP-NOW
 *   - Envia un request (tipo=0) con el node_id destino
 *   - Espera la respuesta (tipo=1) con ID + contador correlativo
 *   - Muestra los datos por Serial (actua como puente/salida)
 * 
 * Protocolo ESP-NOW (identico al firmware ESP32/ESP8266 nodo):
 *   Request  (maestro -> nodo): {tipo=0, node_id=X, counter=0}
 *   Response (nodo -> maestro): {tipo=1, node_id=X, counter=N, uptime_ms}
 * 
 * PINOUT NodeMCU v3 (ESP8266):
 *   - D4 (GPIO2) -> LED_BUILTIN (azul, activo en LOW)
 *   - D9 (GPIO1) -> TX (Serial, 115200 baud)
 *   - D10(GPIO3) -> RX (Serial)
 *   - Resto     -> Sin uso en este firmware
 */

#include <ESP8266WiFi.h>
#include <espnow.h>

// ============================================================
// Configuracion de la red ESP-NOW
// ============================================================

// Intervalo entre polls (milisegundos)
#define POLL_INTERVAL_MS  3000

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
#define MSG_TYPE_REQUEST  0
#define MSG_TYPE_RESPONSE 1

// ============================================================
// Estructura del mensaje (debe coincidir con nodos ESP8266)
// ============================================================
typedef struct __attribute__((packed)) {
  uint8_t  msg_type;    // 0=request, 1=response
  uint8_t  node_id;     // ID del nodo (0-5)
  uint32_t counter;     // Contador correlativo
  uint32_t uptime_ms;   // Tiempo de actividad del nodo
} EspNowMessage;

// ============================================================
// Tabla de nodos ESP8266 en la red
// ============================================================
#define NUM_NODES 6

typedef struct {
  uint8_t  id;
  uint8_t  mac[6];
  bool     known;       // true si la MAC ha sido aprendida
} EspNowNode;

// MACs conocidas de las ESP8266 ya programadas
// ACTUALIZADO tras autodescubrimiento de red
// Nota: Nodo 5 era esta placa (MAC 48:3F:DA:05:86:36),
//       ahora es el bridge/master
EspNowNode nodes[NUM_NODES] = {
  { 0, {0x84, 0xF3, 0xEB, 0x7C, 0x99, 0x1E}, true },
  { 1, {0x80, 0x7D, 0x3A, 0x2B, 0xE3, 0x59}, true },
  { 2, {0x8C, 0xAA, 0xB5, 0x0F, 0x39, 0x7F}, true },
  { 3, {0x50, 0x02, 0x91, 0xDE, 0x0D, 0x7B}, true },
  { 4, {0x48, 0x3F, 0xDA, 0xAA, 0x8E, 0x6D}, true },
  { 5, {0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, false },
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

// Timeout para considerar perdida de respuesta
#define RESPONSE_TIMEOUT_MS 1000
unsigned long request_sent_time = 0;

// Estadisticas
uint32_t total_requests = 0;
uint32_t total_responses = 0;
uint32_t total_timeouts = 0;

// MAC del ultimo nodo que respondio (para aprendizaje)
// Usamos volatile porque se escribe desde ISR
static volatile uint8_t g_last_response_mac[6];
static volatile bool    g_response_received = false;
static volatile uint8_t g_response_data[sizeof(EspNowMessage)];

// MAC de esta placa (bridge) para no auto-interrogarse
static uint8_t my_mac[6];

// ============================================================
// Callback ISR: datos recibidos via ESP-NOW
// ============================================================
void onDataReceived(uint8_t *mac, uint8_t *data, uint8_t len) {
  if (len != sizeof(EspNowMessage)) return;

  EspNowMessage *msg = (EspNowMessage*)data;

  if (msg->msg_type == MSG_TYPE_RESPONSE) {
    // Solo nos interesan respuestas
    // Copiar datos (minimo para no bloquear ISR mucho tiempo)
    memcpy((void*)g_last_response_mac, mac, 6);
    memcpy((void*)g_response_data, data, len);
    g_response_received = true;
  }
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
    // MAC desconocida -> broadcast
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
  EspNowMessage msg;

  // Copiar datos del buffer ISR
  memcpy(mac, (const void*)g_last_response_mac, 6);
  memcpy(&msg, (const void*)g_response_data, sizeof(EspNowMessage));

  uint8_t id = msg.node_id;

  // Validar ID
  if (id >= NUM_NODES) {
    Serial.printf("[WARN] ID de nodo invalido: %d\n", id);
    return;
  }

  // Actualizar MAC en la tabla si no la conocemos
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
  Serial.printf("  Contador correlativo: %u\n", msg.counter);
  Serial.printf("  Tiempo activo nodo  : %u ms (%u s)\n",
                msg.uptime_ms, msg.uptime_ms / 1000);
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
  Serial.println("  6x ESP8266 en red ESP-NOW");
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

  // Auto-identificar nodo bridge en la tabla (nodo 5)
  for (int i = 0; i < NUM_NODES; i++) {
    if (memcmp(nodes[i].mac, my_mac, 6) == 0) {
      Serial.printf("  * Nodo %d identificado como el bridge\n", i);
      break;
    }
    if (!nodes[i].known || memcmp(nodes[i].mac, "\x00\x00\x00\x00\x00\x00", 6) == 0) {
      // Primer slot vacio -> asignar MAC del bridge
      memcpy(nodes[i].mac, my_mac, 6);
      nodes[i].known = true;
      Serial.printf("  * Slot Nodo %d asignado como el bridge\n", i);
      break;
    }
  }

  // Inicializar ESP-NOW
  int ret = esp_now_init();
  if (ret != 0) {
    Serial.println("[FATAL] Error al inicializar ESP-NOW");
    Serial.print("esp_now_init() retornó: ");
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
    Serial.println("  ping <id>  : Preguntar a un nodo especifico (0-5)");
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

  // Verificar timeout de respuesta
  if (waiting_response && (now - request_sent_time > RESPONSE_TIMEOUT_MS)) {
    waiting_response = false;
    total_timeouts++;
    Serial.printf("[TIMEOUT] Nodo %d - No se recibio respuesta (%u ms)\n",
                  last_polled_node_id,
                  (unsigned int)(now - request_sent_time));
    Serial.println();
  }

  // Enviar nuevo request (si auto- polling activo y ha pasado el intervalo)
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
