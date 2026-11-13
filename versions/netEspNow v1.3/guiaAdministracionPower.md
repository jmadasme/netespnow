# Guía de Administración de Potencia — CubeCell + Batería 18650

## Objetivo

Alcanzar **1 año de autonomía energética** con un Heltec CubeCell HTCC-AB01
alimentado por una sola celda 18650, reportando temperatura cada **10 minutos**
por LoRaWAN Clase A en AU915.

---

## 1. Por qué ves uplinks de sincronía aunque el dispositivo "debería estar durmiendo"

En Clase A, el dispositivo **no escucha** salvo en las ventanas RX1 y RX2 que abre
tras cada uplink. Sin embargo, el stack LoRaWAN (RadioLib en CubeCell) y el
Network Server intercambian comandos MAC que fuerzan tráfico adicional.

### 1.1 MAC commands que generan tráfico invisible

| Comando MAC | Quién lo inicia | Propósito | Impacto energético |
|---|---|---|---|
| `LinkCheckReq` | Dispositivo → Red | Verificar conectividad | Fuerza un downlink de respuesta |
| `LinkADRReq/Ans` | Red → Dispositivo | Ajustar SF/TX power/canal | Si ADR activo, forzado periódicamente |
| `ADRParamSetupReq` | Red → Dispositivo | Configurar límites de ADR | Respuesta forzada |
| `DutyCycleReq/Ans` | Red | Forzar duty cycle | Respuesta forzada |
| `RXTimingSetupReq/Ans` | Red | Ajustar RX1 delay y RX2 | Respuesta forzada |

### 1.2 ADR y el "efecto ACK forzado"

El estándar LoRaWAN (v1.0.3/v1.0.4) define:

- **ADR_ACK_LIMIT = 64**: Si el dispositivo envía 64 uplinks sin recibir un
  downlink de la red, debe setear el bit `ADRACKReq` en el próximo uplink.
- **ADR_ACK_DELAY = 32**: Si tras setear el bit, pasan 32 uplinks más sin
  respuesta, se asume pérdida de conectividad y se inicia un procedimiento
  de recuperación (bajar SF, subir potencia, etc.).

**Cada vez que el bit ADRACKReq está seteado, el Network Server DEBE responder**,
lo que implica que el dispositivo tiene que abrir ventanas RX y queda despierto.

### 1.3 Otros factores

- **Join / Re-Join**: Si la sesión expira o se pierde, el dispositivo hace
  un re-join completo que consume ~10 segundos de TX agresivo.
- **Confirmed uplinks**: Si usas `confirmed=true`, el dispositivo espera
  forzosamente por un ACK del servidor.
- **LinkCheck**: Configuración por defecto en algunos stacks que envía
  LinkCheckReq periódicamente.

### 1.4 Conclusión del problema

Aún con ciclo de reporte de 10 minutos, la radio se despierta:
- En cada TX de aplicación
- En ventanas RX1/RX2 (forzosas tras cada uplink)
- En respuestas a comandos MAC (ADR, LinkCheck, etc.)
- En re-joins si hay desconexión

**Cada despertar cuesta ~6 segundos de actividad de radio (~40-120 mA)**, y esto
se suma al consumo de sleep. En SF12 ese costo puede ser 50× mayor que en SF7.

---

## 2. Perfil de consumo del CubeCell HTCC-AB01

| Modo | Corriente típica @ 3.7V |
|---|---|
| Deep sleep (MCU + radio apagada) | **3.5 µA** |
| MCU activa (run mode) | ~8-12 mA |
| SX1262 en idle (RC oscilator) | ~600 nA |
| SX1262 RX activa | 8-10 mA |
| SX1262 TX a +14 dBm | 40 mA |
| SX1262 TX a +17 dBm | 50 mA |
| SX1262 TX a +20 dBm | 85 mA |
| SX1262 TX a +22 dBm (max) | 120 mA |
| LED onboard encendido | ~3-5 mA |
| UART Serial activa | 1-2 mA |

