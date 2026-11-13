# Instalación — netEspNow v1.2

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

### Puerta de Enlace
```bash
arduino-cli compile --fqbn esp8266:esp8266:nodemcuv2 \
  firmware_puerta_enlace/
```

### Bridge
```bash
arduino-cli compile --fqbn esp8266:esp8266:nodemcuv2 \
  firmware_bridge/
```

### Nodos
```bash
for id in 0 1 2 3 4; do
  arduino-cli compile --fqbn esp8266:esp8266:nodemcuv2 \
    firmware_nodo_${id}/
done
```

## Subida a las Placas

### 1. Puerta de Enlace (conectada a /dev/ttyUSB0)
```bash
arduino-cli upload --fqbn esp8266:esp8266:nodemcuv2 \
  --port /dev/ttyUSB0 firmware_puerta_enlace/
```

### 2. Bridge (cuando esté conectado)
```bash
# Desconectar Puerta de Enlace, conectar Bridge
arduino-cli upload --fqbn esp8266:esp8266:nodemcuv2 \
  --port /dev/ttyUSB0 firmware_bridge/
```

### 3. Nodos (cada uno por separado)
```bash
arduino-cli upload --fqbn esp8266:esp8266:nodemcuv2 \
  --port /dev/ttyUSB0 firmware_nodo_0/

arduino-cli upload --fqbn esp8266:esp8266:nodemcuv2 \
  --port /dev/ttyUSB0 firmware_nodo_1/
# ... etc
```

## Puesta en Marcha

1. Conectar **Puerta de Enlace** a alimentación USB
2. Conectar **Bridge** a PC vía USB
3. Abrir monitor serial (CuteCom recomendado):
   - Puerto: `/dev/ttyUSB0`
   - Baudrate: `115200`
   - CR/LF al enviar
4. En el monitor, generar claves ECC:
   ```
   keygen
   ```
5. Verificar estado:
   ```
   list
   ```
6. Probar conexión:
   ```
   ping 0
   ```

## Errores Comunes

| Error | Causa | Solución |
|-------|-------|----------|
| `esp_now_init() falló` | WiFi no inicializado | Resetear el dispositivo |
| `[SEGURIDAD] Firma INVALIDA` | Claves no coinciden | Regenerar claves con `keygen` |
| `[TIMEOUT] Puerta de Enlace` | Puerta apagada o fuera de alcance | Verificar alimentación y distancia |
| No aparece el puerto USB | Driver CP2102 no instalado | `sudo apt install cp210x-dkms` |
| Error de compilación: uECC no encontrado | Librería no instalada | `arduino-cli lib install "micro-ecc"` |
