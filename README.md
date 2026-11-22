# Comentarios de versión
# SATTRACK
v17:
- Se agregan botones para envío de comandos

V16:
- Cambios de distribución visual en interfaz

V15:
- Puerto GPS actualizado
- IP de inicio de interfaz actualizada, permite conexión local a través de otro computador
V14:
- Arreglo de bug de envío de STOP en modo TRK

V13:
- Filtro de pases por sector de azimut de 0° a 360°
- Nombre del satélite en seguimiento en Monitor RF
- Grabación de cuaterniones de la BNO085 en CSV desde Seguimiento

V12:
- Filtro de banda L añadido
- Incorporación de barrido en frecuencia

V11:
- Fix de filtro promedio movil
- Incorporación de rutina de calibración y reporte de accuracy magnetómetro

V10:
- Incorporación de filtro por horario de pase para planificación de observaciones
- Incorporación de terminal serial para envío manual de comandos
- Slider para modificar frecuencia central en monitor RF
- Visualización de frecuencia donde se posiciona el cursor
- Selección automática de frecuencia central al elegir un satélite de la lista
- Incorporación de filtro de promedio móvil para monitor RF

V9: 
- Incorporación de lectura GPS para posicionamiento automático
- Corrección de errores de visualización en escalas de Monitor RF

V8: 
- Ancho de banda y muestreo editables
- Aumento de tamaño en elementos de interfaz
- Adaptación automática de rangos de potencia recibida en Monitor RF

V7:
- Aumento de tamaño de fuentes

V6:
- Cambio en la paleta de colores a una con mayor contraste
- Simplificación de las interacciones para que se pueda manejar solo con mouse sin teclado
- Aumento en el tamaño de la interfaz para una mejor visualización en pantalla de 7 pulgadas
- Incorporación de mejoras basadas en Satdump

V5: 
- Se separó la interfaz en 3 pestañas para visualizar en pantalla de 7 pulgadas. 
- Se incorpora conexión real a pluto y se añade el script ```mock_maia.py``` que permite simular una pluto de forma externa.
- Se añade un mapa para visualización de satélite y observador respecto al espacio aéreo chileno.

# FIRMWARE
V7:
- Limit switch azimutal añadido

V6: 
- Arreglo de firmware por largo de cables, disminuye freq i2c
- Arreglo de bug de inicialización de BNO, watchdog añadido 

V5: 
- Se añade rutina de calibración para BNO

V4: 
- Se corrigen errores asociados al inicio del BNO

V3: 
- Correcciones a vector de elevación

# SATTRACK (simulación + monitor RF Maia)

Aplicación web única para seleccionar un satélite del catálogo orbital, elegir una bajada de banda S, iniciar el próximo pase y observar el seguimiento y la recepción simulados. Combina el flujo del selector de satélites con las vistas de seguimiento y espectro de MaiaView. Los proyectos originales permanecen separados.

## Iniciar en Windows

Desde PowerShell, en esta carpeta:

```powershell
python -m venv .venv
.\.venv\Scripts\python.exe -m pip install -r requirements.txt
.\.venv\Scripts\python.exe app.py --refresh-on-start
```

