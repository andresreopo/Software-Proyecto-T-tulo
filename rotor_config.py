"""Parámetros del rotor y del enlace serial con la ESP32 (único archivo a tocar para tu mecanismo)."""

SERIAL_BAUD    = 115200   # debe coincidir con BAUD en firmware/esp32_rotor/esp32_rotor.ino
SERIAL_RETRY_S = 2.0      # reintento si el puerto se cae tras haber conectado
SERIAL_STALE_S = 2.0      # sin "POS" durante este tiempo → posición obsoleta

# Límites aceptados para las referencias enviadas al rotor (°)
ROTOR_AZ_MAX = 360.0
ROTOR_EL_MIN = 0.0
ROTOR_EL_MAX = 90.0

# Velocidad máxima que puede alcanzar TU rotor (°/s). Solo se usa para avisar cuando un pase la supera
# (cerca del cenit el azimut gira muy rápido) y en el simulador de ESP32 (puerto "sim").
ROTOR_MAX_RATE_AZ = 12.0
ROTOR_MAX_RATE_EL = 6.0

TRACK_RATE_HZ = 5.0       # frecuencia con que se envían las referencias TRK
BAT_STALE_S    = 5.0      # sin "BAT" durante este tiempo → voltaje obsoleto (la ESP32 lo envía cada 1 s)
