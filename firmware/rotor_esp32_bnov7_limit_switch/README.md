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
| `STOP` frenar con rampa y **borrar fallas**. Durante un retroceso de azimut, lo interrumpe de inmediato | `ACK` |
| `CAL,<az>,<el>` la antena apunta ahora a esa posición (guarda). Con azimut absoluto (por defecto) **solo calibra la elevación**: el `<az>` se ignora | `ACK` / `ERR,busy` / `ERR,no_imu` |
| `ZERO` = `CAL,0,0` | idem |
| `CALAX` calibración de ejes con el sensor (mueve ±20° cada eje, ~30 s; `STOP` la aborta) | `ACK` / `ERR,busy` / `ERR,no_imu` + líneas `# calax ...` |
| `GET` | `POS,<az>,<el>` |
| `IMU` diagnóstico del sensor | línea `#` con az, el, valores crudos, precisión y offsets |

ESP32 → interfaz: `POS,<az>,<el>` cada 200 ms (**posición medida**, azimut en norte verdadero = rumbo magnético del sensor + declinación; si el sensor cae se deja de
enviar y la interfaz la marca obsoleta), `BAT,<V>` cada 1 s, líneas `#` de diagnóstico.
Motivos de `ERR`: `formato`, `invalid_target`, `queue_full`, `busy`, `no_imu`, `fault`,
`fault_az`/`fault_el`, `fault_az_limit`, `track_wait`, `unknown_command`, `line_too_long`, `line_timeout`, `invalid_character`.

## Control
- **SET:** velocidad proporcional al error, limitada por la rampa de frenado `sqrt(2·a·error)` para no
  pasarse; dentro de `GOTO_DEADBAND_DEG` se detiene y vigila (corrige si el sensor ve una desviación).
- **TRK:** `velocidad = vsat + CTRL_KP·error + corrección integral` sobre la posición medida, con la referencia
  extrapolada entre mensajes. Cada eje conserva su integral entre mensajes, para compensar el error sostenido.
  La corrección tiene una cota y se acumula solo en la medida en que lo permiten la velocidad, la rampa y los
  topes. Se reinicia al salir de seguimiento por `STOP`, `SET`, falta de mensajes por 1.5 s, pérdida de la
  IMU o una falla.
- Flujo de la interfaz: `SET` al AOS → `TRK` a 5 Hz → `SET` al LOS.

En `src/RotorConfig.h`, `TRK_KI_AZ` y `TRK_KI_EL` parten en 0.8 s⁻². Un valor de 0 desactiva la integral del
eje para comparar con el control proporcional. `TRK_MAX_INTEGRAL_DEG_S` limita su aporte a ±1.5 °/s por eje.
Las ganancias iniciales se verificaron en simulación y se pueden ajustar con mediciones del rotor.

## Seguridad
- **Final de carrera de azimut.** GPIO 32 se configura con `INPUT_PULLUP`. El interruptor debe llevar
  la entrada a GND al activarse. El primer LOW cancela los movimientos de ambos ejes y `CALAX`, y
  emite `ERR,fault_az_limit`. SatTrack detiene el envío de referencias. Solo azimut puede moverse
  durante el retroceso, en sentido contrario al del último paso normal y a un máximo de 100 micropasos/s.
  Al detectar HIGH se detiene y verifica que el contacto siga liberado durante 30 ms antes del STOP
  automático. Si hay rebote se puede continuar en el mismo sentido hasta el límite total de 3°.
  Si el contacto sigue presionado tras esos 3°, si no hubo un paso de azimut en los últimos 250 ms
  (por ejemplo, si arranca presionado) o si llega `STOP`, se bloquea el retroceso hasta que se libere
  el contacto. El firmware queda en reposo y necesita una referencia nueva. Las secuencias continuas
  de `TRK` se descartan con `ERR,track_wait` hasta que haya una pausa de 1.5 s entre mensajes.
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
El control PI se verifica con regresiones de convergencia, saturación, rampa, reinicio y tope mecánico en
`tests/track_pi_checks.cpp`. Desde esta carpeta, puedes compilar y ejecutar las pruebas en Windows
con GNU C++.

```powershell
$testExe = Join-Path $env:TEMP 'rotor-track-pi-checks.exe'
g++ -std=c++11 -Isrc tests/track_pi_checks.cpp -o $testExe
if ($LASTEXITCODE -eq 0) { & $testExe }
```

Para compilar el firmware para `esp32dev`, ejecuta `pio run` desde esta carpeta.
El mismo comando de pruebas de PC con `tests/az_limit_checks.cpp` comprueba el sentido del retroceso,
los rebotes, el límite de pasos, la cancelación con STOP y el rechazo de las referencias `TRK` anteriores.

La versión anterior del control se probó en PC con stubs de Arduino/FreeRTOS/BNO08x y simulación con planta
ideal, ruido de ±0.15° y 20 ms de latencia en el sensor: SET con error final ≈ 0.2° y sin deriva al mantener;
seguimiento a 3 °/s con error máx. 0.15° (az) y 0.05° (el); `CAL` con offset de 37°; detección de sentido
invertido; sensor caído y recuperación. Falta validar con hardware real.
`CALAX` se probó con una planta con sensor desalineado 12° (yaw) y 4° (cabeceo), declinación de prueba de −1.5° y
motores con escala ×1.05 (az) y ×0.88 (el): recuperó la desalineación (11.8°), las escalas (×1.051 y ×0.884) y,
aplicada, azimut y elevación siguieron lo pedido con ≤ 0.5° de error (antes: 17° de deriva de azimut a 52° de
elevación). Con 4 °/s y 5 °/s de tope del motor no se ha probado el barrido a otras velocidades.


## Precisión del sensor y calibración del magnetómetro

- `ACC` responde `# acc fusion=<Unreliable|Low|Medium|High>(n) err=<°> mag=<...>(n)`: calidad de la
  fusión, error angular estimado y estado de calibración del magnetómetro (MagAcc). `IMU` lo incluye.
- Con `POS_INCLUDE_ACCURACY = true` (RotorConfig.h) `POS` pasa a
  `POS,<az>,<el>,<fusionAcc 0-3>,<err_deg>,<magAcc 0-3>`. Por defecto queda el `POS,<az>,<el>` original.
- Rutina de calibración (con el rotor detenido): `MAGCAL` -> mover el sensor en figuras de 8, lejos de
  motores e imanes, hasta ver `Mag=High` -> `SAVECAL` (guarda en la flash del BNO085 y termina MAGCAL).
  `ENDCAL` aborta sin guardar. Mientras dura MAGCAL el firmware rechaza SET/TRK/CALAX con `ERR,busy`.