### Time-on-air por payload (2 bytes de temperatura, CR=4/5, BW=125kHz)

| SF | Time-on-air | con RX1/RX2 windows (~200 ms + ~100 ms guard) | Total activo por ciclo |
|---|---|---|---|
| SF7 | 51.5 ms | +300 ms | **~352 ms** |
| SF8 | 92.7 ms | +300 ms | **~393 ms** |
| SF9 | 164.9 ms | +300 ms | **~465 ms** |
| SF10 | 329.7 ms | +300 ms | **~630 ms** |
| SF11 | 592.6 ms | +300 ms | **~893 ms** |
| SF12 | 1185.2 ms | +300 ms | **~1485 ms** |

> Los tiempos de RX window pueden ser mayores si el servidor manda MAC
> commands, ampliando el tiempo activo hasta ~1-2 segundos adicionales.

---

## 3. Presupuesto energético anual

### 3.1 Supuestos base

- **144 ciclos/día** (10 minutos entre reportes)
- **52,560 ciclos/año**
- **Unconfirmed uplinks** (sin esperar ACK de aplicación)
- **Payload**: 2 bytes (temperatura como `int16_t`)
- **Batería**: 1× 18650 Li-Ion, 3500 mAh nominales

### 3.2 Cálculo por SF y TX power

| SF | TX Power | Corriente TX | I × t por ciclo | Total TX/año | Sleep 3.5µA/año | **Total anual** | **Con autodescarga 2.5%/mes** |
|---|---|---|---|---|---|---|---|
| SF7 | +14 dBm | 40 mA | 0.0141 mA·s | 0.206 mAh | 30.7 mAh | **~31 mAh** | **~47 mAh** |
| SF7 | +20 dBm | 85 mA | 0.0299 mA·s | 0.437 mAh | 30.7 mAh | **~31 mAh** | **~48 mAh** |
| SF9 | +14 dBm | 40 mA | 0.0186 mA·s | 0.272 mAh | 30.7 mAh | **~31 mAh** | **~47 mAh** |
| SF9 | +20 dBm | 85 mA | 0.0395 mA·s | 0.577 mAh | 30.7 mAh | **~31 mAh** | **~48 mAh** |
| SF12 | +14 dBm | 40 mA | 0.0594 mA·s | 0.867 mAh | 30.7 mAh | **~32 mAh** | **~49 mAh** |
| SF12 | +20 dBm | 85 mA | 0.1262 mA·s | 1.842 mAh | 30.7 mAh | **~33 mAh** | **~51 mAh** |

> Estos cálculos asumen IDEAL: puro deep sleep entre ciclos, sin Serial,
> sin LED, sin ADR, sin re-joins, sin periféricos.

### 3.3 ¿36 mAh al año? La trampa del cálculo ideal

**En condiciones reales el consumo puede ser 20-50× mayor** por las siguientes razones:

| Factor | Cuánto suma |
|---|---|
| MCU despierta antes de TX (setup + medición sensor) | 3-5 mA durante ~500 ms → +0.42 µA·h/ciclo |
| ADC para leer sensor (DS18B20/I2C) | 2-3 mA durante ~100 ms |
| UART inicial (`Serial.begin`) y debug prints | 1-2 mA continuo si no se apaga |
| LED onboard (si no se apaga explícitamente) | 3-5 mA |
| Sesión LoRaWAN: `beginOTAA` sin persistencia → re-join | 40-120 mA durante 5-15 segundos |
| MAC commands forzados (ADR ACK, LinkCheck) | 40-85 mA × 300-500 ms con RX extendida |
| Auto-descarga de batería Li-Ion | 2-3% mensual de la capacidad nominal |
| Voltaje de cutoff (el CubeCell necesita >3.3V, la 18650 entrega hasta 2.85V) | Se pierde ~10-15% de capacidad |
| Efecto de temperatura (<10°C en exterior) | Se pierde 10-30% de capacidad |

### 3.4 Cálculo realista — sensor de temperatura con DS18B20