Abrir [http://127.0.0.1:5003/](http://127.0.0.1:5003/). Detener el servidor con `Ctrl+C`. Si `.venv` ya existe, omitir los dos primeros comandos. Requiere Python 3.10 o posterior e Internet para instalar dependencias y actualizar catálogos. Las copias locales de los catálogos permiten abrir la interfaz sin Internet; la fecha de cada fuente se muestra en pantalla.

El servidor escucha solo en `127.0.0.1` por defecto. Para acceder desde otro equipo de la misma red, iniciarlo con `--host 0.0.0.0` y abrir `http://IP_DEL_EQUIPO:5003/`, sujeto al firewall local. El monitor RF se conecta a la Pluto (Maia) y el rotor puede conectarse a una ESP32 por puerto serial: con `--host 0.0.0.0` cualquier equipo de la red podría enviar comandos al rotor y cambiar la sintonía, así que úsalo solo en una red de confianza.

## Arranque automático en Raspberry Pi

Desde una terminal en la Raspberry Pi, entra a la copia local del repositorio. Usa el mismo usuario con el que abres el escritorio y confirma que SATTRACK funciona manualmente. Detén esa instancia antes de instalar el servicio para liberar el puerto 5003.

```bash
bash deploy/raspberry-pi/install.sh
```

El instalador configura el servicio `sattrack.service` y el inicio automático de Chromium en modo quiosco en Raspberry Pi OS con `labwc`. Chromium espera a que la interfaz responda en `http://127.0.0.1:5003/`. Puedes volver a ejecutar el instalador después de actualizar estos scripts.

Si el Python que usas está en otro entorno, actívalo antes de instalar o indica su ruta absoluta con `SATTRACK_PYTHON=/ruta/al/entorno/bin/python bash deploy/raspberry-pi/install.sh`. Si ya arrancas `app.py` con opciones para la Pluto, el GPS o la ESP32, guarda tus opciones en `/etc/default/sattrack` antes de ejecutar el instalador. Ajusta los puertos y la IP del ejemplo a tu equipo.

```ini
SATTRACK_ARGS="--gps-port /dev/ttyAMA0 --serial-port /dev/ttyUSB0 --pluto-ip 192.168.2.1"
```

Mantén el puerto HTTP `5003` para el quiosco. En `sudo raspi-config`, activa el arranque al escritorio y el inicio de sesión automático del escritorio. Reinicia con `sudo reboot` para comprobar que SATTRACK aparece en pantalla completa.

Si la interfaz no aparece, prueba `curl -fsS http://127.0.0.1:5003/api/status`. Si el servidor no responde, revisa el registro con `sudo journalctl -u sattrack.service -b -n 50 --no-pager`.

## Disposición en 3 pestañas (pantalla de 7")

La interfaz se divide en tres pestañas táctiles, pensadas para 800×480 y 1024×600 (en pantallas más grandes ocupan todo el ancho):

1. **Selección** — pasos 01 a 03: estación de observación, lista/búsqueda de satélites y satélite seleccionado (bajada, velocidad, pases e «Iniciar próximo pase»). Al iniciar un pase salta sola a la pestaña 2.
2. **Seguimiento** — seguimiento orbital / rotor (simulado) con cielo local, mapa, telemetría y el registro de sesión.
3. **Monitor RF** — espectro/waterfall de Maia y su configuración. Muestra el nombre del satélite durante el pase y «Sin seguimiento activo» durante la preparación, la pausa y al finalizar. El punto en la pestaña indica si llegan datos de Maia.

La pestaña activa se recuerda al recargar y también se puede abrir con `#select`, `#track` o `#rf`; las flechas ← → del teclado cambian de pestaña.

## Uso

1. La posición de la estación se toma sola del GPS (modo **Automática**, por defecto; ver «GPS» más abajo). Para fijarla a mano, cambiar el selector a **Manual** y ajustar latitud, longitud y altitud; la elevación mínima siempre es manual. Sin GPS ni fix, los campos conservan los valores iniciales de Santiago (`-33.4489`, `-70.6693`, 570 m, 10°).
2. Buscar un nombre o NORAD, o utilizar los accesos rápidos. El filtro «Banda S» muestra satélites con una bajada activa entre 2000 y 4000 MHz en SatNOGS DB; «Visibles ahora» usa la elevación calculada para la estación.
3. Seleccionar el satélite para ver su posición, transmisores y pases de los próximos cinco días. Al activar **Sector de azimut**, se muestran los pases de las próximas 24 h que se mantienen dentro del arco permitido mientras están sobre la elevación mínima. Por ejemplo, 270° → 240° pasa por el norte y deja fuera los pases que entran entre 240° y 270°.
4. Elegir una bajada catalogada o, si se quiere probar una frecuencia no catalogada, la opción manual simulada. Elegir la velocidad de simulación e iniciar el próximo pase.
5. Observar objetivo orbital, rotor simulado, error de apuntamiento, Doppler calculado, SNR sintética, espectro, waterfall y contador de tramas sintéticas. Se puede pausar, reanudar, cambiar velocidad o detener.

La simulación comienza 20 segundos antes del siguiente AOS y avanza a 10× por defecto. No hay que esperar el pase real. El reloj de la sesión muestra la hora simulada de Chile.

## Datos y límites

- [CelesTrak ACTIVE en formato OMM JSON](https://celestrak.org/NORAD/elements/gp.php?GROUP=ACTIVE&FORMAT=JSON): elementos orbitales usados para propagar posición, azimut, elevación, distancia y pases con Skyfield/SGP4.
- [SatNOGS DB, transmisores](https://db.satnogs.org/api/transmitters/): frecuencias y modos **catalogados**. Un registro activo no asegura que la señal se transmita durante un pase ni que esté autorizada su recepción.
- El movimiento del rotor (mientras no haya una ESP32 conectada), la adquisición de señal, la SNR, el espectro, el waterfall y las tramas son artificiales. El Doppler se calcula a partir de la órbita y la frecuencia escogida, pero se visualiza en un receptor virtual.
- Las efemérides dependen de la actualidad de CelesTrak. La aplicación conserva cachés en `data/`, muestra cuándo se descargó cada fuente y permite actualizarlas con `↻`. Si la actualización falla, sigue usando la última copia e indica el error.
- Las sesiones viven en memoria del servidor y se pierden al reiniciarlo. No se graba IQ ni se demodulan datos reales.

## Estructura

| Archivo | Función |
| --- | --- |
| `app.py` | API Flask y validación del flujo de selección/sesión |
| `catalog.py` | caché, búsqueda y actualización de CelesTrak/SatNOGS |
| `orbit.py` | geometría orbital y cálculo de pases |
| `simulation.py` | reloj acelerado, rotor simulado y pase de la sesión |
| `maia_rf.py`, `mock_maia.py` | cliente de Maia SDR (espectro y AD9361) y simulador de Maia |
| `gps_link.py`, `gps_config.py` | lector del GPS ATGM366H (NMEA/UART), posición estable y parámetros de calidad |
| `serial_link.py`, `rotor_follow.py`, `rotor_config.py` | enlace serial con la ESP32, seguimiento del pase y límites del rotor |
| `firmware/esp32_rotor/esp32_rotor.ino` | lado ESP32 del protocolo (modo simulado y TODO para tu hardware) |
| `templates/index.html` | pantalla única |
| `static/app.js`, `static/rf.js`, `static/rotor.js`, `static/gps.js` | selección/seguimiento, monitor RF, rotor y posición automática por GPS |
| `static/app.css` | estilos adaptables |

La API devuelve `GET /api/catalog`, `GET /api/catalog/<NORAD>`, `POST /api/session`, `GET /api/session/<id>` y `POST /api/session/<id>/control`. Cada estado de sesión tiene `geometry`, `rotor`, `receiver` y marcas `simulated: true`; ese contrato separa la selección y la presentación de una futura integración física.


## Posición automática con GPS (ATGM366H)

Un hilo de fondo (`gps_link.py`) lee las sentencias NMEA del módulo por UART y mantiene el último fix. La interfaz
consulta `GET /api/gps` cada 3 s: en modo **Automática** (el de arranque) copia latitud, longitud y altitud a la
tarjeta «Estación de observación» (campos de solo lectura) y refresca el catálogo, igual que «Actualizar posición».

- **Selector Automática / Manual**: el modo se recuerda en el navegador. En Manual el GPS no toca los campos.
- **Indicador**: `FIX · N SAT` (verde), `BUSCANDO · N SAT` (ámbar), `SIN DATOS` / `SIN GPS` (gris). Mientras no haya fix, los
  campos siguen editables y se usan sus valores como respaldo.
- **Fix aceptado** solo con calidad > 0, ≥ 4 satélites y HDOP ≤ 5; caduca a los 10 s sin fix bueno (`gps_config.py`).
- **Posición estable**: el GPS tiembla unos metros aun quieto; la posición solo se actualiza al moverse más de 50 m (`GPS_MOVE_M`),
  para no recalcular pases sin motivo. Una sesión en curso conserva la posición con que se inició.
- **Peticiones sin `lat`/`lon`/`alt_m`** (uso de la API sin la interfaz) usan la posición del GPS si hay fix, o Santiago si no.
- La altitud es la que informa el GPS (sobre el nivel del mar).

Cableado en la Raspberry Pi: TX del módulo → RX de la UART (`/dev/ttyS0`, 9600 baud) con la consola serie desactivada
(`raspi-config` → Interface Options → Serial Port: consola «No», hardware serial «Sí»).

```bash
python app.py                              # usa /dev/ttyS0 si existe; si no, GPS desactivado (posición manual)
python app.py --gps-port /dev/ttyAMA0      # otro puerto (o variable SATTRACK_GPS_PORT)
python app.py --gps-port sim               # GPS simulado: sin fix ~4 s y luego fix en Santiago (para probar sin hardware)
python app.py --no-gps                     # desactiva el GPS
```

El puerto del GPS no se puede usar a la vez para la ESP32: la lista de puertos lo marca «en uso por el GPS» y se rechaza conectarlo.

## Recepción RF real desde Maia SDR (directo a la Pluto)

SatTrack se conecta directamente a Maia (puerto 8000 de la Pluto), sin servidor MaiaView intermedio:
`WS /waterfall` (frames float32 de potencia lineal) y `GET/PATCH /api/ad9361` (`rx_lo_frequency`,
`sampling_frequency`, `rx_rf_bandwidth`, `rx_gain`, `rx_gain_mode`). El servidor reparte cada frame
tal cual llega a los navegadores por `WS /ws/rf` (como hacía MaiaView con `/ws`; si un navegador va
lento se descartan frames solo para él).

```bash
python app.py --port 5003 --pluto-ip 192.168.2.1        # por defecto 192.168.2.1; o --maia-url http://IP:8000 / MAIA_SERVER_URL
```

**Monitor RF (portado de MaiaView):** espectro y waterfall a 4096 puntos y ~20 cuadros/s (pestañas),
formulario del AD9361 con frecuencias frecuentes (NOAA 137.5, VHF 145.8, UHF 437.5, ADS-B 1090), metadatos Fc/Span/RBW,
escala vertical automática o manual, retención de picos, marcador de pico, suavizado, cursor, pico, frames/s y
**doble clic para sintonizar** la frecuencia bajo el cursor. El monitor funciona sin sesión de satélite.
El espectro y la cascada muestran la potencia lineal de Maia. El eje vertical abrevia miles como
`k` y millones como `M`. Las lecturas de nivel pico y ruido se expresan en dB relativos. La SNR pico
se estima como la diferencia entre el pico y la mediana del ruido.

Entre AOS y LOS, SatTrack guarda una fila por pase en `data/rf_metrics.sqlite3` y la actualiza cuando
recibe una SNR mayor en la bajada seleccionada. Busca el pico en la banda de la bajada con un margen de
250 kHz para el Doppler y guarda la frecuencia del pico, la frecuencia central del espectro y la hora UTC
de ese mismo frame. La captura se pausa con el pase y funciona con la pestaña cerrada. En **Monitor RF →
Métricas del pase**, **Descargar CSV** exporta el historial.

Diagnóstico: `GET /api/rf/status`. Configuración: `GET/PATCH /api/rf/config` (acepta solo las 5 claves de arriba y devuelve lo que Maia aceptó).

**Aún no portado desde MaiaView:** seguimiento a partir de un TLE pegado a mano (aquí se usa el catálogo de CelesTrak) y el mapa manual por Az/El/Alt o Lat/Lon/Alt con presets de observatorio.

## Captura de la BNO085

1. Carga el firmware BNO085 de esta rama en la ESP32 y conéctala desde **Seguimiento**.
2. Pulsa **Grabar BNO** y espera a que suba el contador de muestras. Puedes grabar mientras haces seguimiento.
3. Pulsa **Detener captura** y luego **Descargar CSV**. SATTRACK deja una copia en `data/bno/`. Si se pierde el enlace serial, puedes descargar lo que se alcanzó a grabar.

El CSV incluye los cuaterniones `qw`, `qx`, `qy` y `qz`, la hora de recepción en UTC, el contador de la ESP32, la precisión y los ángulos antes de aplicar los offsets. Para probar sin hardware, elige el puerto `sim`. La interfaz indica que son muestras simuladas y el nombre del archivo comienza con `bno_sim_`.

También se puede usar `capturar_bno.py` desde la terminal con la ESP32 desconectada de SATTRACK.

## Rotor (ESP32 por puerto serial)

En la pestaña **Seguimiento**, la tarjeta *Rotor · ESP32* abre el puerto serial, envía referencias de azimut/elevación y recibe la posición actual.
Se puede conectar desde la interfaz o al iniciar: `python app.py --serial-port COM5` (o `/dev/ttyUSB0`). El puerto **`sim`** es una ESP32 simulada
(12 °/s en azimut, 6 °/s en elevación) para probar sin hardware. Solo se aceptan puertos que el sistema lista (más `sim`); en Linux el usuario debe pertenecer al grupo `dialout`.

Protocolo: texto, una línea por mensaje (`\n`), 115200 baud por defecto (`rotor_config.py` y `BAUD` en el sketch).

| Sentido | Mensaje | Significado |
| --- | --- | --- |
| PC → ESP32 | `SET,<az>,<el>` | referencia fija (la usa el seguimiento para mantener la posición); no hay control manual en la interfaz |
| PC → ESP32 | `TRK,<az>,<el>,<vaz>,<vel>` | seguimiento: posición del satélite (°) y su velocidad angular (°/s), 5 Hz |
| PC → ESP32 | `CAL,<az>,<el>` / `STOP` | calibra la posición actual del sensor / frena y borra fallas (firmware en lazo cerrado con BNO085, ver `rotor_esp32`) |
| ESP32 → PC | `POS,<az>,<el>` | posición actual medida por el sensor, ~5 Hz (si el sensor cae, deja de enviarse) |
| ESP32 → PC | `BAT,<V>` | voltaje de la batería en V, ~1 Hz (se muestra en la barra superior, a la izquierda de la hora) |
| ESP32 → PC | otra línea | se muestra en el registro (`ACK` se ignora; las de depuración deben empezar con `#`) |

- **Referencia manual:** azimut/elevación + *Enviar al rotor*; *Usar az/el del satélite* copia el objetivo de la sesión.
- **Seguir con el rotor:** con una sesión activa, lleva el rotor al punto de aparición antes del AOS y, del AOS al LOS, envía `TRK` con la posición y la velocidad angular del satélite (la ESP32 extrapola entre mensajes). Al LOS, al pausar o al detener la sesión, el rotor se queda donde está. La casilla *Seguir automáticamente al iniciar un pase* lo activa al pulsar «Iniciar próximo pase».
- **Velocidad de simulación:** la sesión reproduce el próximo pase desde 20 s antes del AOS. El valor inicial es **1×**. A 5×, 10× o 20× las velocidades enviadas se multiplican y la interfaz avisa si el pase supera `ROTOR_MAX_RATE_AZ/EL`.
- **Adelanto del tracker:** puedes ajustarlo en **Seguimiento → Tracker · ESP32** o junto al nombre del satélite en **Monitor RF**. Ambos controles están sincronizados. Mueve el deslizador de 0 a 300 s y pulsa **Aplicar**. El cambio se aplica al seguimiento en curso y se conserva para los siguientes pases hasta reiniciar SatTrack. El objetivo se limita a la última posición del pase (LOS).
- **Con la ESP32 conectada**, la posición del rotor en el cielo local, la telemetría y el error de apuntamiento son los reales (`POS`); sin ella siguen simulados.
- **Mapa:** un vector ámbar sale del observador hacia el azimut actual del rotor (punteado: objetivo) y la barra derecha muestra la elevación actual y objetivo. Si pasan más de `SERIAL_STALE_S` segundos sin `POS`, la marca se atenúa.
- El puerto se abre con DTR/RTS en bajo para que la mayoría de las ESP32 no se reinicien al conectar. El sketch está **sin probar en una ESP32 real**; con `SIMULATE_MOTION 1` simula el rotor y sirve para verificar cable y baudios.

Límites: un montaje az/el no puede seguir un pase casi cenital (el azimut gira muy rápido); la precisión depende del TLE (CelesTrak) y del reloj del equipo (error de 1 s ≈ 7 km sobre la órbita).

## Probar el monitor RF con Maia simulado (sin Pluto)

`mock_maia.py` (tu simulador) simula Maia (`/api/ad9361` y `/waterfall` en el puerto 8000) con señales en frecuencias
absolutas (137.5, 145.8, 430, 431.25, 437.5 con Doppler, 1090 MHz), así que al cambiar centro o sample rate se mueven o desaparecen.

```bash
python mock_maia.py                 # terminal 1 (127.0.0.1:8000)
python app.py --port 5003 --pluto-ip 127.0.0.1   # terminal 2 (apunta al mock)
curl -X PATCH localhost:5003/api/rf/config -H 'content-type: application/json' -d '{"rx_lo_frequency":437500000,"sampling_frequency":4000000}'
```

Si no llegan frames por >2 s el monitor pasa a «ESPERANDO RF»; el cliente reconecta cada 2 s.
