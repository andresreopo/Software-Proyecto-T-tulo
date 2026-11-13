# Rotor az/el ESP32 — lazo cerrado con BNO085

Firmware sin WiFi/HTTP. Un **BNO085** pegado a la antena mide hacia dónde apunta realmente (azimut y
elevación); esa medición es la posición que se informa (`POS`) y la que cierra el lazo de control.
Los pasos enviados a los drivers ya no se usan como posición: solo fijan la velocidad y alimentan un
detector de fallas.

## Archivos
- `src/RotorConfig.h` — pines, mecánica, velocidades, montaje del sensor, control y seguridad.
- `src/RotorMath.h`   — cuaternión→azimut/elevación, ángulos y parseo (sin hardware, probable en PC).
- `src/main.cpp`      — protocolo serial, tarea del sensor, lazo de control y generador de pasos.
- `platformio.ini`    — dependencia: SparkFun BNO080 Cortex Based IMU.

## Conexión del BNO085 (I2C)
| BNO085 | ESP32 |
|---|---|
| VIN / 3V3 | 3V3 |
| GND | GND (común con drivers) |
| SDA | GPIO 25 |
| SCL | GPIO 26 |
| ADDR / SA0 | libre → 0x4A; a 3V3 → 0x4B (`IMU_ADDRESS`) |

El I2C por defecto del ESP32 (GPIO 21/22) lo ocupan STEP/DIR, por eso se usan 25/26 (editable). Cables
cortos y separados de los de los motores; el sensor lejos de imanes e hierro (motores, reductores).

## Protocolo (115200 8N1, texto, una orden por línea con `\n`)
Es el de `serial_link.py` de la interfaz unificada, más `CAL` e `IMU`.

| Interfaz → ESP32 | Respuesta |
|---|---|
| `SET,<az>,<el>` ir a la posición y mantenerla | `ACK` / `ERR,<motivo>` |
| `TRK,<az>,<el>,<vaz>,<vel>` posición del satélite (°) y velocidad angular (°/s), ~5 Hz | ninguna / `ERR,<motivo>` |
| `STOP` frenar con rampa y **borrar fallas** | `ACK` |
| `CAL,<az>,<el>` la antena apunta ahora a esa posición (guarda). Con azimut absoluto (por defecto) **solo calibra la elevación**: el `<az>` se ignora | `ACK` / `ERR,busy` / `ERR,no_imu` |
| `ZERO` = `CAL,0,0` | idem |
| `CALAX` calibración de ejes con el sensor (mueve ±20° cada eje, ~30 s; `STOP` la aborta) | `ACK` / `ERR,busy` / `ERR,no_imu` + líneas `# calax ...` |
| `GET` | `POS,<az>,<el>` |
| `IMU` diagnóstico del sensor | línea `#` con az, el, valores crudos, precisión y offsets |

ESP32 → interfaz: `POS,<az>,<el>` cada 200 ms (**posición medida**, azimut en norte verdadero = rumbo magnético del sensor + declinación; si el sensor cae se deja de
enviar y la interfaz la marca obsoleta), `BAT,<V>` cada 1 s, líneas `#` de diagnóstico.
Motivos de `ERR`: `formato`, `invalid_target`, `queue_full`, `busy`, `no_imu`, `fault`,
`fault_az`/`fault_el`, `unknown_command`, `line_too_long`, `line_timeout`, `invalid_character`.

## Captura del BNO en CSV

El booleano `PRINT_BNO_DATA` en `src/RotorConfig.h` viene en `true`.
El firmware envía una muestra nueva cada 50 ms como máximo, cuando hay espacio
en el búfer serial. Cambia el valor a `false` y vuelve a cargar el firmware
para desactivar estas líneas. Los mensajes POS y BAT conservan su formato y
frecuencia.

```text
BNO,esp_ms,muestra,qw,qx,qy,qz,estado_precision,error_estimado_deg,az_sin_offset_deg,el_sin_offset_deg
```

`esp_ms` y `muestra` cuentan desde el arranque del ESP32. El cuaternión y el
estado de precisión (0 a 3) provienen del BNO. Los dos ángulos se calculan con
el eje de la antena antes de aplicar los offsets. El CSV añade `pc_utc` al
recibir cada muestra y deja vacíos los valores que el firmware informa como
`nan`.

Después de cargar el firmware, ejecuta uno de estos comandos en PowerShell
desde la raíz del proyecto Python. Sustituye COM6 por el puerto de tu ESP32.
Cada comando crea un CSV nuevo y escribe los registros a medida que llegan.

Para capturar con el puerto libre, cierra el monitor serial y desconecta el
rotor de SatTrack antes de ejecutar el capturador.