**Supuestos:**
- Unconfirmed uplink SF7 @ +14 dBm, RX1/RX2 activas
- DS18B20 leído en 750 ms (conversión de 12 bits) a 1.5 mA
- MCU activa configurando radio: 500 ms a 10 mA
- Sleep profundo real entre ciclos: 3.5 µA
- Serial DESHABILITADA entre ciclos
- LED APAGADO
- Sin ADR tras primer estabilizado
- Sin re-joins (sesión persistente)
- 1 MAC command downlink cada 64 ciclos (ADR_ACK_LIMIT)

| Componente | Energía por ciclo | Anualizado |
|---|---|---|
| TX SF7 @ 14 dBm, 52 ms × 40 mA | 2.08 mA·ms | 30.4 mA·h |
| RX1 + RX2, 300 ms × 10 mA | 3.00 mA·ms | 43.8 mA·h |
| MCU activa (config radio + sensor), 1250 ms × 10 mA | 12.5 mA·ms | 182.5 mA·h |
| DS18B20 conversión, 750 ms × 1.5 mA | 1.125 mA·ms | 16.4 mA·h |
| Sleep profundo, 9m 57s × 3.5 µA | 2.09 µA·min | 30.7 mA·h |
| MAC command adicional cada 64 ciclos | ~0.05 mA·ms/ciclo | 0.6 mA·h |
| **Total ideal** | | **~304 mA·h** |

Con autodescarga (2.5%/mes → 87.5 mA·h/mes en 3500 mAh) y derating de cutoff:

| Factor | Pérdida |
|---|---|
| Autodescarga 12 meses | ~1,050 mA·h |
| Voltaje de cutoff CubeCell (~3.15V bajo carga) | ~500 mA·h (último 15%) |
| Margen de seguridad (temperatura, picos TX) | ~200 mA·h |
| **Capacidad aprovechable real** | 3500 − 1050 − 500 − 200 = **~1,750 mA·h** |
| **Ciclo ideal anual** | ~304 mA·h |
| **Autonomía esperada** | **1,750 / 304 ≈ 5.75 años (sí, alcanza)** |

> Con SF7 cercano al gateway y las optimizaciones descritas, **1 año es
> perfectamente alcanzable**. El mismo cálculo en SF12 @ +20 dBm daría
> unos **1,600 mA·h** anuales, lo cual ya es justo para 1 año con
> una 18650 de buena calidad.

---

## 4. Recomendación de batería

### 4.1 ¿Qué celda 18650 elegir?

No todas las 18650 son iguales. La capacidad impresa puede estar inflada.
Celdas de marcas Chinas genéricas suelen entregar 1,200-2,200 mAh reales
(y con autodescarga acelerada).

| Modelo | Capacidad nominal | Capacidad real típica | Autodescarga | Precio aprox (CLP/USD) |
|---|---|---|---|---|
| **Samsung INR18650-35E** | 3,500 mAh | 3,350-3,450 mAh | Baja (~2%/mes) | ~$6 USD |
| **Panasonic NCR18650G** | 3,600 mAh | 3,450-3,550 mAh | Muy baja (~1.5%/mes) | ~$8 USD |
| **LG INR18650-MJ1** | 3,500 mAh | 3,350-3,450 mAh | Baja (~2%/mes) | ~$5 USD |
| **Sanyo NCR18650GA** | 3,500 mAh | 3,350-3,450 mAh | Baja (~2%/mes) | ~$6 USD |
| UltraFire / TrustFire (evitar) | "9,800 mAh" (falso) | 800-1,500 mAh | Muy alta | ~$2 USD |

### 4.2 Recomendación principal: Samsung INR18650-35E

**Mejor relación capacidad/precio/confiabilidad.** Características:

- **Química**: INR (LiNiCoMnO₂), estable, segura
- **Voltaje nominal**: 3.6-3.7V
- **Voltaje máximo**: 4.20V ± 0.05V
- **Voltaje de corte**: 2.65V (pero CubeCell se apaga a ~3.1V)
- **Corriente de descarga continua**: 5-8A (sobredimensionada para este uso)
- **Ciclos de vida**: 500+ al 80% de capacidad
- **Rango de temperatura**: -20°C a +60°C (carga), -30°C a +80°C (descarga)
- **Peso**: ~48g

