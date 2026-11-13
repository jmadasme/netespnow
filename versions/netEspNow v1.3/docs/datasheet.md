# Datasheet — Red netEspNow v1.3 (Documentación de Usuario)

**Versión**: 1.3  
**Fecha**: 31 Julio 2026  
**Plataforma**: ESP8266 (NodeMCU v3 + módulo cerámico ESP-12F)

---

## 1. Qué es

netEspNow v1.3 es una **red inalámbrica de sensores y actuadores** basada en el protocolo **ESP-NOW** de Espressif. La red consta de:

| Elemento | Cantidad | Función |
|----------|----------|---------|
| **Bridge** (NodeMCU v3) | 1 | Interfaz con el PC por USB-serial; cifra y firma mensajes |
| **Puerta de Enlace** (ESP8266 cerámico) | 1 | Intermediario que descifra/cifra con ECC y reenvía a los nodos |
| **Nodos** (NodeMCU v3) | 5 (0-4) | Plantilla base I/O: entrada digital, ADC y salida digital |

## 2. Características principales

- **Cifrado asimétrico ECC (secp256r1)**: Bridge ↔ Puerta. Firmas ECDSA para autenticidad y contador anti-replay.
- **Cabecera común extendida de 21 bytes** entre Puerta y nodos, con función de nodo y versión de firmware.
- **Actuación remota**: el Bridge puede encender/apagar la salida digital de cualquier nodo con `set <id> on|off`.
- **Watchdog de red en toda la cadena** (auto-reinicio ante fallos de comunicación).
- **Aprendizaje dinámico**: el Bridge descubre la función (FUNC) y versión (FW) de cada nodo en la primera respuesta.

## 3. Especificaciones técnicas

| Parámetro | Valor |
|-----------|-------|
| MCU | ESP8266 (Tensilica Xtensa LX106, 80/160 MHz) |
| RAM | 80 KB disponibles (~36-40% usados en v1.3) |
| Flash | 4 MB (firmware ~250 KB, ~24%) |
| Voltaje | 3.3 V lógico (5 V tolerante en ADC según NodeMCU) |
| Comunicación | ESP-NOW, canal WiFi 1, sin router |
| Alcance típico | 30-100 m en exteriores (depende de antena/obstáculos) |
| Velocidad serial | 115200 baud |

## 4. Puertos y pines

### Nodo (plantilla base I/O)

| Pin | GPIO | Tipo | Uso | Rango |
|-----|------|------|-----|-------|
| D5 | GPIO14 | Entrada digital (pull-up interno) | Botón, contacto NO, sensor digital | 0/1 |
| A0 | ADC | Entrada analógica | Potenciómetro, LDR, divisor de tensión | 0-1023 |
| D1 | GPIO5 | Salida digital | LED, relé, actuador on/off | LOW/HIGH |
| D4 | GPIO2 | Salida digital | LED_BUILTIN heartbeat (activo LOW) | LOW/HIGH |
| VIN/5V | — | Alimentación | USB o fuente externa | 5 V (regulado) |
| 3V3 | — | Alimentación sensores | — | 3.3 V |

### Bridge

| Pin | GPIO | Uso |
|-----|------|-----|
| D9 (TX) | GPIO1 | Serial TX al PC |
| D10 (RX) | GPIO3 | Serial RX del PC |
| D4 | GPIO2 | LED_BUILTIN indicador (activo LOW) |

## 5. Forma de operar

1. Conectar el Bridge al PC por USB → aparece como `/dev/ttyUSB0`.
2. Conectar la Puerta de Enlace a USB (solo alimentación) → `/dev/ttyUSB1`.
3. Abrir un monitor serial a 115200 baud.
4. Encender los nodos 0-4 (alimentación USB o batería).
5. Enviar comandos desde el monitor (ver tabla de comandos).

### Comandos

| Comando | Descripción |
|---------|-------------|
| `ping <id>` | Pide estado actual al nodo (0-4) |
| `ping all` | Pide estado a todos los nodos secuencialmente |
| `set <id> on` | Enciende la salida digital (D1) del nodo |
| `set <id> off` | Apaga la salida digital (D1) del nodo |
| `list` | Muestra tabla de nodos (función + FW) |
| `stats` | Muestra estadísticas de comunicación y estado I/O |
| `auto on` | Interrogación automática cada ~3 s |
| `auto off` | Detiene la interrogación automática |
| `help` | Muestra ayuda |

## 6. Ejemplos de uso

```bash
# Ver la tabla de nodos
list

# Preguntar estado del nodo 2
ping 2
# → [FUNC IO] in=1 adc=720 out=0

# Encender LED/relé del nodo 2
set 2 on
# → [FUNC IO] in=1 adc=720 out=1

# Encender todo y dejar interrogación automática
auto on
```

### Interpretación de la respuesta

```
RESPUESTA DESCIFRADA - Nodo #2 [FUNC IO] in=1 adc=720 out=0
```

| Campo | Significado |
|-------|-------------|
| `in=1` | Entrada digital D5: hay tensión (botón presionado / contacto cerrado) |
| `adc=720` | ADC A0: valor analógico (0-1023) |
| `out=0` | Salida digital D1: apagada |

## 7. Diagrama de conexión (nodo típico)

```
                   3V3 ──[ LDR ]──┬── A0
                                 └──[ 10 kΩ ]── GND

   GND ───[ botón ]─── D5 (GPIO14, pull-up interno)

   D1 (GPIO5) ──[ 220 Ω ]── LED ── GND

   D4 (GPIO2) ──[ 220 Ω ]── LED ── GND   (heartbeat)
```

## 8. Calibración

La plantilla base I/O **no requiere calibración** para uso digital (D5/D1). Para el ADC:

1. Con el sensor en reposo, anotar `adc` en `stats` o `ping <id>`.
2. Con el sensor en valor máximo, anotar el nuevo `adc`.
3. Mapear linealmente si se necesita la magnitud física:
   `magnitud = valor_adc * (max_fisico / 1023)`

Para un LDR simple: a mayor luz → menor resistencia → mayor tensión en A0 → mayor `adc`.

## 9. Advertencias

- **No conectar 5 V a los pines GPIO** (solo 3.3 V; el NodeMCU v3 es tolerante en algunos pines pero la plantilla usa 3.3 V).
- **Resistencia limitadora en LEDs** (220 Ω mínimo) para no dañar los GPIO.
- **Puerta de Enlace sin serial de debug**: no es posible monitorear su estado en vivo; usar el Bridge como ventana.
- **Watchdog**: si la red se queda sin comunicación, los dispositivos se reinician solos (Bridge tras 10 timeouts; Puerta y nodos tras 120 s).
- **No usar `delay()` tras `WiFi.disconnect()`** en el código (bug del core ESP8266 3.1.2) — ya respetado en el firmware v1.3.

## 10. Estado de entrega

| Componente | Estado |
|------------|--------|
| Bridge | ✅ Flasheado v1.3 y validado (ttyUSB0) |
| Puerta de Enlace | ✅ Flasheado v1.3 y validado (ttyUSB1) |
| Nodos 0-4 | ✅ Compilados — **pendiente flashear** cuando se conecten las placas |

---

*Documento generado para la entrega netEspNow v1.3 — Julio 2026*