```powershell
.\.venv\Scripts\python.exe .\capturar_bno.py --port COM6 --output .\bno.csv
```

Para capturar mientras controlas el rotor con SatTrack, inicia la aplicación
con el CSV y deja que SatTrack sea quien abra el puerto.

```powershell
.\.venv\Scripts\python.exe .\app.py --serial-port COM6 --bno-csv .\bno_sattrack.csv
```

Detén la captura con Ctrl+C. El capturador y la opción de SatTrack guardan
únicamente líneas `BNO,` válidas. Usa otro nombre para una captura nueva.

## Control
- **SET:** velocidad proporcional al error, limitada por la rampa de frenado `sqrt(2·a·error)` para no
  pasarse; dentro de `GOTO_DEADBAND_DEG` se detiene y vigila (corrige si el sensor ve una desviación).
- **TRK:** `velocidad = vsat + CTRL_KP·error` sobre la posición medida, con la referencia extrapolada entre
  mensajes. Sin `TRK` por 1.5 s el rotor frena y queda en reposo (sigue hasta ~1.5 s a la velocidad del
  satélite antes de frenar).
- Flujo de la interfaz: `SET` al AOS → `TRK` a 5 Hz → `SET` al LOS.

## Seguridad
- **Sensor caído** (sin datos por `IMU_TIMEOUT_MS`): frena, olvida el objetivo, rechaza `SET/TRK` con
  `ERR,no_imu` y deja de enviar `POS`. Al volver el sensor espera una orden nueva.
- **Detector de fallas:** cada `FAULT_WINDOW_MS` (4 s) compara el movimiento esperado (pasos enviados) con el
  medido. Si un eje avanza menos del 10 % de lo esperado, o al revés, en 2 ventanas seguidas, detiene ambos
  ejes, envía `ERR,fault_az|el` y rechaza órdenes (`ERR,fault`) hasta recibir `STOP`. Junto al error imprime el
  motivo: `# fault el: esperado=+18.00 medido=-17.90 deg en 4.0s (avance=-99%, ...)` y una línea `# motivo:`
  que distingue **sentido contrario** (INVERT o sensor) de **casi no se movió** (driver sin energía, corriente
  baja, motor trabado). Con sentido invertido el eje puede recorrer ~30° al revés antes del corte (en
  simulación); `FAULT_DETECTION=false` lo desactiva.
  Sobre 80° de elevación el azimut no se evalúa (indefinido cerca del cenit).

## Azimut absoluto respecto al norte magnético
Con `AZ_ABSOLUTE_MAGNETIC = true` (por defecto) el azimut es absoluto: sale del magnetómetro del BNO085
(`ROTATION_VECTOR`) sin ninguna calibración manual ni offset guardado. El modelo propagado (Skyfield) entrega
azimut respecto al **norte verdadero**, así que el firmware informa y controla
`azimut = rumbo_magnético + MAG_DECLINATION_DEG`. **Escribe en `RotorConfig.h` la declinación de tu ubicación y
fecha** (positiva hacia el este; calculador del NOAA/NCEI); con 0.0 el rotor apunta en norte magnético y no
coincidirá con el modelo. El valor de la declinación en Chile central es de pocos grados, pero cambia con el lugar y
el año.

## Posición del sensor respecto al eje (11 cm adelante, 29.5 cm arriba)
**No necesita compensación.** La orientación de un cuerpo rígido es la misma en todos sus puntos: el sensor mide el
mismo azimut y elevación esté en el eje o a 31.5 cm de él. Solo importaría para aceleraciones lineales
(giro a ≤ 4.5 °/s con ese brazo: < 0.02 m/s², ≈ 0.1° de error de inclinación), despreciable. Lo que sí
descuadra la elevación es otra cosa: **la orientación con que está pegado el sensor**.

## Por qué la elevación puede "no cuadrar" y cómo se corrige (CALAX)
La dirección de la antena se calcula con un eje del sensor (`BORESIGHT`). Si ese eje no es exactamente
perpendicular a la bisagra de elevación (el sensor pegado con unos grados de giro), la elevación medida se
comprime (con 10° de desalineación: 72° medidos a 75° reales; con 20°: 65°) y, peor, **subir la elevación mueve el
azimut real** (en simulación, a 52° de elevación el azimut real se había ido 17° mientras el medido seguía
en 100°). `CALAX` lo mide con el propio sensor: mueve azimut y elevación ±20°, obtiene alrededor de qué eje giró cada
uno (sin suponer nada del montaje), corrige `BORESIGHT` para que sea perpendicular a la bisagra y lo guarda en
memoria. También informa:
- giro real vs pedido de cada eje y los `AZ_STEPS_PER_DEG` / `EL_STEPS_PER_DEG` que corresponderían,
- que los ejes az/el sean perpendiculares entre sí (ideal 90°) y la inclinación de la base,
- si algún eje gira al revés (`INVERT`).

