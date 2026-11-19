Versión de prueba diaria **20261119** de Lichii-arc (rama `diaria-20261119`, basada en `mark-arc`).

Archivos:
- `lichii-diaria-20261119.exe`: copialo a la carpeta de Lichii-arc en la Claw (al lado de `lichii-menu3.exe`). No reemplaza nada.
- `PRUEBA-diaria-20261119.bat`: toma la configuración de `OoT-Lichii-Arc.bat`, suma las variables nuevas y lanza el exe de prueba.

Qué prueba (todo apagado por defecto, solo lo prende el bat de prueba):
- `LICHII_COMPUTE_SKIP=1` — port de upstream #2104: los compute dispatches de Switch 2 con el pipeline todavía compilando siguen la regla de los dibujos (Bounded) en vez de congelar el frame, solo si sus salidas ya tienen datos de GPU.
- `LICHII_DRAW_HISTORY_GUARD=1` — mismo PR: un dibujo nativo solo se saltea si el test de profundidad está prendido y sus render targets ya existían en un frame anterior.

En el log buscá `LICHII_COMPUTE_SKIP`, `compute dispatches skipped` y `Bounded pipeline wait`.
