# SatTrack + Maia (simulación)

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
- Las efemérides dependen de la actualidad de CelesTrak. La copia inicial incluida en `seed/` permite abrir el proyecto sin Internet; las actualizaciones se guardan en `data/`, fuera de Git. La aplicación muestra cuándo se descargó cada fuente y permite actualizarlas con `↻`. Si la actualización falla, sigue usando la última copia e indica el error.
- Las sesiones viven en memoria del servidor y se pierden al reiniciarlo. No se graba IQ ni se demodulan datos reales.

## Estructura

| Archivo | Función |
| --- | --- |
| `app.py` | API Flask y validación del flujo de selección/sesión |
| `catalog.py` | caché, búsqueda y actualización de CelesTrak/SatNOGS |
| `seed/` | instantánea inicial de ambos catálogos |
| `orbit.py` | geometría orbital y cálculo de pases |
| `simulation.py` | reloj acelerado, rotor y receptor sintéticos |
| `templates/index.html` | pantalla única |
| `static/app.js` | interacción, gráficos y polling |
| `static/app.css` | estilos adaptables |

La API devuelve `GET /api/catalog`, `GET /api/catalog/<NORAD>`, `POST /api/session`, `GET /api/session/<id>` y `POST /api/session/<id>/control`. Cada estado de sesión tiene `geometry`, `rotor`, `receiver` y marcas `simulated: true`; ese contrato separa la selección y la presentación de una futura integración física.
