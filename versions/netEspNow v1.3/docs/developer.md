# Developer Docs — netEspNow v1.3

**Versión**: 1.3  
**Fecha**: 31 Julio 2026

Guía para desarrolladores que mantienen o extienden el firmware de la red netEspNow.

---

## 1. Estructura de archivos

```
firmware_comun/netesponw_comun.h        ← Cabecera común (fuente de verdad del protocolo)
firmware_bridge/firmware_bridge.ino     ← Bridge (PC ↔ Puerta, cifrado ECC)
firmware_bridge/netesponw_comun.h       ← Copia local (compilación independiente)
firmware_puerta_enlace/firmware_puerta_enlace.ino ← Puerta (intermediario)
firmware_puerta_enlace/netesponw_comun.h
firmware_nodo_0/firmware_nodo_0.ino     ← Plantilla base I/O (NODE_ID=0)
firmware_nodo_1..4/                     ← Copias con NODE_ID distinto
```

> **IMPORTANTE**: `firmware_comun/netesponw_comun.h` es la fuente de verdad. Los demás
> `.ino` tienen una copia local en su carpeta porque arduino-cli compila cada sketch
> de forma independiente. Al cambiar el protocolo, **actualizar las 8 copias**
> (bridge, puerta, nodo_0..4) o copiar desde `firmware_comun/`.

## 2. Protocolo (cabecera común 21 B)

```c
typedef struct __attribute__((packed)) {
  uint8_t  msg_type;        // 0=request, 1=response, 2=command
  uint8_t  node_id;         // 0-4
  uint8_t  node_function;   // NODE_FUNC_IO=11
  uint8_t  fw_version;      // 13
  int8_t   rssi_db;         // RESERVADO = 0
  uint32_t counter;         // correlativo
  uint32_t uptime_ms;       // uptime del nodo
  uint8_t  payload[8];      // payload de la función
} EspNowNodeMessageV13;     // 21 B (static_assert)
```

El campo `msg_type` amplía el protocolo v1.1:

| msg_type | Nombre | Dirección | Uso |
|----------|--------|-----------|-----|
| 0 | MSG_TYPE_REQUEST | Puerta → Nodo | Petición de estado |
| 1 | MSG_TYPE_RESPONSE | Nodo → Puerta | Respuesta 21 B |
| 2 | MSG_TYPE_COMMAND | Puerta → Nodo | Actuación (`CMD_SET_STATE` etc.) |

### Payload I/O

```
payload[0] = gpio_in   (uint8, D5)
payload[1] = adc_raw   (uint16 little-endian, A0)
payload[3] = gpio_out  (uint8, D1)
payload[4..7] reservado
```

## 3. Flujo de datos

### Bridge (`firmware_bridge.ino`)

```
loop()
 ├─ leer comando serial → processSerialCommand()
 │    ├─ "ping <id>" / "ping all" / "set <id> on|off"
 │    └─ sendEncryptedCommand(id, texto)
 │         ├─ cifrar payload (XOR con keystream ECDH)
 │         ├─ firmar (ECDSA)
 │         └─ esp_now_send(Puerta)
 ├─ enviarPingAutomatico() cada POLL_INTERVAL_MS (si auto on)
 ├─ processEncryptedResponse()  (ISR copia → loop procesa)
 │    ├─ verificar firma ECDSA
 │    ├─ descifrar payload
 │    ├─ parseIOState(texto, node_id)   ← extrae i=/a=/o=
 │    └─ imprimir "[FUNC IO] in=.. adc=.. out=.."
 └─ watchdog: consecutive_timeouts >= BRIDGE_WDT_TIMEOUTS → ESP.restart()
```

### Puerta de Enlace (`firmware_puerta_enlace.ino`)

```
loop()
 ├─ processEncryptedCommand() (ISR copia → loop procesa)
 │    ├─ verificar firma ECDSA del Bridge
 │    ├─ descifrar payload → texto ("ping N" / "set N on")
 │    ├─ parsear texto
 │    │    ├─ "ping N" → msg_type=REQUEST
 │    │    └─ "set N on|off" → msg_type=COMMAND + CMD_SET_STATE
 │    ├─ armar EspNowNodeMessageV13 de 21 B
 │    └─ sendMessageToNode(id, msg)
 ├─ processNodoResponse()
 │    ├─ recibir respuesta 21 B del nodo
 │    ├─ construir texto corto: "N4 c=.. u=..ms i=.. a=.. o=.."
 │    ├─ cifrar + firmar
 │    └─ sendEncryptedToBridge()
 └─ watchdog: checkNetworkWatchdog() → ESP.restart() tras 120 s
```

### Nodo (`firmware_nodo_0.ino` plantilla)

```
setup()
 ├─ leer NODE_ID (definido por #define en cada nodo)
 ├─ initNodeEEPROM()  (magic "NS13" en EEPROM)
 ├─ pinMode(PIN_GPIO_IN, INPUT_PULLUP)
 ├─ pinMode(PIN_ADC, INPUT)
 ├─ pinMode(PIN_GPIO_OUT, OUTPUT)
 └─ registrar peer broadcast + callback

loop()
 ├─ processReceivedMessage()  (ISR copia → loop procesa)
 │    ├─ MSG_TYPE_REQUEST → responder con estado actual
 │    ├─ MSG_TYPE_COMMAND
 │    │    ├─ CMD_SET_STATE → digitalWrite(PIN_GPIO_OUT, payload[1])
 │    │    └─ ... responder con estado nuevo
 │    └─ armar EspNowNodeMessageV13 (21 B)
 ├─ heartbeat LED_BUILTIN cada 500 ms
 └─ watchdog: sin request/CMD en 120 s → ESP.restart()
```

