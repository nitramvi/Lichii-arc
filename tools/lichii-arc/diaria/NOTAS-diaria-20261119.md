Versión de prueba diaria **20261119** de Lichii-arc (rama `diaria-20261119`, basada en `mark-arc`).

Archivos:
- `lichii-diaria-20261119.exe`: copialo a la carpeta de Lichii-arc en la Claw (al lado de `lichii-menu3.exe`). No reemplaza nada.
- `PRUEBA-diaria-20261119.bat`: toma la configuración de `OoT-Lichii-Arc.bat`, suma las variables nuevas y lanza el exe de prueba.

Qué prueba (todo apagado por defecto, solo lo prende el bat de prueba):
- `LICHII_COMPUTE_SKIP=1` — port de upstream #2104: los compute dispatches de Switch 2 con el pipeline todavía compilando siguen la regla de los dibujos (Bounded) en vez de congelar el frame, solo si sus salidas ya tienen datos de GPU.
- `LICHII_DRAW_HISTORY_GUARD=1` — mismo PR: un dibujo nativo solo se saltea si el test de profundidad está prendido y sus render targets ya existían en un frame anterior.

En el log buscá `LICHII_COMPUTE_SKIP`, `compute dispatches skipped` y `Bounded pipeline wait`.

**Corrida 12:47 (pedido de Pedro: mirar otros repos y recomps)**
- `LICHII_TC_THRASH_GUARD=1` — idea de Fritchii (Frizti, `7994ea8760`, FRITCHII_TC_THRASH_GUARD) rehecha para nuestro recolector: el log de la Claw muestra el commit de Windows en 1,6–2,5 GB libres, lo que baja el umbral crítico de texturas; el recolector borra 40 imágenes por frame sin uso hace 10 frames y al girar la cámara se vuelven a crear (frames de 130–480 ms). Ahora se recuerdan las borradas y, si vuelven rápido, el recolector respeta una edad mínima (hasta 600 frames). `=2` solo cuenta. Log: `LICHII_TC_THRASH_GUARD=` cada 600 frames.
- `LICHII_STAGING_PREWARM=1` (ya en mark-arc) y `LICHII_MEM_PREWARM=0` (libera 1,7 GB de commit).

**En la Claw se usan nombres fijos**: `lichii-prueba.exe` y `PRUEBA.bat` (cada prueba pisa la anterior; `lichii-prueba.version` dice de qué release es el exe y el bat baja el nuevo si no coincide). En el release los archivos siguen con fecha.
