# Comentarios de versión

V5: 
- Se separó la interfaz en 3 pestañas para visualizar en pantalla de 7 pulgadas. 
- Se incorpora conexión real a pluto y se añade el script ```mock_maia.py``` que permite simular una pluto de forma externa.
- Se añade un mapa para visualización de satélite y observador respecto al espacio aéreo chileno.


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

El servidor escucha solo en `127.0.0.1` por defecto. Para acceder desde otro equipo de la misma red, iniciarlo con `--host 0.0.0.0` y abrir `http://IP_DEL_EQUIPO:5003/`, sujeto al firewall local. En esta entrega **no se conecta ni envía comandos a Pluto SDR ni a un rotor**.

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
- El movimiento del rotor, la adquisición de señal, la SNR, el espectro, el waterfall y las tramas son artificiales. El Doppler se calcula a partir de la órbita y la frecuencia escogida, pero se visualiza en un receptor virtual.
- Las efemérides dependen de la actualidad de CelesTrak. La aplicación conserva cachés en `data/`, muestra cuándo se descargó cada fuente y permite actualizarlas con `↻`. Si la actualización falla, sigue usando la última copia e indica el error.
- Las sesiones viven en memoria del servidor y se pierden al reiniciarlo. No se graba IQ ni se demodulan datos reales.

## Estructura

| Archivo | Función |
| --- | --- |
| `app.py` | API Flask y validación del flujo de selección/sesión |
| `catalog.py` | caché, búsqueda y actualización de CelesTrak/SatNOGS |
| `orbit.py` | geometría orbital y cálculo de pases |
| `simulation.py` | reloj acelerado, rotor y receptor sintéticos |
| `templates/index.html` | pantalla única |
| `static/app.js` | interacción, gráficos y polling |
| `static/app.css` | estilos adaptables |

La API devuelve `GET /api/catalog`, `GET /api/catalog/<NORAD>`, `POST /api/session`, `GET /api/session/<id>` y `POST /api/session/<id>/control`. Cada estado de sesión tiene `geometry`, `rotor`, `receiver` y marcas `simulated: true`; ese contrato separa la selección y la presentación de una futura integración física.


## Recepción RF real desde Maia SDR (directo a la Pluto)

SatTrack se conecta directamente a Maia (puerto 8000 de la Pluto), sin servidor MaiaView intermedio:
`WS /waterfall` (frames de 4096 float32 en dB) y `GET/PATCH /api/ad9361` (`rx_lo_frequency`,
`sampling_frequency`, `rx_rf_bandwidth`, `rx_gain`, `rx_gain_mode`). El servidor reparte cada frame
tal cual llega a los navegadores por `WS /ws/rf` (como hacía MaiaView con `/ws`; si un navegador va
lento se descartan frames solo para él).

```bash
python app.py --port 5003 --maia-url http://192.168.2.1:8000   # o MAIA_SERVER_URL; por defecto http://127.0.0.1:8000
```

**Monitor RF (portado de MaiaView):** espectro y waterfall a 4096 puntos y ~20 cuadros/s (pestañas),
formulario del AD9361 con frecuencias frecuentes (NOAA 137.5, VHF 145.8, UHF 437.5, ADS-B 1090), metadatos Fc/Span/RBW,
escala vertical automática o manual, retención de picos, marcador de pico, suavizado, cursor, pico, frames/s y
**doble clic para sintonizar** la frecuencia bajo el cursor. El monitor funciona sin sesión de satélite.
SNR y detección siguen en «—» hasta definir calibración; los dB se muestran tal como llegan.
La sintonía nunca cambia sola: la frecuencia del satélite es solo metadato orbital.

Diagnóstico: `GET /api/rf/status`. Configuración: `GET/PATCH /api/rf/config` (acepta solo las 5 claves de arriba y devuelve lo que Maia aceptó).

**Aún no portado desde MaiaView** (rotor y seguimiento siguen simulados aquí): enlace serial con la ESP32 (`SET`/`TRK`/`POS`,
puerto `sim`), seguimiento TLE hacia el rotor, vector de azimut y barra de elevación sobre el mapa, y el mapa manual
por Az/El/Alt o Lat/Lon/Alt con presets de observatorio.

## Probar el monitor RF con Maia simulado (sin Pluto)

`mock_maia.py` (tu simulador) simula Maia (`/api/ad9361` y `/waterfall` en el puerto 8000) con señales en frecuencias
absolutas (137.5, 145.8, 430, 431.25, 437.5 con Doppler, 1090 MHz), así que al cambiar centro o sample rate se mueven o desaparecen.

```bash
python mock_maia.py                 # terminal 1 (127.0.0.1:8000)
python app.py --port 5003           # terminal 2
curl -X PATCH localhost:5003/api/rf/config -H 'content-type: application/json' -d '{"rx_lo_frequency":437500000,"sampling_frequency":4000000}'
```

Si no llegan frames por >2 s el monitor pasa a «ESPERANDO RF»; el cliente reconecta cada 2 s.
