# NamToClo v2.10.1 — Tone Match directo

## Cambio solicitado

Se elimina el cálculo de confianza espectral, su ponderación, la segunda variante de Tone Match y la selección del ganador. Se mantiene la ausencia de suavizado del 5 %.

Flujo: conversión original A128/B2048 → Corrective IR opcional sobre B2048 → reducción a B1024 (GP-200) o B512 (GP-5) → Tone Match directo sobre ese tamaño → exportación final.

La curva de corrección sigue siendo target − source en dB. Se conserva el estimador de potencia mediana por grupos: la mediana es parte del espectro original, no un indicador de confianza ni un suavizado entre frecuencias. Se han retirado el cálculo MAD y los factores de confianza por energía/grupos/estabilidad.

Se conserva la IR mínimo-fase, la convolución y normalización RMS del bloque B, así como el tratamiento de nivel histórico de Corrective IR. No se añade una EQ específica de agudos.

La observación de más agudos con confianza es una observación de escucha del usuario, no una causa físicamente demostrada en esta revisión.

## Interfaz y resultados

La sección muestra “Tone Match (direct correction)”. La ventana final ya no muestra variantes ni ganador; sólo el error espectral final como diagnóstico. Esa medición no compara opciones ni selecciona un resultado distinto.

Se conserva el selector inicial GP-200/GP-5. Los nombres y tamaños de salida son los mismos que en v2.10.0. Tone3000 y los lotes utilizan el mismo flujo.

## Validación

Pruebas sintéticas sobre el refiner real para B1024 y B512: tamaños, CRC, padding, A/P/K/biquads conservados y coincidencia entre el resultado medido y el archivo exportado. Prueba de regresión contra la ruta directa de v2.10.0 con la misma entrada sintética. No se ha compilado la aplicación completa en Windows ni probado hardware o NAM reales en este entorno.

## Instalación y compilación

El ZIP de cambios se aplica sobre una copia completa de v2.10.0, reemplazando todos los archivos incluidos. Los headers, implementación y GUI deben actualizarse juntos. CHANGED_FILES.txt enumera las diferencias. El ZIP completo incluye el proyecto independiente.

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
cmake --install build --config Release --prefix dist
```

Pruebas aisladas:

```powershell
cmake -S tests -B build-tests -A x64
cmake --build build-tests --config Release
ctest --test-dir build-tests -C Release --output-on-failure
```

Se entregan fuentes, no un ejecutable Windows. VERSION_2.10.0.md se conserva sólo como historial.
