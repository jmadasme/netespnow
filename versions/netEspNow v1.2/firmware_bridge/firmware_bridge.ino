/**
 * Firmware ESP8266 - Bridge Central con Cifrado ECC (netEspNow v1.2)
 * ====================================================================
 * Placa: NodeMCU v3 (ESP-12E, CP2102)
 * 
 * Funcion:
 *   - Recibe comandos del usuario via Serial (115200 baud)
 *   - CIFRA los comandos con ECC asimetrico y los envia a la Puerta de Enlace
 *   - La Puerta de Enlace descifra, interroga al nodo, y devuelve respuesta cifrada
 *   - Bridge descifra la respuesta y la muestra por Serial
 * 
 * Diferencia con v1.1:
 *   - v1.1: Bridge hablaba DIRECTAMENTE con los nodos 0-4
 *   - v1.2: Bridge habla SOLO con la Puerta de Enlace (cifrado)
 *           La Puerta de Enlace habla con los nodos
 * 
 * Protocolo ESP-NOW:
 *   Bridge <-> Puerta: Mensajes cifrados (msg_type=2/3) con ECC
 *   Puerta <-> Nodos:  Mensajes planos (msg_type=0/1) - protocolo v1.1
 * 
 * Hardware:
 *   - D4 (GPIO2) -> LED_BUILTIN (azul, activo en LOW)
 *   - D9 (GPIO1) -> TX (Serial, 115200 baud)
 *   - D10(GPIO3) -> RX (Serial)
 * 
 * Version: 1.2
 */

#include <ESP8266WiFi.h>
#include <espnow.h>
#include <EEPROM.h>
#include <uECC.h>

// ============================================================
// Configuracion general
// ============================================================

#define SERIAL_BAUD         115200
#define POLL_INTERVAL_MS    3000     // Intervalo entre auto-polls
#define RESPONSE_TIMEOUT_MS 2000     // Timeout espera respuesta Puerta de Enlace
#define LED_PIN             LED_BUILTIN
#define LED_ON              LOW
#define LED_OFF             HIGH

