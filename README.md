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

## Disposición en 3 pestañas (pantalla de 7")

La interfaz se divide en tres pestañas táctiles, pensadas para 800×480 y 1024×600 (en pantallas más grandes ocupan todo el ancho):

1. **Selección** — pasos 01 a 03: estación de observación, lista/búsqueda de satélites y satélite seleccionado (bajada, velocidad, pases e «Iniciar próximo pase»). Al iniciar un pase salta sola a la pestaña 2.
2. **Seguimiento** — seguimiento orbital / rotor (simulado) con cielo local, mapa, telemetría y el registro de sesión.
3. **Monitor RF** — espectro/waterfall de Maia y su configuración. El punto junto al nombre indica el estado: verde = recibiendo, rojo = Maia desconectada.

La pestaña activa se recuerda al recargar y también se puede abrir con `#select`, `#track` o `#rf`; las flechas ← → del teclado cambian de pestaña.

## Uso

1. Ajustar latitud, longitud, altitud y elevación mínima de la estación. La ubicación inicial es Santiago (`-33.4489`, `-70.6693`, 570 m, 10°).
2. Buscar un nombre o NORAD, o utilizar los accesos rápidos. El filtro «Banda S» muestra satélites con una bajada activa entre 2000 y 4000 MHz en SatNOGS DB; «Visibles ahora» usa la elevación calculada para la estación.
3. Seleccionar el satélite. La aplicación muestra su posición actual, transmisores registrados y próximos pases sobre la elevación mínima durante cinco días.
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
| `serial_link.py`, `rotor_follow.py`, `rotor_config.py` | enlace serial con la ESP32, seguimiento del pase y límites del rotor |
| `firmware/esp32_rotor/esp32_rotor.ino` | lado ESP32 del protocolo (modo simulado y TODO para tu hardware) |
| `templates/index.html` | pantalla única |
| `static/app.js`, `static/rf.js`, `static/rotor.js` | selección/seguimiento, monitor RF y rotor |
| `static/app.css` | estilos adaptables |

La API devuelve `GET /api/catalog`, `GET /api/catalog/<NORAD>`, `POST /api/session`, `GET /api/session/<id>` y `POST /api/session/<id>/control`. Cada estado de sesión tiene `geometry`, `rotor`, `receiver` y marcas `simulated: true`; ese contrato separa la selección y la presentación de una futura integración física.


## Recepción RF real desde Maia SDR (directo a la Pluto)

SatTrack se conecta directamente a Maia (puerto 8000 de la Pluto), sin servidor MaiaView intermedio:
`WS /waterfall` (frames de 4096 float32 en dB) y `GET/PATCH /api/ad9361` (`rx_lo_frequency`,
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
SNR y detección siguen en «—» hasta definir calibración; los dB se muestran tal como llegan.
La sintonía nunca cambia sola: la frecuencia del satélite es solo metadato orbital.

Diagnóstico: `GET /api/rf/status`. Configuración: `GET/PATCH /api/rf/config` (acepta solo las 5 claves de arriba y devuelve lo que Maia aceptó).

**Aún no portado desde MaiaView:** seguimiento a partir de un TLE pegado a mano (aquí se usa el catálogo de CelesTrak) y el mapa manual por Az/El/Alt o Lat/Lon/Alt con presets de observatorio.

## Rotor (ESP32 por puerto serial)

En la pestaña **Seguimiento**, la tarjeta *Rotor · ESP32* abre el puerto serial, envía referencias de azimut/elevación y recibe la posición actual.
Se puede conectar desde la interfaz o al iniciar: `python app.py --serial-port COM5` (o `/dev/ttyUSB0`). El puerto **`sim`** es una ESP32 simulada
(12 °/s en azimut, 6 °/s en elevación) para probar sin hardware. Solo se aceptan puertos que el sistema lista (más `sim`); en Linux el usuario debe pertenecer al grupo `dialout`.

Protocolo: texto, una línea por mensaje (`\n`), 115200 baud por defecto (`rotor_config.py` y `BAUD` en el sketch).

| Sentido | Mensaje | Significado |
| --- | --- | --- |
| PC → ESP32 | `SET,<az>,<el>` | referencia fija (la usa el seguimiento para mantener la posición); no hay control manual en la interfaz |
| PC → ESP32 | `TRK,<az>,<el>,<vaz>,<vel>` | seguimiento: posición del satélite (°) y su velocidad angular (°/s), 5 Hz |
| ESP32 → PC | `POS,<az>,<el>` | posición actual, ~5 Hz |
| ESP32 → PC | otra línea | se muestra en el registro (`ACK` se ignora; las de depuración deben empezar con `#`) |

- **Referencia manual:** azimut/elevación + *Enviar al rotor*; *Usar az/el del satélite* copia el objetivo de la sesión.
- **Seguir con el rotor:** con una sesión activa, lleva el rotor al punto de aparición antes del AOS y, del AOS al LOS, envía `TRK` con la posición y la velocidad angular del satélite (la ESP32 extrapola entre mensajes). Al LOS, al pausar o al detener la sesión, el rotor se queda donde está. La casilla *Seguir automáticamente al iniciar un pase* lo activa al pulsar «Iniciar próximo pase».
- **Velocidad de simulación:** la sesión reproduce el próximo pase desde 20 s antes del AOS. A **1×** es seguimiento en tiempo real; a 5×, 10× o 20× las velocidades enviadas se multiplican y casi ningún rotor puede seguir (la interfaz avisa si el pase supera `ROTOR_MAX_RATE_AZ/EL`).
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