## Puesta en marcha (con el rotor despejado: CALAX mueve los motores)
1. Enciende y envía `IMU`: debe responder con valores y `acc` pequeño. Si dice `imu_init_failed`, revisa SDA/SCL/dirección.
2. **Montaje del sensor:** la parte superior del sensor (+Y) apunta en la dirección de la antena y X a la
   izquierda (Z queda hacia abajo) → `BORESIGHT = (0, 1, 0)` (ya configurado). X coincide con la bisagra de
   elevación. Si ya ejecutaste `CALAX` con otro valor, el eje guardado en memoria tiene prioridad: envía
   `BORE,0,1,0` por UART para reemplazarlo (sirve también para probar otro montaje sin reflashear) y repite
   `CALAX`. Verifica con `IMU` (az correcto con la antena en un rumbo conocido) y con `SET` (al pedir más
   elevación la antena sube).
3. `MAG_DECLINATION_DEG = 0.08` (0.08° E ± 0.37°, deriva 0.18° W/año), ya configurado.
4. Envía `CALAX` (y no mandes `SET/TRK` hasta que termine). Revisa las líneas `# calax`: corrige `INVERT` si lo
   indica y actualiza los pasos/° si el giro real difiere. El aviso de arranque "eje sin calibrar" desaparece.
5. **Cero de elevación:** no hace falta llevar la antena al horizonte (con los motores energizados no se mueve a
   mano). Con la antena quieta en cualquier posición, mide su elevación real con un inclinómetro digital o la
   app de nivel de un celular apoyado en una cara de referencia, o apunta a un objeto de elevación conocida, y envía
   `CAL,0,<elevación real>` (p. ej. `CAL,0,32.4`; el azimut se ignora). Si necesitas moverla para medir, usa
   `SET,<az>,<el>`. Se mide una sola vez: corrige el desfase fijo entre el eje calculado por `CALAX` y el eje
   mecánico de la antena.
6. Primer movimiento: `SET` pequeño. Si responde `ERR,fault_az|el` inmediatamente, revisa el sentido; con el eje
   **sin calibrar**, subir la elevación también puede disparar la falla por el acoplamiento de arriba.
7. Si cambias el sensor de lugar o de orientación, repite `CALAX`.

## Límites a tener presentes
- **Precisión:** con magnetómetro el azimut absoluto tiene ~1–2° de error típico y los motores/estructura de acero lo
  distorsionan; calibra el magnetómetro del BNO085 en el lugar (movimientos en 8) y lejos de los motores. Si el
  azimut no es estable, prueba `IMU_USE_GAME_ROTATION = true` (sin magnetómetro, pero con deriva lenta).
  No bajes `GOTO_DEADBAND_DEG` de la precisión real del sensor o el rotor oscilará.
- **Velocidad:** con 88.9 pasos/° y `MAX_SPEED = 400` el tope es ≈ 4.5 °/s por eje. El generador admite hasta
  1000 pasos/s (≈ 11 °/s): súbelo en `RotorConfig.h` si el motor y el driver lo permiten, y ajusta
  `ROTOR_MAX_RATE_AZ/EL` en `rotor_config.py` al valor real.
- Sin homing ni finales de carrera: los límites de elevación (0–90°) se imponen sobre la posición medida.
- Los parámetros mecánicos (pasos/vuelta, microstepping, reducción) pueden ser aproximados: el sensor corrige
  la posición; solo fijan las velocidades y el detector de fallas.

## Pruebas realizadas
Compilado en PC con stubs de Arduino/FreeRTOS/BNO08x (no con el core real del ESP32) y simulación con planta
ideal, ruido de ±0.15° y 20 ms de latencia en el sensor: SET con error final ≈ 0.2° y sin deriva al mantener;
seguimiento a 3 °/s con error máx. 0.15° (az) y 0.05° (el); `CAL` con offset de 37°; detección de sentido
invertido; sensor caído y recuperación. Falta validar con hardware real.
`CALAX` se probó con una planta con sensor desalineado 12° (yaw) y 4° (cabeceo), declinación de prueba de −1.5° y
motores con escala ×1.05 (az) y ×0.88 (el): recuperó la desalineación (11.8°), las escalas (×1.051 y ×0.884) y,
aplicada, azimut y elevación siguieron lo pedido con ≤ 0.5° de error (antes: 17° de deriva de azimut a 52° de
elevación). Con 4 °/s y 5 °/s de tope del motor no se ha probado el barrido a otras velocidades.