> El CubeCell HTCC-AB01 tiene un LDO de 3.3V que necesita típicamente
> Vin > 3.4V (dropout ≈ 100-200 mV). Con una 18650 a 3.4V, el LDO
> entrega ~3.2V y el MCU sigue funcionando. A 3.2V de batería, el
> LDO baja a ~3.0V y el ASR6502 (rango 1.8-3.6V) sigue funcionando
> pero la potencia TX se degrada.

### 4.3 ¿Protegida o no protegida?

| Tipo | Pros | Contras |
|---|---|---|
| **No protegida** (flat top) | Cabe en el portapilas del CubeCell; más barata; menor resistencia interna | Sin protección contra sobrecarga/sobredescarga/cortocircuito |
| **Protegida** (button top con PCB) | Protección BMS integrada; más segura para prototipos | 2-3 mm más larga → puede no caber; mayor autodescarga por el BMS (~3-5 µA adicionales); un poco más cara |

**Recomendación para prototipo**: No protegida + supervisión por código
(lectura ADC del voltaje de batería). El CubeCell no tiene cargador
integrado, así que la carga se hace externamente con un módulo TP4056.

### 4.4 Alternativa para >2 años: Li-SOCl₂ tipo ER34615 (celda D)

Si necesitas **más de 2 años sin mantenimiento**, considera una celda
primaria de Cloruro de Tionilo de Litio:

| Parámetro | ER34615 (Li-SOCl₂) | Samsung 35E (Li-Ion) |
|---|---|---|
| Capacidad nominal | 19,000 mAh | 3,500 mAh |
| Voltaje nominal | 3.6V | 3.7V |
| Autodescarga anual | <1% | ~25-30% |
| Recargable | No | Sí |
| Temperatura operación | -55°C a +85°C | -20°C a +60°C |
| Precio aprox | $15-20 USD | $6 USD |

> La CubeCell tiene conector JST-PH 2.0 para batería. El pin Vext/BAT
> soporta 3.3-5.5V. Una ER34615 con portapilas D externo soldado a un
> conector JST da **+10 años** teóricos con ciclo de 10 minutos.

---

## 5. Optimizaciones de código en CubeCell

### 5.1 Sleep profundo real — no busy loops

```cpp
// ❌ MAL: Busy loop quema batería
void sleepSeconds(int s) {
    uint32_t start = millis();
    while ((millis() - start) < s * 1000) { /* vacío pero MCU a 12 mA */ }
}

// ✅ BIEN: Deep sleep del ASR6502
#include <Arduino.h>
void deepSleepSeconds(uint32_t seconds) {
    // Apagar todo antes
    Serial.end();
    pinMode(LED, OUTPUT);
    digitalWrite(LED, LOW);
    
    // Poner el timer del RTC para despertar en N segundos
    // RadioLib se encarga de reinicializar la radio al despertar
    Radio.Sleep();
    delay(100);
    
    // ASR6502 low power mode
    // El CubeCell se despierta por RTC timer o pin externo
    board.lowPower(seconds * 1000);  // milisegundos
}
```

### 5.2 Deshabilitar Serial entre ciclos

El UART del ASR6502 consume 1-2 mA incluso en idle. Si no necesitas
comunicación constante por Serial:

```cpp
void loop() {
    // Encender Serial solo para debug si se necesita
    #ifdef DEBUG
    Serial.begin(115200);
    #endif

    leerSensor();
    enviarLoRaWAN();
    
    #ifdef DEBUG
    Serial.flush();
    Serial.end();
    #endif

    deepSleepSeconds(600);  // 10 minutos
}
```

### 5.3 Apagar periféricos en sleep