// MAC de broadcast
const uint8_t ESP_NOW_BROADCAST_MAC[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// ============================================================
// MAC de la Puerta de Enlace (conocida, hardcodeada tras 
// aprendizaje. Inicialmente se envia a broadcast)
// ============================================================
// IMPORTANTE: Reemplazar con la MAC real de la placa ceramica
// despues de la primera programacion.
static uint8_t puerta_mac[6] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
static bool    puerta_mac_known = false;

// ============================================================
// Tipos de mensaje ESP-NOW
// ============================================================
#define MSG_TYPE_REQUEST     0   // Plano: request a nodo
#define MSG_TYPE_RESPONSE    1   // Plano: respuesta de nodo
#define MSG_TYPE_CRYPT_REQ   2   // Cifrado: Bridge -> Puerta de Enlace
#define MSG_TYPE_CRYPT_RESP  3   // Cifrado: Puerta de Enlace -> Bridge

// ============================================================
// Estructura: Mensaje plano (solo referencia - compatibilidad)
// ============================================================
typedef struct __attribute__((packed)) {
  uint8_t  msg_type;
  uint8_t  node_id;
  uint32_t counter;
  uint32_t uptime_ms;
} EspNowMessage;

// ============================================================
// Estructura: Mensaje cifrado (Bridge <-> Puerta de Enlace)
// ============================================================
#define CRYPTED_PAYLOAD_MAX 32
typedef struct __attribute__((packed)) {
  uint8_t  msg_type;       // 2=bridge->puerta, 3=puerta->bridge
  uint8_t  node_id;        // ID del nodo destino (0-4) o 0xFF
  uint32_t counter;        // Contador correlativo
  uint8_t  payload[CRYPTED_PAYLOAD_MAX]; // Datos cifrados
  uint8_t  payload_len;    // Longitud real del payload
  uint8_t  signature[64];  // Firma ECDSA
} EspNowCryptedMessage;

// ============================================================
// EEPROM Layout
// ============================================================
#define EEPROM_SIZE          512
#define EEPROM_MAGIC         0x4E533132  // "NS12"
#define EEPROM_ADDR_MAGIC    0
#define EEPROM_ADDR_PRIVKEY  4      // 32 bytes
#define EEPROM_ADDR_PUBKEY   36     // 64 bytes
#define EEPROM_ADDR_PEER_PUB 100    // 64 bytes
#define EEPROM_ADDR_PEER_MAC 164    // 6 bytes
// 170-511: Reservado (342 bytes)

#define ECC_PRIV_KEY_SIZE  32
#define ECC_PUB_KEY_SIZE   64

// ============================================================
// Tabla de nodos (informacion local para comandos list/stats)
// ============================================================
#define NUM_NODES 5

typedef struct {
  uint8_t  id;
  uint8_t  mac[6];
  bool     known;
} EspNowNode;

// MACs de los nodos sensores (las mismas que v1.1)
EspNowNode nodes[NUM_NODES] = {
  { 0, {0x84, 0xF3, 0xEB, 0x7C, 0x99, 0x1E}, true },
  { 1, {0x80, 0x7D, 0x3A, 0x2B, 0xE3, 0x59}, true },
  { 2, {0x8C, 0xAA, 0xB5, 0x0F, 0x39, 0x7F}, true },
  { 3, {0x50, 0x02, 0x91, 0xDE, 0x0D, 0x7B}, true },
  { 4, {0x48, 0x3F, 0xDA, 0xAA, 0x8E, 0x6D}, true },
};

// ============================================================
// Variables de estado
// ============================================================

// Claves ECC
static uint8_t my_private_key[ECC_PRIV_KEY_SIZE];
static uint8_t my_public_key[ECC_PUB_KEY_SIZE];
static uint8_t peer_public_key[ECC_PUB_KEY_SIZE];
static bool    keys_initialized = false;
static bool    peer_key_known = false;

// Estado de comunicacion
typedef enum {
  BRIDGE_IDLE,
  BRIDGE_WAITING_RESPONSE  // Esperando respuesta cifrada de Puerta
} BridgeState;

static BridgeState bridge_state = BRIDGE_IDLE;
static unsigned long request_sent_time = 0;
static uint8_t last_polled_node_id = 0;
static bool    auto_poll_enabled = true;
static unsigned long last_poll_time = 0;

// Buffer de respuesta cifrada (desde ISR)
static volatile bool   g_crypt_response_received = false;
static volatile uint8_t g_crypt_sender_mac[6];
static volatile uint8_t g_crypt_buffer[sizeof(EspNowCryptedMessage)];

// Estadisticas
static uint32_t total_requests = 0;
static uint32_t total_responses = 0;
static uint32_t total_timeouts = 0;
static uint32_t msg_counter = 0;

// MAC propia
static uint8_t my_mac[6];

// ============================================================
// Funciones EEPROM
// ============================================================

void initEEPROM() {
  EEPROM.begin(EEPROM_SIZE);
}

bool hasKeysInEEPROM() {
  uint32_t magic;
  EEPROM.get(EEPROM_ADDR_MAGIC, magic);
  return (magic == EEPROM_MAGIC);
}

void saveKeysToEEPROM() {
  uint32_t magic = EEPROM_MAGIC;
  EEPROM.put(EEPROM_ADDR_MAGIC, magic);
  EEPROM.put(EEPROM_ADDR_PRIVKEY, my_private_key);
  EEPROM.put(EEPROM_ADDR_PUBKEY, my_public_key);
  EEPROM.commit();
  Serial.println("[EEPROM] Claves propias guardadas");
}

void loadKeysFromEEPROM() {
  EEPROM.get(EEPROM_ADDR_PRIVKEY, my_private_key);
  EEPROM.get(EEPROM_ADDR_PUBKEY, my_public_key);
}

void savePeerKeyToEEPROM() {
  EEPROM.put(EEPROM_ADDR_PEER_PUB, peer_public_key);
  EEPROM.put(EEPROM_ADDR_PEER_MAC, puerta_mac);
  EEPROM.commit();
  Serial.println("[EEPROM] Clave publica de Puerta de Enlace guardada");
}

bool loadPeerKeyFromEEPROM() {
  uint8_t test[ECC_PUB_KEY_SIZE];
  EEPROM.get(EEPROM_ADDR_PEER_PUB, test);
  
  bool all_zero = true;
  for (int i = 0; i < ECC_PUB_KEY_SIZE; i++) {
    if (test[i] != 0) { all_zero = false; break; }
  }
  if (all_zero) return false;
  
  EEPROM.get(EEPROM_ADDR_PEER_PUB, peer_public_key);
  EEPROM.get(EEPROM_ADDR_PEER_MAC, puerta_mac);
  peer_key_known = true;
  puerta_mac_known = true;
  return true;
}

// ============================================================
// Funciones criptograficas ECC
// ============================================================

static int ecc_rng(uint8_t *dest, unsigned size) {
  for (unsigned i = 0; i < size; i++) {
    dest[i] = (uint8_t)os_random();
  }
  return 1;
}

bool generateECCKeys() {
  const struct uECC_Curve_t *curve = uECC_secp256r1();
  int ret = uECC_make_key(my_public_key, my_private_key, curve);
  if (!ret) {
    Serial.println("[FATAL] Error generando claves ECC");
    return false;
  }
  Serial.println("[OK] Claves ECC generadas");
  return true;
}

void deriveXorKey(const uint8_t *shared_secret, uint32_t counter,
                  uint8_t *output, uint8_t len) {
  for (uint8_t i = 0; i < len; i++) {
    uint8_t idx = (uint8_t)((counter + i) % 32);
    uint8_t scramble = (uint8_t)((counter >> (8 * (i % 4))) & 0xFF);
    output[i] = shared_secret[idx] ^ scramble;
  }
}

void encryptXor(const uint8_t *plain, uint8_t *cipher, uint8_t len,
                const uint8_t *shared_secret, uint32_t counter) {
  uint8_t key_stream[CRYPTED_PAYLOAD_MAX];
  deriveXorKey(shared_secret, counter, key_stream, len);
  for (uint8_t i = 0; i < len; i++) {
    cipher[i] = plain[i] ^ key_stream[i];
  }
}

#define decryptXor(plain, cipher, len, secret, counter) \
  encryptXor(cipher, plain, len, secret, counter)

bool signMessage(const uint8_t *msg, uint8_t msg_len,
                 uint8_t *signature) {
  const struct uECC_Curve_t *curve = uECC_secp256r1();
  uint8_t hash[32];
  memset(hash, 0, 32);
  for (uint8_t i = 0; i < msg_len; i++) {
    hash[i % 32] ^= msg[i];
  }
  int ret = uECC_sign(my_private_key, hash, 32, signature, curve);
  return (ret == 1);
}

bool verifySignature(const uint8_t *msg, uint8_t msg_len,
                     const uint8_t *signature) {
  const struct uECC_Curve_t *curve = uECC_secp256r1();
  uint8_t hash[32];
  memset(hash, 0, 32);
  for (uint8_t i = 0; i < msg_len; i++) {
    hash[i % 32] ^= msg[i];
  }
  int ret = uECC_verify(peer_public_key, hash, 32, signature, curve);
  return (ret == 1);
}

// ============================================================
// Callbacks ISR ESP-NOW
// ============================================================

void onDataReceived(uint8_t *mac, uint8_t *data, uint8_t len) {
  if (len < 1) return;
  
  uint8_t msg_type = data[0];
  
  if (msg_type == MSG_TYPE_CRYPT_RESP) {
    // Respuesta cifrada de la Puerta de Enlace
    if (len >= 8) {  // Al menos cabecera minima
      memcpy((void*)g_crypt_sender_mac, mac, 6);
      size_t copy_len = (len < sizeof(EspNowCryptedMessage)) ? len : sizeof(EspNowCryptedMessage);
      memcpy((void*)g_crypt_buffer, data, copy_len);
      g_crypt_response_received = true;
    }
  }
  // Ignoramos mensajes tipo request/response planos
  // (los maneja la Puerta de Enlace, no nosotros)
}

void onDataSent(uint8_t *mac, uint8_t status) {
  (void)mac;
  // status != 0 indica fallo
  if (status != 0) {
    // No hacer mucho en ISR
  }
}

// ============================================================
// Enviar comando cifrado a la Puerta de Enlace
// ============================================================
bool sendEncryptedCommand(uint8_t node_id, const char *command) {
  if (!keys_initialized || !peer_key_known) {
    Serial.println("[ERROR] No hay claves ECC. Genere claves primero.");
    return false;
  }
  
  if (!puerta_mac_known) {
    Serial.println("[ERROR] No se conoce la MAC de la Puerta de Enlace.");
    Serial.println("[INFO] Espere a que la Puerta de Enlace responda o use broadcast.");
    return false;
  }
  
  // Calcular secreto compartido ECDH
  const struct uECC_Curve_t *curve = uECC_secp256r1();
  uint8_t shared_secret[ECC_PRIV_KEY_SIZE];
  int ret = uECC_shared_secret(my_private_key, peer_public_key, shared_secret, curve);
  if (!ret) {
    Serial.println("[ERROR] ECDH fallo");
    return false;
  }
  
  // Preparar mensaje cifrado
  EspNowCryptedMessage msg;
  memset(&msg, 0, sizeof(msg));
  msg.msg_type = MSG_TYPE_CRYPT_REQ;
  msg.node_id = node_id;
  msg.counter = msg_counter++;
  
  uint8_t cmd_len = strlen(command);
  if (cmd_len > CRYPTED_PAYLOAD_MAX) cmd_len = CRYPTED_PAYLOAD_MAX;
  msg.payload_len = cmd_len;
  
  // Cifrar payload
  encryptXor((uint8_t*)command, msg.payload, cmd_len, shared_secret, msg.counter);
  
  // Firmar: msg_type + node_id + counter + plain_command
  uint8_t data_to_sign[4 + CRYPTED_PAYLOAD_MAX + 2];
  uint16_t idx = 0;
  data_to_sign[idx++] = msg.msg_type;
  data_to_sign[idx++] = msg.node_id;
  memcpy(data_to_sign + idx, &msg.counter, 4); idx += 4;
  memcpy(data_to_sign + idx, command, cmd_len); idx += cmd_len;
  data_to_sign[idx++] = msg.payload_len;
  
  if (!signMessage(data_to_sign, idx, msg.signature)) {
    Serial.println("[ERROR] Firma ECDSA fallo");
    return false;
  }
  
  // Enviar a Puerta de Enlace
  // Asegurar peer registrado
  if (!esp_now_is_peer_exist(puerta_mac)) {
    esp_now_add_peer(puerta_mac, ESP_NOW_ROLE_COMBO, 1, NULL, 0);
  }
  
  ret = esp_now_send(puerta_mac, (uint8_t*)&msg, sizeof(EspNowCryptedMessage));
  
  if (ret == 0) {
    Serial.printf("[ENVIO CIFRADO] Comando -> Puerta de Enlace (nodo=%d, cmd=%s)\n",
                  node_id, command);
    total_requests++;
    bridge_state = BRIDGE_WAITING_RESPONSE;
    request_sent_time = millis();
    return true;
  } else {
    Serial.printf("[ERROR] Fallo envio cifrado (codigo=%d)\n", ret);
    bridge_state = BRIDGE_IDLE;
    return false;
  }
}

// ============================================================
// Procesar respuesta cifrada de la Puerta de Enlace
// ============================================================
void processEncryptedResponse() {
  if (!g_crypt_response_received) return;
  g_crypt_response_received = false;
  
  EspNowCryptedMessage resp;
  memcpy(&resp, (const void*)g_crypt_buffer, sizeof(EspNowCryptedMessage));
  
  // Aprender MAC de Puerta si no la conocemos
  if (!puerta_mac_known) {
    memcpy(puerta_mac, (const void*)g_crypt_sender_mac, 6);
    puerta_mac_known = true;
    Serial.printf("[APRENDIZAJE] MAC Puerta de Enlace: %02X:%02X:%02X:%02X:%02X:%02X\n",
                  puerta_mac[0], puerta_mac[1], puerta_mac[2],
                  puerta_mac[3], puerta_mac[4], puerta_mac[5]);
    
    // Agregar como peer
    if (!esp_now_is_peer_exist(puerta_mac)) {
      esp_now_add_peer(puerta_mac, ESP_NOW_ROLE_COMBO, 1, NULL, 0);
    }
  }
  
  if (!peer_key_known) {
    // Intentar cargar de EEPROM
    if (!loadPeerKeyFromEEPROM()) {
      Serial.println("[ERROR] No hay clave publica de Puerta de Enlace");
      Serial.println("[INFO] Use 'learnkey' para aprenderla del primer mensaje");
      bridge_state = BRIDGE_IDLE;
      return;
    }
  }
  
  // Verificar firma
  uint8_t data_to_verify[4 + CRYPTED_PAYLOAD_MAX + 2];
  uint16_t idx = 0;
  data_to_verify[idx++] = resp.msg_type;
  data_to_verify[idx++] = resp.node_id;
  memcpy(data_to_verify + idx, &resp.counter, 4); idx += 4;
  memcpy(data_to_verify + idx, resp.payload, resp.payload_len); idx += resp.payload_len;
  data_to_verify[idx++] = resp.payload_len;
  
  if (!verifySignature(data_to_verify, idx, resp.signature)) {
    Serial.println("[SEGURIDAD] Firma ECDSA INVALIDA - respuesta rechazada");
    bridge_state = BRIDGE_IDLE;
    return;
  }
  Serial.println("[SEGURIDAD] Firma ECDSA VALIDA - Puerta autenticada");
  
  // Descifrar payload
  const struct uECC_Curve_t *curve = uECC_secp256r1();
  uint8_t shared_secret[ECC_PRIV_KEY_SIZE];
  int ret = uECC_shared_secret(my_private_key, peer_public_key, shared_secret, curve);
  if (!ret) {
    Serial.println("[ERROR] ECDH fallo en descifrado");
    bridge_state = BRIDGE_IDLE;
    return;
  }
  
  uint8_t plaintext[CRYPTED_PAYLOAD_MAX];
  decryptXor(plaintext, resp.payload, resp.payload_len, shared_secret, resp.counter);
  plaintext[resp.payload_len] = '\0';
  
  total_responses++;
  bridge_state = BRIDGE_IDLE;
  
  // Mostrar resultado
  uint32_t rtt = millis() - request_sent_time;
  Serial.println("----------------------------------------");
  Serial.printf("RESPUESTA DESCIFRADA - Nodo #%d\n", resp.node_id);
  Serial.printf("  %s\n", (char*)plaintext);
  Serial.printf("  RTT (via Puerta): %u ms\n", rtt);
  Serial.println("----------------------------------------");
  
  // Parpadeo de confirmacion
  digitalWrite(LED_PIN, LED_ON);
  delay(20);
  digitalWrite(LED_PIN, LED_OFF);
}

// ============================================================
// Comandos seriales
// ============================================================

void printHelp() {
  Serial.println();
  Serial.println("=== COMANDOS DISPONIBLES (netEspNow v1.2) ===");
  Serial.println("  ping <id>    : Preguntar a un nodo (0-4) via Puerta de Enlace");
  Serial.println("  ping all     : Preguntar a todos los nodos");
  Serial.println("  list         : Mostrar tabla de nodos");
  Serial.println("  stats        : Mostrar estadisticas");
  Serial.println("  auto on      : Activar interrogacion automatica");
  Serial.println("  auto off     : Desactivar interrogacion automatica");
  Serial.println("  keygen       : Generar nuevo par de claves ECC");
  Serial.println("  learnkey     : Aprender clave publica de Puerta desde EEPROM");
  Serial.println("  showkey      : Mostrar clave publica de este Bridge");
  Serial.println("  help / ?     : Mostrar esta ayuda");
  Serial.println("=============================================");
  Serial.println();
}

void printList() {
  Serial.println("=== TABLA DE NODOS ===");
  for (int i = 0; i < NUM_NODES; i++) {
    Serial.printf("  Nodo %d: %s [%02X:%02X:%02X:%02X:%02X:%02X]\n",
                  nodes[i].id,
                  nodes[i].known ? "CONOCIDO" : "DESCONOCIDO",
                  nodes[i].mac[0], nodes[i].mac[1], nodes[i].mac[2],
                  nodes[i].mac[3], nodes[i].mac[4], nodes[i].mac[5]);
  }
  Serial.printf("  Puerta de Enlace: %s [%02X:%02X:%02X:%02X:%02X:%02X]\n",
                puerta_mac_known ? "CONOCIDA" : "DESCONOCIDA",
                puerta_mac[0], puerta_mac[1], puerta_mac[2],
                puerta_mac[3], puerta_mac[4], puerta_mac[5]);
  Serial.printf("  Cifrado ECC: %s | Clave Puerta: %s\n",
                keys_initialized ? "ACTIVO" : "INACTIVO",
                peer_key_known ? "CONOCIDA" : "DESCONOCIDA");
  Serial.println("========================");
}

void printStats() {
  Serial.println("=== ESTADISTICAS ===");
  Serial.printf("  Peticiones enviadas:  %u\n", total_requests);
  Serial.printf("  Respuestas OK:        %u\n", total_responses);
  Serial.printf("  Timeouts:             %u\n", total_timeouts);
  if (total_requests > 0) {
    Serial.printf("  Tasa de exito:        %u%%\n",
                  (total_responses * 100) / total_requests);
  }
  Serial.printf("  Claves ECC:           %s\n",
                keys_initialized ? "GENERADAS" : "NO GENERADAS");
  Serial.printf("  Clave Puerta:         %s\n",
                peer_key_known ? "CONOCIDA" : "DESCONOCIDA");
  Serial.printf("  MAC Puerta:           %s\n",
                puerta_mac_known ? "CONOCIDA" : "DESCONOCIDA");
  Serial.println("====================");
}

void showPublicKey() {
  if (!keys_initialized) {
    Serial.println("[ERROR] No hay claves. Use 'keygen' primero.");
    return;
  }
  Serial.println("=== CLAVE PUBLICA DEL BRIDGE ===");
  Serial.print("  Hex: ");
  for (int i = 0; i < ECC_PUB_KEY_SIZE; i++) {
    if (i > 0) Serial.print(" ");
    Serial.printf("%02X", my_public_key[i]);
  }
  Serial.println();
  Serial.println("=================================");
}

void learnPeerKey() {
  if (loadPeerKeyFromEEPROM()) {
    Serial.println("[OK] Clave publica de Puerta de Enlace cargada de EEPROM");
    Serial.printf("  MAC Puerta: %02X:%02X:%02X:%02X:%02X:%02X\n",
                  puerta_mac[0], puerta_mac[1], puerta_mac[2],
                  puerta_mac[3], puerta_mac[4], puerta_mac[5]);
  } else {
    Serial.println("[INFO] No hay clave de Puerta en EEPROM.");
    Serial.println("[INFO] Conecte la Puerta y espere a que envie su primer mensaje.");
  }
}

void processSerialCommand(const char *cmd) {
  while (*cmd == ' ') cmd++;
  
  if (strcmp(cmd, "help") == 0 || strcmp(cmd, "?") == 0) {
    printHelp();
    return;
  }
  
  if (strcmp(cmd, "list") == 0) {
    printList();
    return;
  }
  
  if (strcmp(cmd, "stats") == 0) {
    printStats();
    return;
  }
  
  if (strcmp(cmd, "auto on") == 0) {
    auto_poll_enabled = true;
    last_poll_time = millis() - POLL_INTERVAL_MS;
    Serial.println("[OK] Interrogacion automatica ACTIVADA");
    return;
  }
  
  if (strcmp(cmd, "auto off") == 0) {
    auto_poll_enabled = false;
    Serial.println("[OK] Interrogacion automatica DESACTIVADA");
    return;
  }
  
  if (strcmp(cmd, "keygen") == 0) {
    Serial.println("[INFO] Generando nuevo par de claves ECC...");
    if (generateECCKeys()) {
      saveKeysToEEPROM();
      Serial.println("[OK] Nuevas claves ECC generadas y guardadas");
    }
    return;
  }
  
  if (strcmp(cmd, "learnkey") == 0) {
    learnPeerKey();
    return;
  }
  
  if (strcmp(cmd, "showkey") == 0) {
    showPublicKey();
    return;
  }
  
  // Comando: ping <id> o ping all
  if (strncmp(cmd, "ping", 4) == 0) {
    cmd += 4;
    while (*cmd == ' ') cmd++;
    
    if (bridge_state != BRIDGE_IDLE) {
      Serial.println("[WARN] Esperando respuesta de la Puerta de Enlace...");
      return;
    }
    
    if (strcmp(cmd, "all") == 0) {
      Serial.println("[CMD] Preguntando a TODOS los nodos...");
      auto_poll_enabled = true;
      last_polled_node_id = 0;
      last_poll_time = millis() - POLL_INTERVAL_MS;
      return;
    }
    
    int id = atoi(cmd);
    if (id >= 0 && id < NUM_NODES) {
      Serial.printf("[CMD] Preguntando a Nodo %d via Puerta de Enlace...\n", id);
      last_polled_node_id = id;
      
      char cmd_buf[16];
      snprintf(cmd_buf, sizeof(cmd_buf), "ping %d", id);
      
      if (!sendEncryptedCommand(id, cmd_buf)) {
        Serial.println("[ERROR] No se pudo enviar comando cifrado");
      }
      return;
    }
    
    Serial.printf("[ERROR] ID invalido: %s (use 0-%d)\n", cmd, NUM_NODES - 1);
    return;
  }
  
  Serial.printf("[ERROR] Comando desconocido: '%s' (escriba 'help')\n", cmd);
}

// ============================================================
// Lectura de comandos seriales
// ============================================================
#define SERIAL_BUF_SIZE 32
static char serial_buffer[SERIAL_BUF_SIZE];
static uint8_t serial_idx = 0;

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
// Setup
// ============================================================
void setup() {
  Serial.begin(SERIAL_BAUD);
  Serial.println();
  Serial.println("============================================");
  Serial.println("  ESP8266 BRIDGE - Red ESP-NOW Cifrada v1.2");
  Serial.println("  Bridge -> Puerta de Enlace (ECC)");
  Serial.println("============================================");
  Serial.println();
  
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LED_OFF);
  
  // Inicializar EEPROM
  initEEPROM();
  
  // Cargar o generar claves ECC
  if (hasKeysInEEPROM()) {
    loadKeysFromEEPROM();
    keys_initialized = true;
    Serial.println("[EEPROM] Claves ECC cargadas");
    
    if (loadPeerKeyFromEEPROM()) {
      Serial.println("[EEPROM] Clave publica de Puerta de Enlace cargada");
      Serial.printf("  MAC Puerta: %02X:%02X:%02X:%02X:%02X:%02X\n",
                    puerta_mac[0], puerta_mac[1], puerta_mac[2],
                    puerta_mac[3], puerta_mac[4], puerta_mac[5]);
    }
  } else {
    Serial.println("[INFO] No hay claves. Use el comando 'keygen' para generarlas.");
  }
  
  // Inicializar WiFi
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);
  
  Serial.print("MAC del Bridge: ");
  Serial.println(WiFi.macAddress());
  WiFi.macAddress(my_mac);
  
  // Inicializar ESP-NOW
  int ret = esp_now_init();
  if (ret != 0) {
    Serial.println("[FATAL] Error al inicializar ESP-NOW");
    delay(5000);
    ESP.restart();
  }
  
  esp_now_set_self_role(ESP_NOW_ROLE_COMBO);
  esp_now_register_recv_cb(onDataReceived);
  esp_now_register_send_cb(onDataSent);
  
  // Agregar peer broadcast
  esp_now_add_peer((uint8_t*)ESP_NOW_BROADCAST_MAC, ESP_NOW_ROLE_COMBO, 1, NULL, 0);
  
  Serial.println("[OK] ESP-NOW inicializado");
  Serial.println("[OK] Bridge listo. Use 'help' para comandos.");
  Serial.println();
  Serial.printf("Auto-polling: %s\n", auto_poll_enabled ? "ACTIVADO" : "DESACTIVADO");
  Serial.printf("Intervalo: %d ms\n", POLL_INTERVAL_MS);
  Serial.println("----------------------------------------");
  Serial.println();
  
  // Parpadeo de inicio
  for (int i = 0; i < 3; i++) {
    digitalWrite(LED_PIN, LED_ON);
    delay(100);
    digitalWrite(LED_PIN, LED_OFF);
    delay(100);
  }
  
  last_poll_time = millis();
}

