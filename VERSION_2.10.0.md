# NamToClo v2.10.0 — Tone Match sobre el destino final

## Flujo implementado

1. Elegir destino al inicio: GP-200 (B1024) o GP-5 (B512). GP-200 es el predeterminado.
2. Convertir el NAM con el trainer original A128/B2048.
3. Aplicar Corrective IR, si se ha activado, sobre B2048. Se conserva su normalización y -6 dB.
4. Reducir B a los primeros 1024 o 512 coeficientes, manteniendo A128 y PRE/POST/P/K.
5. Renderizar ese modelo compacto para compararlo con el NAM.
6. Crear dos candidatos independientes desde ese mismo compacto:
   - sin confianza: corrección en dB = target − source;
   - con confianza: corrección en dB = (target − source) × confianza espectral.
7. Sintetizar la IR mínimo-fase de cada candidato, convolucionarla con B, conservar sólo B1024/B512 y normalizar el RMS de B como hacía Tone Match (0 dB post gain).
8. Renderizar y puntuar ambos candidatos finales. Guardar el de menor error, sin recortes o normalizaciones posteriores.

El suavizado del 5 % se ha eliminado; tampoco se aplica otro suavizado en su lugar. La confianza sólo modifica la curva del segundo candidato. Suavizado y confianza son operaciones distintas.

La selección se aplica a archivos locales, NAM descargados de Tone3000 y lotes: todos comparten la misma ruta. El preview Tone3000 no cambia.

## Qué significa «mejor»

Menor RMSE espectral en dB, sin ponderación por confianza ni ajuste escalar de nivel. Se usan los mismos 512 puntos logarítmicos de 40 a 18000 Hz y las mismas ventanas para ambos candidatos.

Las ventanas se fijan con el compacto inicial y el NAM; se descartan las que no contienen suficiente señal en ambas cadenas. Un candidato no puede mejorar la puntuación haciendo que desaparezcan de la evaluación sus ventanas difíciles. Los 600 samples de guard del trainer no forman parte del intervalo 50–70 s.

La confianza conserva el cálculo previo (energía, grupos y dispersión MAD) y toma el mínimo entre source y target. En la variante ponderada, confianza 0 lleva la corrección a 0 dB; confianza 1 mantiene toda la corrección.

Los dos candidatos se evalúan sobre la misma referencia utilizada para el ajuste; esta versión no añade un conjunto de audio independiente de validación ni afirma superioridad perceptual universal. El motor CLO utilizado para evaluar conserva el wrapper histórico Gain=50/Volume=50 de la versión anterior, también para la selección GP-5; su equivalencia física GP-5 requiere comprobación. No se ha inventado una curva de ganancia específica de GP-5.

Empate: gana el candidato sin confianza. Si uno no es numéricamente válido, gana el otro; si ninguno es válido, la conversión falla sin publicar un CLO. El compacto sin Tone Match sólo se mide como diagnóstico: no es un tercer candidato. Si ambos candidatos empeoran el baseline, se sigue eligiendo el mejor de los dos, conforme al flujo solicitado.

Se conserva el RMS de los coeficientes B en la etapa de Tone Match. Por ello, no se ha añadido una optimización independiente del nivel global. Se compara el comportamiento real resultante de esa normalización, no la curva ideal antes de aplicarla.

## Corrective IR y referencia

La Corrective IR opcional sigue operando sobre B2048, con el código de v2.9.15 intacto. Cuando está activa, al NAM objetivo se le aplica la misma IR y ganancia efectiva: la comparación sigue siendo NAM+IR frente a CLO+IR. No se elimina la IR en Tone Match.

La referencia custom conserva sus primeros 20 s, insertados en el estímulo; se renderiza ese mismo estímulo para las dos cadenas. La referencia por defecto utiliza el tail del estímulo de conversión.

## Salidas

- `<nombre>_NATIVE_GP200_1024_TONEMATCH.clo`
- `<nombre>_NATIVE_GP5_512_TONEMATCH.clo`

GP-200 conserva 0x2288 bytes físicos, 0x1288 declarados y padding cero, compatible con el uploader actual. GP-5 se escribe con 0x0A88 bytes físicos/declarados. Ambos CRC finales siguen la convención high-byte-first de los compactos existentes.

