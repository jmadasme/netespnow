# Protocolo ESP-NOW — netEspNow v1.3

**Versión**: 1.3 — cabecera común extendida de 21 B

## Resumen

Dos familias de mensajes conviven en la red:
1. **Mensajes planos de 21 B** (`EspNowNodeMessageV13`) — entre Puerta de Enlace y Nodos 0-4
2. **Mensajes cifrados** (msg_type 2/3, 103 B) — entre Bridge y Puerta de Enlace

---

## 1. Cabecera común extendida (Nodos ↔ Puerta) — 21 bytes

```c
typedef struct __attribute__((packed)) {
  uint8_t  msg_type;        // 0=request, 1=response, 2=command
  uint8_t  node_id;         // ID del nodo (0-4)
  uint8_t  node_function;   // NODE_FUNC_IO=11 en v1.3
  uint8_t  fw_version;      // NODE_FW_VERSION=13
  int8_t   rssi_db;         // RESERVADO = 0 (medición descartada v1.3)
  uint32_t counter;         // Contador correlativo de respuestas
  uint32_t uptime_ms;       // Tiempo de actividad del nodo
  uint8_t  payload[8];      // Datos específicos de la función
} EspNowNodeMessageV13;     // 21 bytes
```

**Tamaño total**: 21 bytes (verificado con `static_assert`).

### Tipos de mensaje plano

| msg_type | Nombre | Dirección | Descripción |
|----------|--------|-----------|-------------|
| 0 | MSG_TYPE_REQUEST | Puerta → Nodo | Pide estado al nodo |
| 1 | MSG_TYPE_RESPONSE | Nodo → Puerta | Respuesta con cabecera 21 B |
| 2 | MSG_TYPE_COMMAND | Puerta → Nodo | Comando de actuación |

### Payload I/O (`NODE_FUNC_IO = 11`)

| Índice | Campo | Tipo | Descripción |
|--------|-------|------|-------------|
| 0 | `PAYLOAD_IO_GPIO_IN` | uint8 | Estado entrada digital (D5) |
| 1 | `PAYLOAD_IO_ADC_RAW` | uint16 | Valor ADC (A0), little endian |
| 3 | `PAYLOAD_IO_GPIO_OUT` | uint8 | Estado salida digital (D1) |
| 4-7 | `PAYLOAD_IO_RESERVED` | 4 B | Reservado |

### Comandos de actuación (msg_type = 2)

| CMD | Valor | payload | Descripción |
|-----|-------|---------|-------------|
| CMD_SET_STATE | 0x01 | payload[1]=0/1 | Escribe PIN_GPIO_OUT (on/off) |
| CMD_SET_PWM | 0x02 | — | Futuro (función PWM) |
| CMD_GET_STATE | 0x03 | — | Fuerza respuesta inmediata |

### Flujo de petición de estado

```
Puerta de Enlace                      Nodo X
     │                                    │
     │── msg_type=0, node_id=X, 21 B ────>│
     │                                    │ (procesa en loop, no ISR)
     │                                    │── msg_type=1, 21 B
     │<────────────────────────────────────│   node_function=11, fw=13
     │                                    │   payload[0]=gpio_in
     │                                    │   payload[1..2]=adc (LE)
     │                                    │   payload[3]=gpio_out
```

### Flujo de actuación

```
Puerta de Enlace                      Nodo X
     │                                    │
     │── msg_type=2, CMD_SET_STATE ──────>│
     │    payload[1]=1 (ON)               │
     │                                    │ digitalWrite(D1, HIGH)
     │                                    │── msg_type=1 (estado nuevo)──>│
     │<────────────────────────────────────│
```

### ISR en nodos

Los callbacks ESP-NOW se ejecutan en contexto de interrupción. Por eso:
- El nodo solo copia la MAC origen y activa un flag (`volatile bool g_rx_pending`)
- El procesamiento real ocurre en `loop()`
- No se usa `Serial.print()` ni `delay()` dentro del ISR
- Se valida `len >= sizeof(EspNowNodeMessageV13)` antes de copiar

---

## 2. Mensajes Cifrados (Bridge ↔ Puerta) — 103 bytes

