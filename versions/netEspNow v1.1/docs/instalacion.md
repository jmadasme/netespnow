# Instalación y Compilación

## Requisitos del Sistema

### 1. Instalar arduino-cli

```bash
# Descargar la última versión
curl -fsSL https://raw.githubusercontent.com/arduino/arduino-cli/master/install.sh | sh

# O instalar desde paquete (Linux)
sudo apt install arduino-cli
```

### 2. Configurar arduino-cli

```bash
# Generar archivo de configuración
arduino-cli config init

# Agregar URLs de cores adicionales
arduino-cli config add board_manager.additional_urls \
  https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json \
  https://arduino.esp8266.com/stable/package_esp8266com_index.json
```

### 3. Instalar el core ESP8266

```bash
arduino-cli core update-index
arduino-cli core install esp8266:esp8266
```

Verificar instalación:
```bash
arduino-cli core list
```
Debe aparecer: `esp8266:esp8266`

### 4. Verificar la placa conectada

```bash
arduino-cli board list
```

La salida debe mostrar algo como:
```
Port         Protocol  Type              Board Name  FQBN
/dev/ttyUSB0 serial    Serial Port       Unknown     esp8266:esp8266:nodemcuv2
```

---

## Compilar y Subir Firmware

### Nodo Sensor (ej: Nodo 0)

```bash
# Compilar
arduino-cli compile \
  --fqbn esp8266:esp8266:nodemcuv2 \
  "netEspNow v1.1/firmware_nodo_0/"

# Subir a la placa
arduino-cli upload \
  -p /dev/ttyUSB0 \
  --fqbn esp8266:esp8266:nodemcuv2 \
  "netEspNow v1.1/firmware_nodo_0/"
```

### Bridge

```bash
# Compilar
arduino-cli compile \
  --fqbn esp8266:esp8266:nodemcuv2 \
  "netEspNow v1.1/firmware_bridge/"

# Subir
arduino-cli upload \
  -p /dev/ttyUSB0 \
  --fqbn esp8266:esp8266:nodemcuv2 \
  "netEspNow v1.1/firmware_bridge/"
```

### Script de Compilación Rápida

Guarda esto como `compilar.sh`:

```bash
#!/bin/bash
# Uso: ./compilar.sh nodo 0  /dev/ttyUSB0
#      ./compilar.sh bridge /dev/ttyUSB0

TIPO=$1
ID=$2
PUERTO=${3:-/dev/ttyUSB0}

if [ "$TIPO" = "bridge" ]; then
  DIR="netEspNow v1.1/firmware_bridge"
elif [ "$TIPO" = "nodo" ]; then
  DIR="netEspNow v1.1/firmware_nodo_$ID"
else
  echo "Uso: $0 {nodo|bridge} [id] [puerto]"
  exit 1
fi

arduino-cli compile --fqbn esp8266:esp8266:nodemcuv2 "$DIR" && \
arduino-cli upload -p "$PUERTO" --fqbn esp8266:esp8266:nodemcuv2 "$DIR"
```

---

## Solución de Errores Comunes

### "Port not found"
```bash
# Listar puertos disponibles
ls -la /dev/ttyUSB* /dev/ttyACM*
```

### "Permission denied"
```bash
# Agregar usuario al grupo dialout
sudo usermod -a -G dialout $USER
# Cerrar sesión y volver a entrar, o ejecutar:
newgrp dialout
```

### "No such file or directory"
```bash
# Verificar que la ruta existe
ls -la "netEspNow v1.1/firmware_nodo_0/"
```

### Error de compilación: librería faltante
```bash
# El core ESP8266 ya incluye ESP8266WiFi.h y espnow.h
# No se requieren librerías adicionales
```

### Error de subida: chip no responde
```bash
# El NodeMCU v3 a veces requiere mantener presionado el flash button
# durante la conexión. También probar:
# - Desconectar y reconectar USB
# - Usar otro cable USB
# - Verificar que el puerto no está ocupado (CuteCom cerrado)
```
