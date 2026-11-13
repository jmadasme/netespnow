# Hardware — netEspNow v1.2

## Bridge: NodeMCU v3 (ESP-12E)

| Especificación | Valor |
|---------------|-------|
| Chip | ESP8266EX |
| Flash | 4 MB (W25Q32) |
| RAM | 80 KB (datos) + 64 KB (instrucciones) |
| USB | CP2102 |
| Alimentación | 5V USB o 3.3V regulado |

### Pinout

| GPIO | Silkscreen | Función en v1.2 |
|------|-----------|-----------------|
| TX (GPIO1) | D9 | Serial a PC (115200) |
| RX (GPIO3) | D10 | Serial desde PC |
| GPIO2 | D4 | LED_BUILTIN (activo LOW) |
| GPIO0 | D3 | NO USADO |
| GPIO4 | D2 | NO USADO |
| GPIO5 | D1 | NO USADO |
| GPIO12 | D6 | NO USADO |
| GPIO13 | D7 | NO USADO |
| GPIO14 | D5 | NO USADO |
| GPIO16 | D0 | NO USADO |
| ADC | A0 | NO USADO |

## Puerta de Enlace: ESP8266 Cerámica

| Especificación | Valor |
|---------------|-------|
| Chip | ESP8266EX |
| Encapsulado | Cerámico sellado |
| GPIO | No accesibles |
| USB | Solo alimentación (por defecto) |
| Comunicación | Solo ESP-NOW |

**MAC**: `48:55:19:14:A8:39`

> **Nota**: Aunque tiene cable USB externalizado, la placa cerámica no expone pines GPIO. El USB se usa exclusivamente para alimentación durante operación normal. Durante la programación, el USB también transporta datos (TX/RX del conversor USB-serial interno).

## Nodos 0-4: NodeMCU v3 (ESP-12E)

Mismas especificaciones que el Bridge. Sin cambios respecto a v1.1.

| Nodo | MAC | GPIO usados |
|------|-----|------------|
| 0 | `84:F3:EB:7C:99:1E` | GPIO2 (LED) |
| 1 | `80:7D:3A:2B:E3:59` | GPIO2 (LED) |
| 2 | `8C:AA:B5:0F:39:7F` | GPIO2 (LED) |
| 3 | `50:02:91:DE:0D:7B` | GPIO2 (LED) |
| 4 | `48:3F:DA:AA:8E:6D` | GPIO2 (LED) |