```c
#define CRYPTED_PAYLOAD_MAX 32

typedef struct __attribute__((packed)) {
  uint8_t  msg_type;       // 2=bridge->puerta, 3=puerta->bridge
  uint8_t  node_id;        // ID del nodo destino (0-4)
  uint32_t counter;        // Contador correlativo (anti-replay)
  uint8_t  payload[32];    // Datos cifrados (XOR con keystream ECDH)
  uint8_t  payload_len;    // Longitud real del payload
  uint8_t  signature[64];  // Firma ECDSA (secp256r1)
} EspNowCryptedMessage;
```

**Tamaño total**: 1+1+4+32+1+64 = **103 bytes**

### Tipos

| msg_type | Dirección | Descripción |
|----------|-----------|-------------|
| 2 | Bridge → Puerta | Comando cifrado (ej: "ping 1", "set 1 on") |
| 3 | Puerta → Bridge | Respuesta cifrada |

### Flujo

```
Bridge                              Puerta de Enlace
  │                                      │
  │ 1. Cifra payload con ECDH+XOR      │
  │ 2. Firma con ECDSA (clave privada) │
  │── msg_type=2, node_id, counter ────>│
  │    payload(cifrado), signature     │
  │                                      │
  │                                      │ 3. Verifica firma ECDSA
  │                                      │ 4. Descifra con ECDH+XOR
  │                                      │ 5. Procesa comando
  │                                      │ 6. ... (interroga nodo) ...
  │                                      │ 7. Cifra respuesta
  │                                      │ 8. Firma respuesta
  │<── msg_type=3, node_id, counter ─────│
  │    payload(cifrado), signature      │
  │                                      │
  │ 9. Verifica firma                    │
  │ 10. Descifra y muestra               │
```

### Formato del payload plano en la respuesta cifrada

La Puerta construye un texto corto de respuesta que el Bridge parsea con `strstr`:

```
N<id> c=<counter> u=<uptime_ms>ms i=<gpio_in> a=<adc> o=<gpio_out>
```

Ejemplo: `N4 c=12 u=34567ms i=1 a=512 o=0`

El Bridge extrae `i=`, `a=` y `o=`; solo si los tres patrones existen marca el nodo
como `has_io=true` (un `TIMEOUT: nodo sin respuesta` no marca I/O).

---

## 3. Cifrado ECC

### Algoritmo

| Operación | Algoritmo | Propósito |
|-----------|-----------|-----------|
| Generación de claves | secp256r1 vía uECC | Par (privada, pública) |
| Confidencialidad | XOR stream (derivado de clave) | Cifrar payload |
| Autenticidad | ECDSA | Firmar/verificar origen |
| Anti-replay | Contador de 32 bits | Evitar repetición |

### XOR stream

1. Emisor deriva `shared_secret[i] = my_private_key[i] ^ peer_public_key[i]`
2. `key_stream[i] = shared_secret[(counter + i) % 32] ^ scramble`
3. Cifra: `ciphertext[i] = plaintext[i] ^ key_stream[i]`
4. Descifra: mismo proceso (XOR es simétrico)

### ECDSA

1. Bridge firma: `counter + node_id + payload + payload_len` con su clave privada
2. Puerta verifica con la clave pública del Bridge
3. Si la firma no coincide → mensaje **rechazado**

---

## 4. Gestión de Peers

| Dispositivo | Peers |
|------------|-------|
| Bridge | Broadcast + Puerta de Enlace |
| Puerta de Enlace | Broadcast + Bridge (aprendida) |
| Nodos 0-4 | Broadcast + quien les haga request (aprendida) |

### Broadcast

Se usa MAC `FF:FF:FF:FF:FF:FF` para descubrimiento inicial. Una vez conocida la MAC destino, se usa envío directo.

---

## 5. Timeouts y Watchdog de red (v1.3)

| Timeout | Valor | Monitoreado por | Acción |
|---------|-------|-----------------|--------|
| Bridge espera Puerta | 2000 ms | Bridge | Marca timeout, incrementa contador |
| Bridge timeouts consecutivos | 10 | Bridge | `ESP.restart()` |
| Puerta sin mensaje cifrado | 120 s | Puerta de Enlace | `ESP.restart()` |
| Nodo sin request/CMD | 120 s | Nodo | `ESP.restart()` |

---

*Protocolo netEspNow v1.3 — Julio 2026*