```cpp
// LED onboard: muchas CubeCell usan GPIO como LED
#define LED_BUILTIN 35   // HTCC-AB01 usa GPIO35 para LED
pinMode(LED_BUILTIN, OUTPUT);
digitalWrite(LED_BUILTIN, LOW);

// Si usas I2C para el sensor, dormir el bus
// Wire.end();

// Para DS18B20 OneWire: apagar GPIO de power si usas parasitic power
```

### 5.4 Sensor power gating con GPIO

Alimenta el sensor desde un pin GPIO para cortarlo completamente en sleep:

```cpp
#define SENSOR_POWER_PIN GPIO5  // Vext o pin libre

void leerSensor() {
    digitalWrite(SENSOR_POWER_PIN, HIGH);
    delay(10);  // tiempo de estabilización del sensor
    
    // Leer temperatura...
    
    digitalWrite(SENSOR_POWER_PIN, LOW);
}
```

> No todos los pines GPIO entregan suficiente corriente. Si tu sensor
> necesita >10 mA, usa un MOSFET P-channel controlado por GPIO o el
> pin Vext del CubeCell (controlable por software, entrega hasta 150 mA).

### 5.5 Unconfirmed uplinks

```cpp
// Confirmed: el dispositivo espera ACK del servidor → abre RX y puede
// reintentar hasta 8 veces.
// Unconfirmed: envía y solo abre RX1/RX2 lo justo para MAC commands.

int state = node.sendReceive(
    payload, payloadLen,
    1,                    // fPort (1-223 para datos de app)
    downlinkBuffer, &downlinkLen,
    false,                // ← confirmed = false (unconfirmed)
    NULL, NULL
);
```

### 5.6 Sintonizar ADR

```cpp
// Si el gateway está cerca (<2 km) y la señal es estable,
// deshabilitar ADR después del primer join estable.

// Opción 1: Deshabilitar completamente
// node.setADR(false);  // según API de RadioLib

// Opción 2: No setear el bit ADR en el uplink
// RadioLib lo maneja vía LoRaWANNode::setADR()
```

### 5.7 Persistir sesión para evitar re-joins

Cada re-join cuesta 5-15 segundos de TX a máxima potencia (120 mA típicos).

```cpp
// Guardar DevAddr, NwkSKey, AppSKey en flash/EEPROM
// y restaurarlos con node.restoreSession()

// Ejemplo conceptual con RadioLib:
uint8_t devAddr[4];
uint8_t nwkSKey[16];
uint8_t appSKey[16];

// node.getDevAddr(devAddr);
// node.getNwkSKey(nwkSKey);
// node.getAppSKey(appSKey);
// Guardar en flash...

// En setup():
// if (sessionSavedInFlash()) {
//     node.beginABP(devAddr, nwkSKey, appSKey);
//     // No hace join por OTAA, ahorra energía
// }
```

### 5.8 Medir voltaje de batería

```cpp
float leerVoltajeBateria() {
    // CubeCell tiene divisor interno en pin ADC
    // Para HTCC-AB01, el ADC mide VBAT / 0.232
    // (varía según revisión de hardware, verificar con multímetro)
    uint16_t raw = analogRead(ADC_INTERNAL_BAT);
    float voltage = raw * (3.3 / 4096.0) * (1.0 / 0.223);
    return voltage;
}
```

### 5.9 Checklist final de optimizaciones de código

| Optimización | Ahorro estimado |
|---|---|
| Deep sleep real (`board.lowPower`) vs `delay()` | 12 mA mientras dormido → 3.5 µA |
| `Serial.end()` entre ciclos | 1-2 mA continuo → 0 |
| Apagar LED | 3-5 mA continuo → 0 |
| Sensor power gating | 1-3 mA en espera → 0 |
| Unconfirmed en vez de confirmed | Evita 1-8 reintentos TX + RX extendida |
| Deshabilitar ADR en señal estable | Elimina tráfico MAC innecesario |
| Persistir sesión | Evita re-joins de 15s × 120 mA |
| Payload mínimo (2 bytes) | Reduce time-on-air ~20-30% vs 51 bytes |

---

## 6. Optimizaciones del lado del Network Server

### 6.1 ChirpStack / ChirpStack v4