// ============================================================
// Loop principal
// ============================================================
void loop() {
  unsigned long now = millis();
  
  // 1. Procesar respuesta cifrada de Puerta de Enlace
  processEncryptedResponse();
  
  // 2. Leer comandos serial del usuario
  readSerialCommands();
  
  // 3. Verificar timeout de respuesta
  if (bridge_state == BRIDGE_WAITING_RESPONSE) {
    if (now - request_sent_time > RESPONSE_TIMEOUT_MS) {
      bridge_state = BRIDGE_IDLE;
      total_timeouts++;
      Serial.printf("[TIMEOUT] Puerta de Enlace - Sin respuesta (%u ms)\n",
                    RESPONSE_TIMEOUT_MS);
      Serial.println("[INFO] Verifique que la Puerta de Enlace este encendida y dentro del alcance");
    }
  }
  
  // 4. Auto-polling (si activo)
  if (auto_poll_enabled && bridge_state == BRIDGE_IDLE && 
      (now - last_poll_time >= POLL_INTERVAL_MS)) {
    last_poll_time = now;
    
    last_polled_node_id = last_polled_node_id % NUM_NODES;
    
    char cmd_buf[16];
    snprintf(cmd_buf, sizeof(cmd_buf), "ping %d", last_polled_node_id);
    
    Serial.printf("[AUTO] Preguntando a Nodo %d...\n", last_polled_node_id);
    sendEncryptedCommand(last_polled_node_id, cmd_buf);
    
    last_polled_node_id = (last_polled_node_id + 1) % NUM_NODES;
  }
  
  // 5. Stats periodicos (cada 30s si auto-poll activo)
  static unsigned long last_stats_time = 0;
  if (auto_poll_enabled && (now - last_stats_time >= 30000)) {
    last_stats_time = now;
    printStats();
  }
  
  delay(1);
}
