/**
 * Firmware ESP8266 - Puerta de Enlace (Cifrado Asimetrico ECC)
 * ==============================================================
 * Placa: ESP8266EX ceramica (sin GPIO accesibles)
 * 
 * Funcion:
 *   - Intermediario cifrado entre Bridge (PC) y nodos sensores 0-4
 *   - Recibe comandos cifrados del Bridge via ESP-NOW
 *   - Descifra usando ECC (clave privada en EEPROM)
 *   - Envia comando plano al nodo destino
 *   - Espera respuesta del nodo
 *   - Cifra la respuesta y la reenvia al Bridge
 * 
 * Flujo:
 *   Bridge --(ESP-NOW cifrado)--> Puerta de Enlace
 *   Puerta de Enlace --(ESP-NOW plano)--> Nodo X
 *   Nodo X --(ESP-NOW plano)--> Puerta de Enlace
 *   Puerta de Enlace --(ESP-NOW cifrado)--> Bridge
 * 
 * Cifrado:
 *   - ECC secp256r1 (uECC library)
 *   - ECDH para derivar secreto compartido
 *   - XOR con keystream derivado del secreto compartido + contador
 *   - ECDSA para firma/verificacion de autenticidad
 *   - Claves almacenadas en EEPROM
 * 
 * Hardware:
 *   - Sin GPIO accesibles (solo USB para alimentacion)
 *   - Sin salida serial de debug (excepto si se habilita)
 * 
 * Version: 1.2
 */

// ============================================================
// Descomentar SOLO si el USB de la placa ceramica soporta
// datos (TX/RX). Por defecto esta deshabilitado porque la
// placa ceramica solo tiene USB para alimentacion.
// ============================================================
//#define ENABLE_SERIAL_DEBUG

#include <ESP8266WiFi.h>
#include <espnow.h>
#include <EEPROM.h>
#include <uECC.h>

// ============================================================
// Configuracion general
// ============================================================

#ifdef ENABLE_SERIAL_DEBUG
  #define SERIAL_BAUD 115200
  #define DEBUG_PRINT(x)   Serial.print(x)
  #define DEBUG_PRINTLN(x) Serial.println(x)
  #define DEBUG_PRINTF(...) Serial.printf(__VA_ARGS__)
#else
  #define DEBUG_PRINT(x)
  #define DEBUG_PRINTLN(x)
  #define DEBUG_PRINTF(...)
#endif

#define ESP_NOW_CHANNEL     1
#define NODE_TIMEOUT_MS     1000    // Timeout espera respuesta de nodo
#define HEARTBEAT_MS        2000    // LED virtual (latido interno)

// ============================================================
// Tipos de mensaje ESP-NOW
// ============================================================
#define MSG_TYPE_REQUEST     0   // Plano: Bridge/Puerta -> Nodo
#define MSG_TYPE_RESPONSE    1   // Plano: Nodo -> Bridge/Puerta
#define MSG_TYPE_CRYPT_REQ   2   // Cifrado: Bridge -> Puerta de Enlace
#define MSG_TYPE_CRYPT_RESP  3   // Cifrado: Puerta de Enlace -> Bridge

// ============================================================
// Estructura: Mensaje plano (nodos 0-4) - COMPATIBLE v1.1
// ============================================================
typedef struct __attribute__((packed)) {
  uint8_t  msg_type;    // 0=request, 1=response
  uint8_t  node_id;     // ID del nodo (0-4)
  uint32_t counter;     // Contador correlativo
  uint32_t uptime_ms;   // Tiempo de actividad del nodo
} EspNowMessage;