La ventana final de conversión individual muestra el destino, el candidato seleccionado y los errores con/sin confianza. Durante los lotes, el estado informa del candidato elegido por cada conversión.

## Archivos y compatibilidad de código

`CloRefineConfig` deja de tener `enabled` y `passes`: Tone Match ahora es obligatorio y prueba exactamente las dos variantes. Añade `destination`. `ConversionResult` sustituye el campo `gp2001024` por `outputClo`, e incluye destino y estadísticas. Se incluyen juntos headers, implementaciones y GUI actualizada.

Se separa el identificador Win32 del selector de referencia Tone Match del identificador de Cabinet IR: ambos eran 155. El nuevo selector de destino usa 159 y el selector de referencia 160.

Los workflows ejecutan las pruebas sintéticas y empaquetan con `cmake --install`. La instalación incluye el WAV de estímulo obligatorio, omitido en los workflows anteriores. No hay cambios en los protocolos MIDI ni en los uploaders.

## Verificación realizada

- `clo_refiner.cpp` compilado con g++ C++20 y avisos `-Wall -Wextra -Wpedantic`.
- Pruebas sintéticas que ejecutan el refiner real para B1024 y B512.
- Verificados CRC, tamaños, padding, preservación de A/P/K/PRE/POST y exclusión del guard.
- Relectura y render del archivo exportado: su puntuación coincide con la del candidato ganador.
- Pruebas de confianza nula, coeficientes no finitos, entrada corta y ventanas comunes.
- Preparadores originales de subida: aceptan el nuevo GP-200 en su uploader y el nuevo GP-5 en el suyo; GP-5 genera 146 chunks.
- Trainer y serializador original B2048, anteriores a `convertNamToClo()`, idénticos byte a byte a v2.9.15. `corrective_ir.cpp` también idéntico.

Ejemplo sintético de filtro objetivo corto:

| Destino | Baseline RMSE dB | Sin confianza | Con confianza | Ganador |
|---|---:|---:|---:|---|
| B1024 | 1.30271 | 0.317889 | 0.465301 | Sin confianza |
| B512 | 1.30271 | 0.317832 | 0.464930 | Sin confianza |

Otro caso sintético con coeficiente objetivo retardado 900 muestras:

| Destino | Baseline RMSE dB | Sin confianza | Con confianza | Ganador |
|---|---:|---:|---:|---|
| B1024 | 1.23719 | 0.282192 | 0.490365 | Sin confianza |
| B512 | 1.23719 | 1.001350 | 1.033930 | Sin confianza |

Son pruebas de funcionamiento con un modelo sintético, no conversiones de NAM reales ni resultados de escucha. Los valores pueden variar ligeramente según compilador/plataforma.

No se ha compilado la GUI/aplicación completa para Windows en este entorno, ni ejecutado una conversión NAMCore completa o una subida física. Se entrega código fuente, no EXE. Los workflows incluyen compilación y pruebas para realizarlas en Windows.

## Compilación en Windows

Desde la carpeta del proyecto, con CMake y Visual Studio x64 instalados:

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
cmake --install build --config Release --prefix dist
```

El ejecutable y el WAV necesario quedarán en `dist`. Las dependencias conservan la configuración del proyecto anterior.

Pruebas aisladas, sin NAMCore ni dispositivo:

```powershell
cmake -S tests -B build-tests -A x64
cmake --build build-tests --config Release
ctest --test-dir build-tests -C Release --output-on-failure
```

## Instalación del paquete de cambios

Usar el ZIP completo como copia independiente de v2.10.0, o extraer el ZIP de archivos modificados sobre la raíz de una copia de v2.9.15, sustituyendo todos los archivos incluidos. No mezclar sólo el `.cpp` con headers o GUI antiguos. El archivo `CHANGED_FILES.txt` enumera las diferencias.

Antes de dar la versión por validada en hardware: compilar en Windows, probar ambos destinos con el mismo NAM, repetir con Corrective IR y referencia custom, escuchar y comprobar la subida. Conservar v2.9.15 para comparación.
