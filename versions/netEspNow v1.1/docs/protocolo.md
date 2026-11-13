# Protocolo ESP-NOW — Especificación Técnica

## Introducción

ESP-NOW es un protocolo de comunicación inalámbrica desarrollado por Espressif que permite la transmisión directa de paquetes entre dispositivos ESP8266/ESP32 **sin necesidad de un punto de acceso WiFi**. Es ideal para redes de sensores de baja latencia y bajo consumo.

### Características Clave

- **Sin conexión**: No requiere handshake, asociación AP, ni infraestructura de red
- **Baja latencia**: Típicamente 1–6 ms de ida y vuelta (RTT)
- **Peer-to-peer**: Comunicación directa entre dispositivos
- **Broadcast**: Soporta envío a todos los dispositivos en alcance
- **Tamaño de paquete**: Máximo 250 bytes por mensaje

---

## Estructura del Mensaje

Todos los mensajes ESP-NOW en esta red usan una estructura binaria de **8 bytes**:

```
Offset  Tamaño  Campo       Tipo     Descripción
──────  ──────  ──────────  ───────  ───────────────────────────
0       1       msg_type    uint8_t  0=Request, 1=Response
1       1       node_id     uint8_t  ID del nodo (0-4)
2       4       counter     uint32_t Contador correlativo
6       4       uptime_ms   uint32_t Tiempo activo del nodo (ms)
```

### Definición en C (ambos firmwares deben coincidir)

```c
typedef struct __attribute__((packed)) {
  uint8_t  msg_type;    // 0=request, 1=response
  uint8_t  node_id;     // ID del nodo destino/origen (0-4)
  uint32_t counter;     // Contador correlativo (nodo) / 0 (request)
  uint32_t uptime_ms;   // Tiempo de actividad del nodo en ms
} EspNowMessage;
```

**Importante**: El atributo `__attribute__((packed))` es necesario para evitar que el compilador introduzca bytes de relleno (padding) entre los campos, garantizando que la estructura ocupe exactamente 8 bytes.

---

## Flujo de Mensajes

### 1. Request (Bridge → Nodo)

```
Bridge envía:  msg_type=0, node_id=X, counter=0, uptime_ms=0
```

El bridge construye un mensaje de tipo REQUEST (0) con el ID del nodo destino. Los campos `counter` y `uptime_ms` se envían en 0 (no se usan en el request).

**Envío directo vs broadcast**:
- Si el bridge **conoce la MAC** del nodo, envía directo a esa MAC
- Si la MAC es **desconocida**, envía por broadcast (`FF:FF:FF:FF:FF:FF`)

### 2. Procesamiento en el Nodo

Cuando un nodo recibe un mensaje:

```c
void onDataReceived(uint8_t *mac, uint8_t *data, uint8_t len) {
  if (len != sizeof(EspNowMessage)) return;  // Validar tamaño
  EspNowMessage *msg = (EspNowMessage*)data;
  
  // Solo responder si es un request dirigido a este nodo
  if (msg->msg_type == MSG_TYPE_REQUEST && msg->node_id == NODE_ID) {
    memcpy((void*)g_master_mac, mac, 6);      // Guardar MAC del bridge
    g_request_pending = true;                   // Señalizar al loop
  }
}
```

### 3. Response (Nodo → Bridge)

```
Nodo responde:  msg_type=1, node_id=X, counter=N, uptime_ms=MMMM
```

El nodo incrementa su contador interno y responde con:
- `counter`: Valor correlativo (se incrementa en cada respuesta)
- `uptime_ms`: Tiempo transcurrido desde el inicio del nodo (`millis()`)

### 4. Procesamiento en el Bridge

```c
void processResponse() {
  // Copiar datos del buffer ISR (seguro para el contexto principal)
  uint8_t id = msg.node_id;
  
  if (!nodes[id].known) {
    // Aprender MAC del nodo (autodescubrimiento)
    memcpy(nodes[id].mac, mac, 6);
    nodes[id].known = true;
  }
  
  // Mostrar datos por Serial
  Serial.printf("RESPUESTA RECIBIDA - Nodo #%d\n", id);
  Serial.printf("  MAC                 : %02X:%02X:%02X:%02X:%02X:%02X\n", ...);
  Serial.printf("  Contador correlativo: %u\n", msg.counter);
  Serial.printf("  Tiempo activo nodo  : %u ms (%u s)\n", 
                msg.uptime_ms, msg.uptime_ms / 1000);
  Serial.printf("  RTT                 : %u ms\n", rtt);
}
```

---

## Temporización y Timeouts

```
Bridge                           Nodo X
  │                                │
  ├──► Envía Request ─────────────►│
  │                                │
  │    ┌─── Inicia temporizador    │
  │    │     (1000 ms timeout)     │
  │    │                           │
  │    ├──► (llega respuesta) ────►│  ✓ Éxito
  │    │                           │
  ├──► Envía Request ─────────────►│
  │    │                           │
  │    ┌─── Inicia temporizador    │
  │    │     (1000 ms timeout)     │
  │    │                           │
  │    ✗ Timeout (sin respuesta)   │  ✗ Falla
  │    [TIMEOUT] Nodo X - No se    │
  │    recibio respuesta           │
  │                                │
```

- **Timeout**: 1000 ms (1 segundo)
- **Intervalo entre polls (auto)**: 3000 ms (3 segundos)
- El bridge no envía un nuevo request hasta que recibe respuesta o se agota el timeout

---

## Autodescubrimiento de Nodos

Cuando el bridge desconoce la MAC de un nodo (nuevo en la red), envía el request por **broadcast**. Todos los nodos reciben el mensaje, pero solo el nodo con el `node_id` solicitado responde.

Al recibir la respuesta, el bridge **aprende automáticamente** la MAC del nodo y la guarda en su tabla. A partir de ese momento, las comunicaciones son **directas** (no broadcast), lo que reduce el tráfico inalámbrico y mejora la confiabilidad.

---

## Consideraciones Técnicas

### ISR (Interrupt Service Routines)

Los callbacks de ESP-NOW (`onDataReceived`, `onDataSent`) se ejecutan en **contexto de interrupción (ISR)**. Esto implica:

- ❌ **NO** usar `Serial.print()`, `Serial.printf()`, `delay()`, `memcpy()` lentos
- ✅ Usar flags atómicos (`volatile bool`) para señalizar al loop principal
- ✅ Copiar datos mínimos necesarios y procesar en el loop principal
- ✅ Las variables compartidas deben ser `volatile`

### Peer Management

Cada nodo con el que el bridge quiere comunicarse debe ser **agregado como peer**:

```c
esp_now_add_peer(mac_destino, ESP_NOW_ROLE_COMBO, 1, NULL, 0);
```

El bridge agrega automáticamente:
- **Broadcast peer**: `FF:FF:FF:FF:FF:FF` (para descubrimiento)
- **Peers directos**: Las MAC conocidas de cada nodo (aprendidas o preconfiguradas)
