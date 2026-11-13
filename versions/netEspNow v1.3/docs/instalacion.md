# Instalación — netEspNow v1.3

## Requisitos del Sistema

```bash
# Instalar arduino-cli (si no está)
curl -fsSL https://raw.githubusercontent.com/arduino/arduino-cli/master/install.sh | sh

# Instalar core ESP8266
arduino-cli core install esp8266:esp8266

# Instalar librería micro-ecc (cifrado ECC)
arduino-cli lib install "micro-ecc"

# Verificar instalación
arduino-cli core list
arduino-cli lib list | grep micro-ecc
```

## Compilación

> **Nota v1.3.1**: Bridge y Puerta incluyen `bearssl/bearssl.h` directamente.
> Ya no se requiere el flag `--build-property`. FQBN unificado: `esp8266:esp8266:generic`.

```bash
# Puerta de Enlace
arduino-cli compile --fqbn esp8266:esp8266:generic firmware_puerta_enlace/

# Bridge
arduino-cli compile --fqbn esp8266:esp8266:generic firmware_bridge/

# Nodos (plantilla base I/O)
for id in 0 1 2 3 4; do
  arduino-cli compile --fqbn esp8266:esp8266:generic firmware_nodo_${id}/
done
```

## Subida a las Placas

### 1. Puerta de Enlace
```bash
arduino-cli upload --fqbn esp8266:esp8266:generic \
  --port /dev/ttyUSB1 firmware_puerta_enlace/
```

### 2. Bridge
```bash
arduino-cli upload --fqbn esp8266:esp8266:generic \
  --port /dev/ttyUSB0 firmware_bridge/
```

### 3. Nodos (cada uno por separado)
```bash
arduino-cli upload --fqbn esp8266:esp8266:generic \
  --port /dev/ttyUSB2 firmware_nodo_0/

arduino-cli upload --fqbn esp8266:esp8266:generic \
  --port /dev/ttyUSB3 firmware_nodo_1/
# ... etc
```

## Puesta en Marcha

1. Conectar **Puerta de Enlace** a alimentación USB (`/dev/ttyUSB1`)
2. Conectar **Bridge** a PC vía USB (`/dev/ttyUSB0`)
3. Abrir monitor serial (CuteCom recomendado):
   - Puerto: `/dev/ttyUSB0`
   - Baudrate: `115200`
   - CR/LF al enviar
4. (Opcional) Restaurar claves ECC de fábrica:
   ```
   keygen
   ```
5. Verificar estado:
   ```
   list
   ```
6. Probar consulta y actuación:
   ```
   ping 1
   set 1 on
   ```

> **Validación v1.3 con multímetro**: tras `set 0 on`, medir 3.3 V entre D1 y GND del nodo.

## Errores Comunes

| Error | Causa | Solución |
|-------|-------|----------|
| `esp_now_init() falló` | WiFi no inicializado | Resetear el dispositivo |
| `[SEGURIDAD] Firma INVALIDA` | Claves no coinciden | Regenerar claves con `keygen` |
| `[TIMEOUT] Puerta de Enlace` | Puerta apagada o fuera de alcance | Verificar alimentación y distancia |
| `[TIMEOUT] nodo sin respuesta` | Nodo apagado, sin flashear o fuera de alcance | Flashear el nodo, verificar alimentación |
| No aparece el puerto USB | Driver CP2102 no instalado | `sudo apt install cp210x-dkms` |
| Error de compilación: uECC no encontrado | Librería no instalada | `arduino-cli lib install "micro-ecc"` || Watchdog reinicia el Bridge | 10 timeouts consecutivos | Conectar/flashear nodos, o usar `auto off` |

---

*Instalación netEspNow v1.3 — Julio 2026*