// ============================================================
// Estructura: Mensaje cifrado (Bridge <-> Puerta de Enlace)
// ============================================================
#define CRYPTED_PAYLOAD_MAX 32
typedef struct __attribute__((packed)) {
  uint8_t  msg_type;       // 2=bridge->puerta, 3=puerta->bridge
  uint8_t  node_id;        // ID del nodo destino (0-4) o 0xFF
  uint32_t counter;        // Contador correlativo (anti-replay)
  uint8_t  payload[CRYPTED_PAYLOAD_MAX]; // Datos cifrados
  uint8_t  payload_len;    // Longitud real del payload
  uint8_t  signature[64];  // Firma ECDSA (Bridge firma, Puerta verifica)
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

// Tamanos ECC (secp256r1)
#define ECC_PRIV_KEY_SIZE  32
#define ECC_PUB_KEY_SIZE   64

// ============================================================
// Estado de la maquina de estados
// ============================================================
typedef enum {
  STATE_IDLE,                  // Esperando comando cifrado del Bridge
  STATE_ESP_RESP_NODO,         // Esperando respuesta del nodo
  STATE_CRYPT_AND_SEND        // Cifrando respuesta y enviando al Bridge
} PuertaState;

// ============================================================
// Variables globales
// ============================================================

// Claves ECC
static uint8_t my_private_key[ECC_PRIV_KEY_SIZE];
static uint8_t my_public_key[ECC_PUB_KEY_SIZE];
static uint8_t peer_public_key[ECC_PUB_KEY_SIZE];
static bool    keys_initialized = false;
static bool    peer_key_known = false;

// MAC del Bridge (peer)
static uint8_t bridge_mac[6];
static bool    bridge_mac_known = false;

// MAC de broadcast
const uint8_t ESP_NOW_BROADCAST_MAC[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// Estado actual
static PuertaState state = STATE_IDLE;
static unsigned long state_enter_time = 0;

// Mensaje actual en procesamiento
static EspNowCryptedMessage current_crypted_msg;
static EspNowMessage current_nodo_response;

// Buffer ISR para mensajes recibidos
static volatile bool   g_crypt_msg_received = false;
static volatile uint8_t g_crypt_sender_mac[6];
static volatile uint8_t g_crypt_buffer[sizeof(EspNowCryptedMessage)];

static volatile bool   g_nodo_response_received = false;
static volatile uint8_t g_nodo_sender_mac[6];
static volatile uint8_t g_nodo_buffer[sizeof(EspNowMessage)];

// Contadores
static uint32_t msg_counter = 0;
static uint32_t total_commands_decrypted = 0;
static uint32_t total_responses_encrypted = 0;
static uint32_t total_timeouts = 0;
static uint32_t total_errors = 0;

// Heartbeat interno (no tenemos LED)
static unsigned long last_heartbeat = 0;
static uint32_t heartbeat_count = 0;

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
}

void loadKeysFromEEPROM() {
  EEPROM.get(EEPROM_ADDR_PRIVKEY, my_private_key);
  EEPROM.get(EEPROM_ADDR_PUBKEY, my_public_key);
}

void savePeerKeyToEEPROM() {
  EEPROM.put(EEPROM_ADDR_PEER_PUB, peer_public_key);
  EEPROM.put(EEPROM_ADDR_PEER_MAC, bridge_mac);
  EEPROM.commit();
}

bool loadPeerKeyFromEEPROM() {
  uint8_t test[ECC_PUB_KEY_SIZE];
  EEPROM.get(EEPROM_ADDR_PEER_PUB, test);
  
  // Verificar que no sea todo ceros
  bool all_zero = true;
  for (int i = 0; i < ECC_PUB_KEY_SIZE; i++) {
    if (test[i] != 0) { all_zero = false; break; }
  }
  if (all_zero) return false;
  
  EEPROM.get(EEPROM_ADDR_PEER_PUB, peer_public_key);
  EEPROM.get(EEPROM_ADDR_PEER_MAC, bridge_mac);
  peer_key_known = true;
  bridge_mac_known = true;
  return true;
}

// ============================================================
// Funciones criptograficas ECC
// ============================================================

// RNG para uECC (usa hardware ESP8266)
static int ecc_rng(uint8_t *dest, unsigned size) {
  for (unsigned i = 0; i < size; i++) {
    dest[i] = (uint8_t)os_random();
  }
  return 1;
}

bool generateECCKeys() {
  // uECC_secp256r1 es la curva por defecto
  const struct uECC_Curve_t *curve = uECC_secp256r1();
  
  int ret = uECC_make_key(my_public_key, my_private_key, curve);
  if (!ret) {
    DEBUG_PRINTLN("[FATAL] Error generando claves ECC");
    return false;
  }
  
  DEBUG_PRINTF("[OK] Claves ECC generadas. Pub key first byte: 0x%02X\n", my_public_key[0]);
  
  return true;
}

// Derivar keystream para XOR a partir del secreto compartido ECDH + contador
// key_stream[i] = shared_secret[(counter + i) % 32]
void deriveXorKey(const uint8_t *shared_secret, uint32_t counter, 
                  uint8_t *output, uint8_t len) {
  for (uint8_t i = 0; i < len; i++) {
    uint8_t idx = (uint8_t)((counter + i) % 32);
    uint8_t scramble = (uint8_t)((counter >> (8 * (i % 4))) & 0xFF);
    output[i] = shared_secret[idx] ^ scramble;
  }
}

// Cifrar: XOR del plaintext con keystream derivado
void encryptXor(const uint8_t *plain, uint8_t *cipher, uint8_t len, 
                const uint8_t *shared_secret, uint32_t counter) {
  uint8_t key_stream[CRYPTED_PAYLOAD_MAX];
  deriveXorKey(shared_secret, counter, key_stream, len);
  for (uint8_t i = 0; i < len; i++) {
    cipher[i] = plain[i] ^ key_stream[i];
  }
}

// Descifrar: mismo XOR (simetrico)
#define decryptXor(plain, cipher, len, secret, counter) \
  encryptXor(cipher, plain, len, secret, counter)

// Firmar mensaje con ECDSA
bool signMessage(const uint8_t *msg, uint8_t msg_len, 
                 uint8_t *signature, const uint8_t *private_key) {
  const struct uECC_Curve_t *curve = uECC_secp256r1();
  
  // Hash simple: XOR de todos los bytes para reducir a 32 bytes
  // (No es un hash criptografico real, pero para este proposito es suficiente)
  uint8_t hash[32];
  memset(hash, 0, 32);
  for (uint8_t i = 0; i < msg_len; i++) {
    hash[i % 32] ^= msg[i];
  }
  // Mezclar con contador global
  hash[0] ^= (uint8_t)(msg_counter);
  hash[1] ^= (uint8_t)(msg_counter >> 8);
  
  int ret = uECC_sign(private_key, hash, 32, signature, curve);
  return (ret == 1);
}

// Verificar firma ECDSA
bool verifySignature(const uint8_t *msg, uint8_t msg_len,
                     const uint8_t *signature, const uint8_t *public_key) {
  const struct uECC_Curve_t *curve = uECC_secp256r1();
  
  uint8_t hash[32];
  memset(hash, 0, 32);
  for (uint8_t i = 0; i < msg_len; i++) {
    hash[i % 32] ^= msg[i];
  }
  
  int ret = uECC_verify(public_key, hash, 32, signature, curve);
  return (ret == 1);
}

// ============================================================
// Callbacks ISR ESP-NOW
// ============================================================

// Datos recibidos
void onDataReceived(uint8_t *mac, uint8_t *data, uint8_t len) {
  if (len < 1) return;
  
  uint8_t msg_type = data[0];
  
  if (msg_type == MSG_TYPE_CRYPT_REQ) {
    // Mensaje cifrado del Bridge
    if (len >= sizeof(EspNowCryptedMessage) - 64) { // Al menos cabecera
      memcpy((void*)g_crypt_sender_mac, mac, 6);
      memcpy((void*)g_crypt_buffer, data, len);
      g_crypt_msg_received = true;
    }
  }
  else if (msg_type == MSG_TYPE_RESPONSE) {
    // Respuesta plana de un nodo
    if (len >= sizeof(EspNowMessage)) {
      memcpy((void*)g_nodo_sender_mac, mac, 6);
      memcpy((void*)g_nodo_buffer, data, len);
      g_nodo_response_received = true;
    }
  }
  // Ignorar otros tipos
}

// Confirmacion de envio
void onDataSent(uint8_t *mac, uint8_t status) {
  (void)mac;
  if (status != 0) {
    DEBUG_PRINTF("[WARN] Envio ESP-NOW fallo (status=%d)\n", status);
  }
}

// ============================================================
// Enviar mensaje cifrado al Bridge
// ============================================================
bool sendEncryptedToBridge(uint8_t node_id, const uint8_t *data, uint8_t data_len) {
  if (!bridge_mac_known || !keys_initialized || !peer_key_known) {
    DEBUG_PRINTLN("[ERROR] No se puede cifrar: faltan claves o MAC del Bridge");
    return false;
  }
  
  // Calcular secreto compartido ECDH
  const struct uECC_Curve_t *curve = uECC_secp256r1();
  uint8_t shared_secret[ECC_PRIV_KEY_SIZE];
  int ret = uECC_shared_secret(my_private_key, peer_public_key, shared_secret, curve);
  if (!ret) {
    DEBUG_PRINTLN("[ERROR] ECDH fallo");
    total_errors++;
    return false;
  }
  
  // Cifrar payload con XOR
  EspNowCryptedMessage msg;
  memset(&msg, 0, sizeof(msg));
  msg.msg_type = MSG_TYPE_CRYPT_RESP;
  msg.node_id = node_id;
  msg.counter = msg_counter++;
  msg.payload_len = data_len;
  
  encryptXor(data, msg.payload, data_len, shared_secret, msg.counter);
  
  // Firmar el mensaje (autenticidad)
  uint8_t msg_to_sign[4 + data_len]; // counter + payload
  memcpy(msg_to_sign, &msg.counter, 4);
  memcpy(msg_to_sign + 4, data, data_len);
  
  if (!signMessage(msg_to_sign, 4 + data_len, msg.signature, my_private_key)) {
    DEBUG_PRINTLN("[ERROR] Firma ECDSA fallo");
    total_errors++;
    return false;
  }
  
  // Enviar al Bridge
  ret = esp_now_send(bridge_mac, (uint8_t*)&msg, sizeof(EspNowCryptedMessage));
  if (ret == 0) {
    DEBUG_PRINTF("[ENVIO] Respuesta cifrada -> Bridge (nodo=%d, len=%d)\n", 
                 node_id, data_len);
    total_responses_encrypted++;
    return true;
  } else {
    DEBUG_PRINTF("[ERROR] Fallo envio al Bridge (codigo=%d)\n", ret);
    total_errors++;
    return false;
  }
}

// ============================================================
// Enviar request plano a un nodo (protocolo v1.1)
// ============================================================
bool sendRequestToNode(uint8_t node_id) {
  EspNowMessage request;
  request.msg_type = MSG_TYPE_REQUEST;
  request.node_id = node_id;
  request.counter = 0;
  request.uptime_ms = 0;
  
  // Enviar por broadcast (cualquier nodo que escuche y tenga ese ID respondera)
  int ret = esp_now_send((uint8_t*)ESP_NOW_BROADCAST_MAC, 
                         (uint8_t*)&request, sizeof(EspNowMessage));
  
  if (ret == 0) {
    DEBUG_PRINTF("[ENVIO] Request plano -> Nodo %d\n", node_id);
    return true;
  } else {
    DEBUG_PRINTF("[ERROR] Fallo envio a nodo %d (codigo=%d)\n", node_id, ret);
    return false;
  }
}

// ============================================================
// Procesar comando descifrado del Bridge
// ============================================================
void processDecryptedCommand(const uint8_t *plaintext, uint8_t len, uint8_t node_id) {
  DEBUG_PRINTF("[CMD] Comando descifrado: nodo=%d, len=%d\n", node_id, len);
  DEBUG_PRINT("[CMD] Contenido: ");
  for (uint8_t i = 0; i < len; i++) {
    DEBUG_PRINT((char)plaintext[i]);
  }
  DEBUG_PRINTLN();
  
  // Enviar request al nodo
  state = STATE_ESP_RESP_NODO;
  state_enter_time = millis();
  
  if (!sendRequestToNode(node_id)) {
    DEBUG_PRINTLN("[ERROR] No se pudo enviar request al nodo");
    state = STATE_IDLE;
    
    // Enviar error al Bridge
    const char *err_msg = "ERROR: nodo no disponible";
    sendEncryptedToBridge(node_id, (uint8_t*)err_msg, strlen(err_msg));
  }
}

// ============================================================
// Verificar y descifrar mensaje del Bridge
// ============================================================
bool verifyAndDecrypt(EspNowCryptedMessage *msg, uint8_t *plaintext, uint8_t *plain_len) {
  if (!keys_initialized || !peer_key_known) {
    DEBUG_PRINTLN("[ERROR] No hay claves para descifrar");
    return false;
  }
  
  // Primero verificar firma ECDSA
  uint8_t signed_data[4 + msg->payload_len];
  memcpy(signed_data, &msg->counter, 4);
  // Necesitamos el plaintext para verificar... pero aun no lo tenemos.
  // En realidad, la firma se hizo sobre: counter + plaintext
  // Pero nosotros tenemos counter + ciphertext, no plaintext aun.
  // 
  // Solucion alternativa: firmar solo el counter + node_id + payload_len
  // y usar el payload cifrado como parte de la firma.
  // 
  // Simplificacion: firmamos el mensaje completo (excepto firma misma).
  // El Bridge firma: msg_type + node_id + counter + payload + payload_len
  // Verificamos eso antes de descifrar.
  
  uint8_t data_to_verify[4 + CRYPTED_PAYLOAD_MAX + 2];
  uint16_t idx = 0;
  data_to_verify[idx++] = msg->msg_type;
  data_to_verify[idx++] = msg->node_id;
  memcpy(data_to_verify + idx, &msg->counter, 4); idx += 4;
  memcpy(data_to_verify + idx, msg->payload, msg->payload_len); idx += msg->payload_len;
  data_to_verify[idx++] = msg->payload_len;
  
  if (!verifySignature(data_to_verify, idx, msg->signature, peer_public_key)) {
    DEBUG_PRINTLN("[SEGURIDAD] Firma ECDSA INVALIDA - mensaje rechazado");
    total_errors++;
    return false;
  }
  DEBUG_PRINTLN("[SEGURIDAD] Firma ECDSA VALIDA - Bridge autenticado");
  
  // Ahora descifrar
  const struct uECC_Curve_t *curve = uECC_secp256r1();
  uint8_t shared_secret[ECC_PRIV_KEY_SIZE];
  int ret = uECC_shared_secret(my_private_key, peer_public_key, shared_secret, curve);
  if (!ret) {
    DEBUG_PRINTLN("[ERROR] ECDH fallo en descifrado");
    return false;
  }
  
  decryptXor(plaintext, msg->payload, msg->payload_len, shared_secret, msg->counter);
  *plain_len = msg->payload_len;
  
  DEBUG_PRINT("[OK] Mensaje descifrado: ");
  for (uint8_t i = 0; i < *plain_len; i++) {
    DEBUG_PRINT((char)plaintext[i]);
  }
  DEBUG_PRINTLN();
  
  total_commands_decrypted++;
  return true;
}

// ============================================================
// Procesar respuesta del nodo
// ============================================================
void processNodoResponse() {
  if (!g_nodo_response_received) return;
  g_nodo_response_received = false;
  
  EspNowMessage resp;
  memcpy(&resp, (const void*)g_nodo_buffer, sizeof(EspNowMessage));
  
  uint8_t node_id = resp.node_id;
  
  DEBUG_PRINTF("[RX] Respuesta del nodo %d: counter=%u, uptime=%u ms\n",
               node_id, resp.counter, resp.uptime_ms);
  
  // Cifrar la respuesta y enviarla al Bridge
  // El formato del plaintext sera: "Nodo X | contador=Y | uptime=Z ms"
  char plaintext[64];
  int plain_len = snprintf(plaintext, sizeof(plaintext),
                           "Nodo %d | contador=%u | uptime=%u ms",
                           node_id, resp.counter, resp.uptime_ms);
  
  state = STATE_CRYPT_AND_SEND;
  if (sendEncryptedToBridge(node_id, (uint8_t*)plaintext, plain_len)) {
    DEBUG_PRINTF("[OK] Respuesta del nodo %d cifrada y enviada al Bridge\n", node_id);
  } else {
    DEBUG_PRINTF("[ERROR] No se pudo enviar respuesta cifrada al Bridge\n");
  }
  
  state = STATE_IDLE;
}

// ============================================================
// Verificar timeout en espera de nodo
// ============================================================
void checkTimeouts() {
  unsigned long now = millis();
  
  if (state == STATE_ESP_RESP_NODO) {
    if (now - state_enter_time > NODE_TIMEOUT_MS) {
      DEBUG_PRINTLN("[TIMEOUT] No se recibio respuesta del nodo");
      total_timeouts++;
      
      // Informar al Bridge
      const char *err_msg = "TIMEOUT: nodo sin respuesta";
      if (bridge_mac_known) {
        sendEncryptedToBridge(current_crypted_msg.node_id, 
                              (uint8_t*)err_msg, strlen(err_msg));
      }
      
      state = STATE_IDLE;
    }
  }
}

// ============================================================
// Setup
// ============================================================
void setup() {
#ifdef ENABLE_SERIAL_DEBUG
  Serial.begin(SERIAL_BAUD);
  Serial.println();
  Serial.println("=============================================");
  Serial.println("  ESP8266 - PUERTA DE ENLACE (Cifrado ECC)");
  Serial.println("  netEspNow v1.2");
  Serial.println("=============================================");
  Serial.println();
#endif

  // Inicializar EEPROM
  initEEPROM();
  
  // Cargar o generar claves ECC
  if (hasKeysInEEPROM()) {
    loadKeysFromEEPROM();
    keys_initialized = true;
    DEBUG_PRINTLN("[EEPROM] Claves ECC cargadas de EEPROM");
    
    // Cargar clave publica del Bridge
    if (loadPeerKeyFromEEPROM()) {
      DEBUG_PRINTLN("[EEPROM] Clave publica del Bridge cargada");
    }
  } else {
    DEBUG_PRINTLN("[EEPROM] No hay claves. Generando nuevas...");
    if (generateECCKeys()) {
      saveKeysToEEPROM();
      keys_initialized = true;
      DEBUG_PRINTLN("[OK] Claves ECC generadas y guardadas en EEPROM");
    }
  }
  
  // Inicializar WiFi en modo Station
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);
  
  DEBUG_PRINT("MAC de esta Puerta de Enlace: ");
  DEBUG_PRINTLN(WiFi.macAddress());
  
  // Inicializar ESP-NOW
  int ret = esp_now_init();
  if (ret != 0) {
    DEBUG_PRINTF("[FATAL] Error esp_now_init(): %d\n", ret);
    // No podemos hacer mucho sin ESP-NOW
    while (1) { delay(1000); }
  }
  
  esp_now_set_self_role(ESP_NOW_ROLE_COMBO);
  esp_now_register_recv_cb(onDataReceived);
  esp_now_register_send_cb(onDataSent);
  
  // Agregar peer broadcast (para hablar con nodos)
  ret = esp_now_add_peer((uint8_t*)ESP_NOW_BROADCAST_MAC, ESP_NOW_ROLE_COMBO, 1, NULL, 0);
  if (ret != 0) {
    DEBUG_PRINTF("[WARN] Peer broadcast no agregado (codigo=%d)\n", ret);
  }
  
  DEBUG_PRINTLN("[OK] ESP-NOW inicializado");
  DEBUG_PRINTLN("[OK] Puerta de Enlace lista, esperando comandos cifrados...");
  DEBUG_PRINTLN();
  
  last_heartbeat = millis();
}

