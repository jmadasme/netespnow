# Solución de Problemas

## 1. No se detecta la placa (port not found)

```bash
# Ver todos los puertos serie disponibles
arduino-cli board list

# También verificar manualmente
ls -la /dev/ttyUSB* /dev/ttyACM*
```

**Posibles causas**:
- Cable USB defectuoso → probar otro cable
- Placa no alimentada → verificar LED de encendido
- Driver CP2102 no instalado → `lsmod | grep cp210x`
- Puerto ocupado por otra aplicación → cerrar CuteCom, Arduino IDE, etc.

## 2. Error de permisos al grabar

```
[ERROR] Could not open port /dev/ttyUSB0: Permission denied
```

**Solución**:
```bash
# Agregar usuario al grupo dialout
sudo usermod -a -G dialout $USER

# Aplicar cambios (cerrar sesión o ejecutar:)
newgrp dialout
```

## 3. Error al compilar: "espnow.h: No such file or directory"

**Causa**: El core ESP8266 no está instalado o está desactualizado.

**Solución**:
```bash
arduino-cli core update-index
arduino-cli core install esp8266:esp8266
```

## 4. El bridge no responde a comandos seriales

**Síntoma**: El monitor serial no muestra nada al escribir comandos.

**Posibles causas**:
- Baudrate incorrecto → debe ser **115200**
- Puerto incorrecto → verificar con `arduino-cli board list`
- CuteCom en modo RAW vs línea → debe terminar con Enter (LF o CRLF)
- El bridge se colgó → presionar RESET o reconectar USB

## 5. Un nodo no responde al ping

**Síntoma**: 
```
[ENVIO] Request -> Nodo X [MAC]
[TIMEOUT] Nodo X - No se recibio respuesta (1001 ms)
```

**Verificaciones**:
1. ¿El nodo está encendido? → Verificar LED heartbeat (parpadea cada 1s)
2. ¿El nodo tiene el ID correcto? → Verificar mensaje de inicio
3. ¿El bridge conoce la MAC? → Ejecutar `list`
4. ¿El nodo está dentro del alcance? → Acercar al bridge
5. ¿El nodo se programó correctamente? → Reconectar y verificar

**Solución rápida**: Desconectar y reconectar el nodo (reinicia el firmware).

## 6. Timeout en broadcast (nodo no conocido)

**Síntoma**:
```
[ENVIO] Request broadcast para nodo X
[TIMEOUT] Nodo X - No se recibio respuesta
```

**Explicación**: El bridge no conoce la MAC del nodo y envía por broadcast. Si el nodo no responde al broadcast, puede ser porque:
- El nodo no existe físicamente con ese ID
- El nodo está apagado
- El nodo tiene otro ID en su firmware

## 7. El monitor serial muestra estadísticas constantemente

**Solución**: Escribir `auto off` para detener las estadísticas automáticas.

## 8. Error "Fallo al enviar request"

**Síntoma**:
```
[ERROR] Fallo al enviar request a nodo X (codigo=XXX)
```

**Causa**: Error interno de ESP-NOW. Generalmente ocurre si:
- El peer no está agregado correctamente (código de error 1)
- El buffer de ESP-NOW está lleno

**Solución**: El bridge reintenta automáticamente en el siguiente ciclo.

## 9. El bridge se reinicia solo

**Posibles causas**:
- Watchdog timer (WDT) por loop bloqueante
- Bajo voltaje en la alimentación (usar cable USB de calidad)
- Interferencia electromagnética

## 10. MAC address desconocida

**Síntoma**: 
```
Nodo X: DESCONOCIDO [00:00:00:00:00:00]
```

**Causa**: El bridge nunca ha recibido una respuesta de ese nodo.

**Solución**: 
1. Verificar que el nodo existe y está encendido
2. Ejecutar `ping X` (forzará broadcast y autodescubrimiento)
3. Si no aparece, el nodo tiene un ID diferente

## 11. Dos nodos tienen la misma MAC

**Causa**: Imposible — cada chip ESP8266 tiene una MAC única grabada en fábrica. Si ves dos nodos con la misma MAC, es error en la tabla o confusión de IDs.

## 12. El LED del nodo no parpadea

**Causa probable**: El firmware no se grabó correctamente.

**Solución**:
1. Reconectar el nodo al PC
2. Recompilar y subir el firmware
3. Verificar el mensaje de inicio en el monitor serial

## 13. El ESP32 no funciona (nota histórica)

Si intentaste usar un ESP32 y viste:
```
E (57) flash_parts: partition 0 invalid magic number 0x1389
Failed to verify partition table
```

Este error ocurre porque el bootloader del core ESP32 3.3.x es incompatible con ciertas revisiones de chip (v3.1). La solución fue migrar toda la red a ESP8266 NodeMCU v3, que funciona de forma estable.

## 14. Ayuda adicional

Si el problema persiste después de verificar todos los puntos anteriores:
1. Ejecutar `help` en el bridge para ver los comandos disponibles
2. Ejecutar `stats` para ver el estado de la red
3. Ejecutar `list` para ver la tabla de nodos
4. Verificar que todos los nodos están encendidos y dentro del alcance
