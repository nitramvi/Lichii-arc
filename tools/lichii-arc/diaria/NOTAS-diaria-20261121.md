# Prueba diaria 2026-11-21

Lichii-arc puesto al día con Lichii upstream (rama nx2, 295 commits: Tasa port, emulación de mesh shaders por compute, compactación de texturas, arreglos de varios juegos), conservando todo lo nuestro:
GPL, LICHII_SMART_FAST, LICHII_WARM_TESS, XeSS, render scale y FSR 1440 de OoT, thrash guard, staging prewarm.

- Los pipelines gráficos siguen con layouts propios (los necesita GPL); los de compute pasan a la caché de layouts de upstream.
- LICHII_OOT_TEX_GAP=1 ahora enciende el parche de compactación de texturas de upstream.
- La primera carga puede tardar más: cambian versiones de caché de sombreadores.

Mismas variables que la prueba del 19/11.
