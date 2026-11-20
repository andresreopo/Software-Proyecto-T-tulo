"""Parámetros del GPS (ATGM366H por UART): puerto, velocidad y criterios de calidad del fix."""

GPS_PORT = "/dev/serial0"     # UART de la Raspberry Pi; en Windows/otros usa --gps-port o SATTRACK_GPS_PORT
GPS_BAUD = 9600             # ATGM366H: 9600 baud de fábrica
GPS_RETRY_S = 3.0           # reintento si el puerto no abre o se cae

# Un fix se acepta como posición del observador solo si cumple TODO esto
GPS_MIN_SATS = 4            # satélites usados (GGA)
GPS_MAX_HDOP = 5.0          # HDOP máximo (mayor = geometría pobre)
GPS_MAX_AGE_S = 10.0        # sin un fix bueno durante este tiempo → se considera perdido
GPS_RX_STALE_S = 5.0        # sin ninguna sentencia válida durante este tiempo → "sin datos del módulo"

# Histéresis: el GPS "tiembla" unos metros aun estando quieto. La posición informada solo cambia
# cuando el receptor se aleja más de esto de la posición anterior (evita recalcular pases sin motivo).
GPS_MOVE_M = 50.0

# Posición del puerto simulado "sim" (sin hardware): Santiago
GPS_SIM_POS = (-33.4489, -70.6693, 570.0)
GPS_SIM_NOFIX_S = 4.0       # el simulador tarda esto en "adquirir" el fix
