# Prueba Claw (rama `prueba`)

Una sola rama y un solo release (`prueba-claw`). En la Claw: `PRUEBA.bat` + `lichii-prueba.exe`; el bat baja solo cada versión nueva.

## Esta versión (24/11, mediodía)

**Objetivo: menos esperas de texturas en OoT (y todos los juegos) + upstream al día.** Sigue todo lo anterior (escalador elegible con FSR 4, LSFG x2, HOT_PAGES, GUEST_FAST, image reaper).

Nuevo nuestro (apagado salvo en PRUEBA-vars.bat):
- `LICHII_TC_NO_GPU_WAIT=1`: port de conongnang/Lichii d7b86404f2 (vía 604697d0b1). El recolector de texturas, al pasar el umbral "esperado" de memoria, borraba también imágenes que dibujó la GPU, y cada una obliga a esperar a la GPU para copiarla de vuelta (en OoT, girar la cámara borró 304 de 408 imágenes con 40 esperas). Ahora esas solo se borran pasando el umbral crítico o si falta memoria, y dentro de cada pasada van al final. Apunta a las esperas `texture` del log (~280 ms cada 10 s el 22/11). Puede usar algo más de RAM de video.

Nuevo de upstream (nx2 al 24/11 06:34 UTC, para todos los juegos):
- #2517: los búferes origen de copias sparse se mantienen hasta que la GPU termina (arreglo de corrupción).
- #2502: parar un juego ya no dispara el apagado varias veces.
- #2512 (log de Z24 emulado), #2478, #2418 (LDN), tests y CMake.

Qué probar (con PRUEBA.bat, se actualiza solo):
1. OoT en Kokiri y campo de Hyrule girando la cámara rápido: tirones y "game FPS"; en el log la categoría `texture` (antes ~280 ms por 10 s).
2. Que la RAM no se dispare en sesiones largas (si pasa, `LICHII_TC_NO_GPU_WAIT=0`).
3. Otro juego cualquiera, para descartar regresiones.

## Versión del 23/11, noche

