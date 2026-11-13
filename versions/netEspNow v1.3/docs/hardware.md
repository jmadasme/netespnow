# Hardware — netEspNow v1.3

## Bridge: NodeMCU v3 (ESP-12E)

| Especificación | Valor |
|---------------|-------|
| Chip | ESP8266EX |
| Flash | 4 MB (W25Q32) |
| RAM | 80 KB (datos) + 64 KB (instrucciones) |
| USB | CP2102 |
| Alimentación | 5V USB o 3.3V regulado |
| MAC | `48:3F:DA:05:86:36` |

### Pinout

| GPIO | Silkscreen | Función en v1.3 |
|------|-----------|-----------------|
| TX (GPIO1) | D9 | Serial a PC (115200) |
| RX (GPIO3) | D10 | Serial desde PC |
| GPIO2 | D4 | LED_BUILTIN (activo LOW) |
| GPIO0 | D3 | NO USADO |
| GPIO4 | D2 | NO USADO |
| GPIO5 | D1 | NO USADO (solo en nodos) |
| GPIO12 | D6 | NO USADO |
| GPIO13 | D7 | NO USADO |
| GPIO14 | D5 | NO USADO (solo en nodos) |
| GPIO16 | D0 | NO USADO |
| ADC | A0 | NO USADO (solo en nodos) |

## Puerta de Enlace: ESP8266 Cerámica

| Especificación | Valor |
|---------------|-------|
| Chip | ESP8266EX |
| Encapsulado | Cerámico sellado |
| GPIO | No accesibles |
| USB | Solo alimentación (por defecto) |
| Comunicación | Solo ESP-NOW |
| MAC | `C4:5B:BE:74:DB:4A` |

> **Nota**: Aunque tiene cable USB externalizado, la placa cerámica no expone pines GPIO. El USB se usa exclusivamente para alimentación durante operación normal. Durante la programación, el USB también transporta datos (TX/RX del conversor USB-serial interno).

## Nodos 0-4: NodeMCU v3 (ESP-12E) — Plantilla base I/O (v1.3)

Mismas especificaciones que el Bridge. En v1.3 cada nodo usa **3 pines de propósito general**:

| Nodo | MAC | GPIO entrada (D5) | ADC (A0) | GPIO salida (D1) | GPIO LED (D4) |
|------|-----|-------------------|----------|------------------|---------------|
| 0 | `84:F3:EB:7C:99:1E` | GPIO14 | A0 | GPIO5 | GPIO2 |
| 1 | `80:7D:3A:2B:E3:59` | GPIO14 | A0 | GPIO5 | GPIO2 |
| 2 | `8C:AA:B5:0F:39:7F` | GPIO14 | A0 | GPIO5 | GPIO2 |
| 3 | `50:02:91:DE:0D:7B` | GPIO14 | A0 | GPIO5 | GPIO2 |
| 4 | `48:3F:DA:AA:8E:6D` | GPIO14 | A0 | GPIO5 | GPIO2 |

### Función de cada pin en la plantilla I/O

| Pin | GPIO | Modo | Uso |
|-----|------|------|-----|
| D5 | GPIO14 | INPUT_PULLUP | Entrada digital (botón/contacto/sensor) — leído en `i=` |
| A0 | ADC | INPUT | ADC 0-1023 — leído en `a=` |
| D1 | GPIO5 | OUTPUT | Salida de actuación on/off — accionada con `set <id> on\|off` |
| D4 | GPIO2 | OUTPUT | LED_BUILTIN heartbeat (activo LOW) |

### Conexión típica

```
NodeMCU v3 (Nodo X)
┌───────────────────────┐
│ GPIO14 (D5) ●──[btn]──┤ GND
│ A0        ●──[pot]───┤ 3V3
│ GPIO5 (D1) ●──[LED]───┤ GND   (220 Ω)
│ GPIO2 (D4) ●──[LED]───┤ GND   (220 Ω, heartbeat)
│ USB                    │
└───────────────────────┘
```

---

*Hardware netEspNow v1.3 — Julio 2026*