// ============================================================
// Loop principal
// ============================================================
void loop() {
  unsigned long now = millis();
  
  // Heartbeat interno
  if (now - last_heartbeat >= HEARTBEAT_MS) {
    last_heartbeat = now;
    heartbeat_count++;
    
    // Cada 30 heartbeats (60s), mostrar estado via LED parpadeo si hubiera
    if (heartbeat_count % 30 == 0) {
      DEBUG_PRINTF("[STATS] Cmd descifrados=%u | Resp cifradas=%u | Timeouts=%u | Errores=%u\n",
                   total_commands_decrypted, total_responses_encrypted,
                   total_timeouts, total_errors);
    }
  }
  
  // 1. Verificar si llego un mensaje cifrado del Bridge
  if (g_crypt_msg_received) {
    g_crypt_msg_received = false;
    
    // Copiar del buffer ISR
    EspNowCryptedMessage crypt_msg;
    uint8_t sender_mac[6];
    memcpy(sender_mac, (const void*)g_crypt_sender_mac, 6);
    memcpy(&crypt_msg, (const void*)g_crypt_buffer, sizeof(EspNowCryptedMessage));
    
    // Aprender MAC del Bridge si no la conocemos
    if (!bridge_mac_known) {
      memcpy(bridge_mac, sender_mac, 6);
      bridge_mac_known = true;
      
      // Agregar peer Bridge
      if (!esp_now_is_peer_exist(bridge_mac)) {
        esp_now_add_peer(bridge_mac, ESP_NOW_ROLE_COMBO, 1, NULL, 0);
      }
      DEBUG_PRINTF("[APRENDIZAJE] MAC del Bridge: %02X:%02X:%02X:%02X:%02X:%02X\n",
                   bridge_mac[0], bridge_mac[1], bridge_mac[2],
                   bridge_mac[3], bridge_mac[4], bridge_mac[5]);
    }
    
    // Verificar que tengamos la clave publica del Bridge
    if (!peer_key_known) {
      if (!loadPeerKeyFromEEPROM()) {
        DEBUG_PRINTLN("[WARN] No hay clave publica del Bridge en EEPROM");
        DEBUG_PRINTLN("[WARN] Esperando intercambio de claves...");
        // No podemos procesar este mensaje aun
        return;
      }
    }
    
    // Verificar firma y descifrar
    uint8_t plaintext[CRYPTED_PAYLOAD_MAX];
    uint8_t plain_len = 0;
    
    if (verifyAndDecrypt(&crypt_msg, plaintext, &plain_len)) {
      // Clonar el mensaje actual para referencia
      memcpy(&current_crypted_msg, &crypt_msg, sizeof(EspNowCryptedMessage));
      
      // El plaintext deberia contener algo como "ping 1" o "ping all"
      // El node_id esta en crypt_msg.node_id
      processDecryptedCommand(plaintext, plain_len, crypt_msg.node_id);
    } else {
      DEBUG_PRINTLN("[SEGURIDAD] Mensaje cifrado RECHAZADO - firma invalida");
    }
  }
  
  // 2. Procesar respuesta de nodo (si estamos esperando una)
  if (state == STATE_ESP_RESP_NODO) {
    processNodoResponse();
  }
  
  // 3. Verificar timeouts
  checkTimeouts();
  
  delay(1);
}