En el **Device Profile** de tu dispositivo:

| Parámetro | Valor recomendado | Razón |
|---|---|---|
| Supports Class B | No | Solo necesitas Clase A |
| Supports Class C | No | Idem |
| Mac version | LoRaWAN 1.0.3 | Compatible con RadioLib |
| RX1 delay | 1 (1 segundo) | El mínimo que permite la spec |
| RX1 data-rate offset | 0 | Mismo DR que uplink |
| RX2 data-rate | DR8 (SF12, 500 kHz para AU915) | Máximo alcance para downlink |
| RX2 frequency | 923.3 MHz (AU915) | Default regional |
| ADR allowed | Enabled (pero con límites agresivos) | Ver sección 6.2 |
| Max EIRP | 14 | Limitar potencia máxima TX |
| Flush queue on activate | False | Evitar re-joins innecesarios |

### 6.2 Configuración agresiva de ADR en ChirpStack

En el device-profile, sección ADR:

```
min_dr: 4      → DR4 = SF7/125kHz (el más eficiente)
max_dr: 5      → DR5 = SF7/125kHz (no permitir SF8+)
min_tx_power: 0 → 14 dBm
max_tx_power: 0 → 14 dBm (no permitir subir potencia)
```

Esto fuerza que el servidor mantenga al dispositivo en el SF más bajo
y la potencia más baja, maximizando eficiencia energética. **Solo hazlo
si el gateway está a <2 km urbano.**

### 6.3 The Things Network (TTN/TTI)

En TTN Console → Application → Device → Settings:

- **Class**: A
- **Activation**: OTAA
- **Frame counter width**: 16 bit
- **RX1 delay**: 1
- **RX2 data rate**: SF12 / BW500
- Desmarcar "Confirmed downlink" para ahorrar energía

### 6.4 Duty cycle regional (AU915)

AU915 (Australia) no tiene duty cycle de sub-banda estricto como EU868.
Sin embargo, si por alguna razón configuras EU868/US915:

- EU868: 1% duty cycle → máximo 36 segundos de TX por hora
- A 10-min interval con SF7 (52 ms TX), envías 144 × 52 ms = 7.5s/hora
  → 7.5s << 36s, holgadamente dentro del límite

---

## 7. Ejemplo práctico: esqueleto de sensor de temperatura 1 año

