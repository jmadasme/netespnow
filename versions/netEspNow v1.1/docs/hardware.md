# Hardware — Pinout y Conexiones

## Placa: NodeMCU v3 (ESP8266-12E, CP2102)

### Especificaciones Técnicas

| Parámetro | Valor |
|---|---|
| Microcontrolador | ESP8266EX a 80/160 MHz |
| RAM | 80 KB (datos) + 64 KB (instrucciones) |
| Flash | 4 MB (ESP-12E) |
| WiFi | 802.11 b/g/n, 2.4 GHz |
| GPIO disponibles | 11 (D0–D8, TX, RX, A0) |
| ADC | 10 bits, 0–3.3V (pin A0) |
| USB-Serial | CP2102 |
| Alimentación | 5V vía USB o VIN (7–12V) |
| Consumo | ~80 mA (activo), ~10 µA (deep sleep) |

### Mapa de Pines NodeMCU v3

```
          ┌─────────────────────────────┐
          │                             │
  RST ────┤  D0 (GPIO16)  ◄───── WAKE  ├───
  ADC ────┤  D1 (GPIO5)   ◄───── SCL   ├───
          │  D2 (GPIO4)   ◄───── SDA   ├───
  3V3 ────┤  D3 (GPIO0)   ◄───── FLASH ├───
  GND ────┤  D4 (GPIO2)   ◄── LED_BUIL ├───  ← LED_BUILTIN
  GND ────┤  D5 (GPIO14)  ◄───── SCK   ├───
  Vin ────┤  D6 (GPIO12)  ◄───── MISO  ├───
          │  D7 (GPIO13)  ◄───── MOSI  ├───
          │  D8 (GPIO15)  ◄───── CS    ├───
          │                             │
  TX ─────┤  D9 (GPIO1)   ───► Serial  ├───  (a PC)
  RX ─────┤  D10(GPIO3)  ◄─── Serial  ├───  (a PC)
          └─────────────────────────────┘
```

### Pines Usados en el Firmware

| Pin | GPIO | Señal | Dirección | Propósito |
|-----|------|-------|-----------|-----------|
| D4 | GPIO2 | LED_BUILTIN | OUTPUT | LED indicador (heartbeat) |
| TX | GPIO1 | Serial TX | OUTPUT | Comunicación con PC (115200) |
| RX | GPIO3 | Serial RX | INPUT | Comunicación con PC (115200) |

### LED_BUILTIN (GPIO2, D4)

- **Activo en LOW**: `digitalWrite(LED, LOW)` = LED encendido
- El firmware del nodo parpadea el LED 1 vez por segundo como heartbeat
- Al iniciar, parpadea 3 veces rápidas para indicar que el firmware está vivo
- El bridge parpadea el LED al recibir una respuesta de un nodo

### Alimentación

- Los nodos se alimentan por USB (5V desde el PC o un cargador USB)
- El bridge se alimenta por USB desde el PC que ejecuta el monitor serial
- **No superar 3.3V** en los pines GPIO — el ESP8266 no es tolerante a 5V
