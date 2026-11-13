# Protocolo ESP-NOW — netEspNow v1.2

## Resumen

Dos tipos de mensajes conviven en la red:
1. **Mensajes planos** (msg_type 0/1) — entre Puerta de Enlace y Nodos 0-4
2. **Mensajes cifrados** (msg_type 2/3) — entre Bridge y Puerta de Enlace

---

## 1. Mensajes Planos (v1.1 compatible)

Usados en la comunicación Puerta de Enlace ↔ Nodos.

### Estructura

```c
typedef struct __attribute__((packed)) {
  uint8_t  msg_type;    // 0=request, 1=response
  uint8_t  node_id;     // ID del nodo (0-4)
  uint32_t counter;     // Contador correlativo
  uint32_t uptime_ms;   // Tiempo de actividad del nodo
} EspNowMessage;
```

**Tamaño total**: 12 bytes

### Tipos

| msg_type | Dirección | Descripción |
|----------|-----------|-------------|
| 0 | Puerta → Nodo | Request: pide respuesta al nodo |
| 1 | Nodo → Puerta | Response: respuesta con contador y uptime |

### Flujo

```
Puerta de Enlace                      Nodo X
     │                                    │
     │── msg_type=0, node_id=X ──────────>│
     │                                    │ (procesa en ISR)
     │                                    │── msg_type=1, node_id=X,
     │<────────────────────────────────────│   counter=N, uptime_ms
```

### ISR en nodos

Los callbacks ESP-NOW se ejecutan en contexto de interrupción. Por eso:
- El nodo solo copia la MAC origen y activa un flag (`volatile bool g_request_pending`)
- El procesamiento real ocurre en `loop()`
- No se usa `Serial.print()` ni `delay()` dentro del ISR

---

## 2. Mensajes Cifrados (nuevo en v1.2)

Usados en la comunicación Bridge ↔ Puerta de Enlace.

### Estructura

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
| 2 | Bridge → Puerta | Comando cifrado (ej: "ping 1") |
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

---

## 3. Cifrado ECC

### Algoritmo

| Operación | Algoritmo | Propósito |
|-----------|-----------|-----------|
| Generación de claves | secp256r1 vía uECC | Par (privada, pública) |
| Confidencialidad | ECDH + XOR stream | Cifrar payload |
| Autenticidad | ECDSA | Firmar/verificar origen |
| Anti-replay | Contador de 32 bits | Evitar repetición |

### ECDH + XOR

1. Emisor calcula: `shared = ECDH(priv_propia, pub_destinatario)`
2. Deriva keystream: `key_stream[i] = shared[(counter + i) % 32] ^ scramble`
3. Cifra: `ciphertext[i] = plaintext[i] ^ key_stream[i]`
4. Descifra: mismo proceso (XOR es simétrico)

### ECDSA

1. Bridge firma: `counter + node_id + payload` con su clave privada
2. Puerta verifica con la clave pública del Bridge
3. Si la firma no coincide → mensaje **rechazado**

---

## 4. Gestión de Peers

Cada dispositivo agrega peers según necesita:

| Dispositivo | Peers |
|------------|-------|
| Bridge | Broadcast + Puerta de Enlace |
| Puerta de Enlace | Broadcast + Bridge (aprendida) |
| Nodos 0-4 | Broadcast + quien les haga request (aprendida) |

### Broadcast

Se usa MAC `FF:FF:FF:FF:FF:FF` para descubrimiento inicial. Una vez conocida la MAC destino, se usa envío directo.

---

## 5. Timeouts

| Timeout | Valor | Monitoreado por |
|---------|-------|----------------|
| Bridge espera Puerta | 2000 ms | Bridge |
| Puerta espera Nodo | 1000 ms | Puerta de Enlace |