```cpp
/*
 * CubeCell_TempSensor_UltraLowPower.ino
 * Sensor DS18B20, reporte c/10min, target 1 año con 18650
 */

#include <RadioLib.h>
#include <OneWire.h>
#include <DallasTemperature.h>

/* --- Pines --- */
#define ONE_WIRE_PIN   GPIO5
#define SENSOR_PWR     GPIO4    // power gating del sensor

/* --- Credenciales --- */
uint64_t joinEui = 0x24E124C0002ADD01;
uint64_t devEui  = 0x24F124725F486022;
uint8_t appKey[] = { /* ... */ };
uint8_t nwkKey[] = { /* ... */ };

/* --- Config --- */
const LoRaWANBand_t Region = AU915;
const uint8_t subBand = 2;
#define SLEEP_SECONDS 600      // 10 minutos
#define BATTERY_CHECK true     // incluir voltaje batería en payload

/* --- Buffer payload --- */
const uint8_t fPort = 10;
uint8_t payload[4]; // temp(2bytes) + bat(2bytes)
uint8_t downlink[16];
size_t downlinkLen;

Module mod(35, 38, 47, 39);
SX1262 radio(&mod);
LoRaWANNode node(&radio, &Region, subBand);
OneWire oneWire(ONE_WIRE_PIN);
DallasTemperature sensors(&oneWire);

static int16_t leerTemperatura() {
    digitalWrite(SENSOR_PWR, HIGH);
    delay(10);
    sensors.begin();
    sensors.requestTemperatures();
    float tempC = sensors.getTempCByIndex(0);
    
    // Si falla el sensor (DS18B20 devuelve -127)
    if (tempC <= -100) tempC = 0;
    
    digitalWrite(SENSOR_PWR, LOW);
    return (int16_t)(tempC * 10);  // 25.7°C → 257
}

static uint16_t leerBateria() {
    uint16_t raw = analogRead(ADC);
    // Factor de divisor para CubeCell HTCC-AB01
    float v = raw * (3.3 / 4096.0) * (1.0 / 0.223);
    return (uint16_t)(v * 100);  // 3.85V → 385
}

void setup() {
    // Mínimo absoluto: sin Serial, sin LED
    pinMode(LED, OUTPUT);
    digitalWrite(LED, LOW);
    pinMode(SENSOR_PWR, OUTPUT);
    digitalWrite(SENSOR_PWR, LOW);

    // Radio
    radio.begin();
    node.beginOTAA(joinEui, devEui, nwkKey, appKey);
    node.activateOTAA();
    
    // ADR deshabilitado (gateway cercano)
    // node.setADR(false);
}

void loop() {
    if (!node.isActivated()) {
        node.activateOTAA();
        if (!node.isActivated()) {
            // Esperar y reintentar
            radio.sleep();
            board.lowPower(60000); // 1 minuto de sleep, reintentar
            return;
        }
    }

    // Medir
    int16_t temp = leerTemperatura();
    uint16_t bat = leerBateria();

    // Preparar payload
    payload[0] = (temp >> 8) & 0xFF;
    payload[1] = temp & 0xFF;
    payload[2] = (bat >> 8) & 0xFF;
    payload[3] = bat & 0xFF;

    // Enviar (unconfirmed)
    downlinkLen = sizeof(downlink);
    node.sendReceive(payload, sizeof(payload), fPort,
                     downlink, &downlinkLen, false);

    // Dormir profundamente
    radio.sleep();
    board.lowPower(SLEEP_SECONDS * 1000);
}
```

### Autonomía esperada para este sketch (con Samsung 35E):

| Condición | Meses estimados |
|---|---|
| SF7, +14 dBm, gateway a <500m | **70+ meses (~6 años)** |
| SF7, +17 dBm, gateway a <1 km | **60+ meses (~5 años)** |
| SF8, +14 dBm, gateway a <2 km | **55+ meses** |
| SF9, +17 dBm, gateway a <5 km | **40+ meses** |
| SF12, +20 dBm, gateway a >10 km (rural) | **~18 meses** |

---

## 8. Tabla resumen de decisión

| ¿Qué quiero? | Configuración recomendada | Batería | Autonomía esperada |
|---|---|---|---|
| **1 año mínimo, gateway cerca** (<2 km) | SF7/8, 14 dBm, unconfirmed, serial off, sensor gating, sin ADR | Samsung 35E | **4-6 años** |
| **1 año, gateway lejos** (>5 km urbano) | ADR activo con límite SF10 max, 17 dBm, unconfirmed | Samsung 35E | **1.5-2 años** |
| **Máxima autonomía absoluta** (>5 años) | SF7 fijo, 14 dBm, unconfirmed, + Li-SOCl₂ ER34615 | ER34615 19,000 mAh | **10-15 años** |
| **Prototipo rápido, sin optimizar** | Config por defecto, ADR activo, confirmed, Serial ON, LED ON | Samsung 35E | **3-6 meses** |

---

## 9. Verificación con multímetro

Para validar que el sleep profundo realmente funciona:

1. Colocar el multímetro en serie entre la batería y el CubeCell (modo µA/mA).
2. Observar la corriente en sleep: debe ser **<10 µA**.
3. Si mide >100 µA, algo no está durmiendo (Serial, LED, sensor, LDO auxiliar).
4. Medir el pico de TX: debe corresponder a la tabla de la sección 2.
5. Usar un osciloscopio o logger de corriente (INA219/Joulescope) para ver el
   perfil completo: sleep → wake → sense → TX → RX → sleep.

---

*Documento generado para CubeCell HTCC-AB01 con RadioLib. Ajustar pines y
parámetros según la revisión de hardware específica. Verificar consumo real
con instrumento antes de desplegar en campo.*