## 4. ISR (callbacks ESP-NOW)

Los callbacks ESP-NOW se ejecutan en **contexto de interrupción**:

- **Prohibido**: `Serial.print()`, `delay()`, `ESP.restart()`, operaciones lentas.
- **Correcto**: copiar el buffer y MAC origen a variables `volatile`, activar un flag, procesar en `loop()`.
- **Protección de longitud obligatoria**: `if (len >= sizeof(EspNowNodeMessageV13))` antes de `memcpy` para evitar overflow.

Patrón usado en los 3 firmwares:

```c
static volatile bool g_rx_pending = false;
static volatile uint8_t g_rx_buffer[sizeof(EspNowNodeMessageV13)];

void onEspNowRecv(uint8_t *mac, uint8_t *data, uint8_t len) {
  if (len >= sizeof(EspNowNodeMessageV13)) {
    memcpy((void*)g_rx_buffer, data, sizeof(EspNowNodeMessageV13));
    memcpy((void*)g_rx_sender_mac, mac, 6);
    g_rx_pending = true;
  }
}
```

## 5. Cifrado ECC (Bridge ↔ Puerta)

### Derivación de clave compartida

```c
uint8_t shared_secret[ECC_PRIV_KEY_SIZE];
for (int i = 0; i < ECC_PRIV_KEY_SIZE; i++) {
  shared_secret[i] = my_private_key[i] ^ peer_public_key[i];
}
```

> El "ECDH" del proyecto usa XOR entre la clave privada propia y la pública del otro
> (simplificación del proyecto; **no** es ECDH canónico).

### XOR stream

```c
// Cifrar/descifrar: XOR es simétrico
for (int i = 0; i < payload_len; i++) {
  int k = (counter + i) % 32;
  uint8_t key_byte = shared_secret[k] ^ scramble;
  plaintext[i] = ciphertext[i] ^ key_byte;
}
```

### Firma ECDSA

```c
// Firmar: counter + node_id + payload + payload_len
uint8_t data_to_sign[4 + CRYPTED_PAYLOAD_MAX + 2];
// ... ensamblar ...
uECC_sign(private_key, data_to_sign, sizeof(data_to_sign), signature, &curve);
```

**Orden de validación (crítico)**: verificar firma → descifrar → procesar.
Nunca descifrar antes de verificar autenticidad.

## 6. Watchdog de red (v1.3)

| Dispositivo | Condición | Acción |
|-------------|-----------|--------|
| Bridge | `consecutive_timeouts >= 10` | `ESP.restart()` |
| Puerta | 120 s sin mensaje cifrado del Bridge | `ESP.restart()` |
| Nodo | 120 s sin request/CMD de la Puerta | `ESP.restart()` |

- El contador de timeouts del Bridge se **resetea a 0** con cada respuesta válida.
- El watchdog de red es **independiente** del Soft WDT del core (que se desactiva
  temporalmente durante ECDSA con `ESP.wdtDisable()` / `ESP.wdtEnable(0)`).

## 7. Notas del core ESP8266 3.1.2

- **NO usar `delay()` después de `WiFi.disconnect()`** — bug conocido del core; puede colgar el dispositivo.
- El IRAM está al 91-92% en compilaciones con uECC/bearssl (límite 65536 B). Al añadir código, vigilar que no supere el 100%.
- Compilar Bridge/Puerta requiere `--build-property "build.extra_flags=-I.../tools/sdk/include/bearssl"` (uECC necesita `bearssl.h`).

## 8. Cómo añadir una función de nodo nueva

1. Añadir `#define NODE_FUNC_XXX` en `netesponw_comun.h` (fuente + 7 copias).
2. Definir el layout del payload en `PAYLOAD_XXX_*`.
3. En el nodo: leer sensores y rellenar `payload[]` según el layout.
4. En el Bridge: extender `parseIOState()` para extraer los nuevos campos.
5. En la Puerta: si el comando requiere parámetros nuevos, ampliar el parseo de texto.
6. Compilar los 3 tipos, flashear y probar con `ping <id>` / `set <id> on|off`.

## 9. Compilación y verificación

```bash
# Bridge/Puerta (con bearssl)
arduino-cli compile --fqbn esp8266:esp8266:nodemcuv2 \
  --build-property "build.extra_flags=-I$HOME/.arduino15/packages/esp8266/hardware/esp8266/3.1.2/tools/sdk/include/bearssl" \
  firmware_bridge/

# Nodos (sin librerías extra)
arduino-cli compile --fqbn esp8266:esp8266:nodemcuv2 firmware_nodo_0/

# Verificación estática del tamaño de cabecera
grep -n "static_assert" firmware_*/netesponw_comun.h firmware_comun/netesponw_comun.h
```

---

*Documentación de desarrollador — netEspNow v1.3, Julio 2026*