**Objetivo: más FPS en OoT bajando el trabajo de la CPU** (en la Claw OoT va con la CPU al ~100 % y el log muestra ~890 ms de esperas "buffer" cada 10 s). Sigue todo lo de la versión de la tarde (0.667 + XeSS, image reaper, LSFG x2) y upstream al día (nx2 al 23/11 15:45 UTC: #2495 dumps de cuadros, #2498 capturas PNG en el Álbum).

Nuevo nuestro (apagado salvo en PRUEBA-vars.bat):
- `LICHII_HOT_PAGES=1`: port de conongnang/Lichii a9a832442e (kraibse). Después de cada subida a la GPU, Lichii protege contra escritura las páginas copiadas, y la próxima escritura del juego produce una falla que la desprotege: en OoT ~25.000 llamadas al sistema y ~130.000 páginas por segundo. Una página que el juego reescribe dos veces en 128 cuadros pasa a "caliente": queda sin proteger y se vuelve a subir como mucho una vez por cuadro. Medido en PC (Kokiri): cuadro 25,9 → 24,7 ms, CPU del hilo de la GPU 14,5 → 13,8 ms.
- `LICHII_GUEST_FAST=1`: port de conongnang/Lichii 855f205be6 (Fritchii EXP35). Las escrituras del juego en memoria que la GPU nunca escribió ya no esperan el candado del buffer cache que el hilo de la GPU tiene mientras dibuja: se juntan y se aplican antes del próximo dibujo. Medido en PC (Castle Town): caminando 52 → 57 fps y menos tirones. `2` = también páginas que escribió la GPU (más agresivo, no se usa).

Qué probar (con PRUEBA.bat, se actualiza solo):
1. OoT en Kokiri y en el campo de Hyrule: fps de juego (el número de "game FPS" con LSFG), y en el log las esperas `buffer` (antes ~890 ms por 10 s).
2. Que no haya texturas, agua, pasto o geometría rotos ni parpadeos nuevos. Si los hay: en PRUEBA-vars.bat poner `LICHII_HOT_PAGES` y `LICHII_GUEST_FAST` en 0 (de a uno, para saber cuál fue).
3. Algún otro juego (`LICHII_HOT_PAGES` actúa en todos; `LICHII_GUEST_FAST` solo en juegos de Switch 2) para descartar regresiones.

## Versión del 23/11, tarde

**Objetivo: más FPS en OoT con XeSS a ~720p.** Además, Lichii upstream al día (nx2 al 23/11, 15 commits nuevos), conservando GPL, LICHII_SMART_FAST, LICHII_WARM_TESS, XeSS, render scale y FSR 1440 de OoT, thrash guard, staging prewarm y LICHII_S2_FIXED_COUNTER.

Nuevo nuestro (apagado salvo en PRUEBA-vars.bat):
- `LICHII_OOT_AUTO_PASS=1`: con una escala propia (`LICHII_OOT_RENDER_SCALE=0.667`, escena 1308x736) el juego usa un pase de agrandado que Lichii no conocía, y XeSS nunca entraba. Ahora, si en un cuadro con cámara de escena aparece un pase que lee la escena (formato E5B9G9R9, tamaño de la escena) y escribe una imagen 16:9 más grande (B10G11R11), se toma como el pase de agrandado y XeSS lo reemplaza desde el cuadro siguiente. Todas las demás verificaciones siguen; si algo no coincide, queda el pase del juego. Log: `LICHII_UPSCALE_LEARNED`.
- `LICHII_IMAGE_REAPER=1`: las texturas borradas se destruyen en un hilo aparte (port de conongnang/Lichii 493e7fa5, de Longshot #792/#1914: −70 % de tiempo liberando imágenes en el hilo de render, p99 de 10,9 a 3,4 ms al llegar al campo de Hyrule, medido en NVIDIA). Con poca memoria vacía la cola enseguida.

Nuevo de upstream (para todos los juegos, sin variables):
- #2481: corta los envíos a la GPU en los bordes de render pass (port de conongnang/Longshot): la GPU empieza antes.
- #2449: sin trabajo de más del hilo de render cuando está apagado (nuestro caso): menos CPU por dibujo.
- #2215 / #2173: menos trabajo por dibujo en el buffer cache (medido en DK Bananza, ayuda a cualquier juego con mucho dibujo).
- #1640: los búferes presentados se liberan por fences cuando ya se compusieron.
- #2485 / #2488: texturas retiradas se mantienen hasta su tick (arreglo de corrupción/cuelgues), #2464 SMAA/FSR 3 sin bordes viejos, #2459 Mario Kart World, #2470 / #2452 DK Bananza y Kirby, #2480 shaders.

Qué probar (con PRUEBA.bat, se actualiza solo):
1. OoT en Kokiri y en el campo de Hyrule: fps (con LSFG x2), nitidez comparada con la escala del bat principal, y si en el log aparecen `LICHII_UPSCALE_LEARNED` y `XeSS evaluated`.
2. Tirones al girar la cámara rápido y al entrar/salir de zonas (image reaper).
3. Algún otro juego que uses, para ver que nada se rompió.
- Si 0.667 se ve peor o más lento: en PRUEBA-vars.bat cambiar `0.667` por `performance`.

## Versión anterior (22/11, tarde)

Lichii-arc al día con Lichii upstream (nx2 al 22/11 14:00 UTC, 17 commits nuevos), conservando GPL, LICHII_SMART_FAST, LICHII_WARM_TESS, XeSS, render scale y FSR 1440 de OoT, thrash guard, staging prewarm y LICHII_S2_FIXED_COUNTER.

Nuevo de upstream (para todos los juegos):
- #2421: en GPUs sin formato de profundidad D24 (caso Intel Arc) los sombreadores redondean la profundidad a valores D24 como en la Switch: menos z-fighting, sombras con rayas y objetos que parpadean/atraviesan.
- #2361: Windows conserva la protección de memoria rápida (fastmem) al liberar memoria del juego en partes: menos cuelgues/corrupción raros en juegos que mapean y desmapean mucho.
- #2415: el GPU retoma escrituras de registros partidas entre llamadas GPFIFO (gráficos rotos ocasionales).
- #2386: frame gen Optical Flow con búsqueda de bloques gruesa→fina (menos artefactos si usás frame gen).
- #2435 Xenoblade 2 (tablas de color), #2416 Hyrule Warriors (sombreador de decals), #2411 Wonder S2 Edition a 60 fps integrado, #2436 OpenGL, #2420 archivos en Windows.
- #2425: hilo de render separado (estilo Fritchii) — **apagado** (EDEN_NX2_RENDER_FOLLOWER); no se prende en la prueba porque no está probado con GPL.

Qué probar: OoT (sesión normal; mirar sombras, pasto y objetos lejanos que antes parpadeaban) y cualquier otro juego que tuviera parpadeo de geometría o z-fighting. Si usás frame gen, compararlo.

## Versión del 22/11, madrugada

Lichii-arc al día con Lichii upstream (nx2 al 22/11), conservando GPL, LICHII_SMART_FAST, LICHII_WARM_TESS, XeSS, render scale y FSR 1440 de OoT, thrash guard y staging prewarm.

Nuevo de upstream (para todos los juegos):
- #2414: la copia de texturas lineales ya no escribe más allá de su destino (cuelgues/corrupción en texturas lineales).
- #2393: las texturas ASTC grandes se decodifican por tandas que reusan la memoria libre (en la Claw ya iba por tandas; ahora reusa mejor la imagen).
- #2396: espera a la GPU en tramos + vigía opcional de GPU colgada (apagado por defecto).
- #2406: FlyHigh (frame gen neuronal) como opción de Windows: solo si lo elegís a mano en frame gen; el `flyhigh.dll` no viene en el release de prueba, así que ahí cae solo a Optical Flow.
- #2405: Mod Store (menú nuevo, no cambia nada si no lo abrís).

Nuevo nuestro (apagado salvo en PRUEBA.bat):
- `LICHII_S2_FIXED_COUNTER=1`: en juegos de Switch 2 el contador del juego no se acelera con CPU Boost/Overclock (port de mariohbd/Lichii@8d2746e8 vía Orchid). Solo cambia algo si usás Boost/Overclock.

Qué probar: OoT (sesión normal, ver que no haya texturas rotas) y algún juego de Switch 2 con CPU Boost si lo usás (velocidad del juego/animaciones normales).

- La primera carga puede tardar más: cambian versiones de caché de sombreadores.
- LICHII_OOT_TEX_GAP=1 enciende el parche de compactación de texturas de upstream.
