# Prueba Claw (rama `prueba`)

Una sola rama y un solo release (`prueba-claw`). En la Claw: `PRUEBA.bat` + `lichii-prueba.exe`; el bat baja solo cada versión nueva.

## Esta versión (22/11, tarde)

Lichii-arc al día con Lichii upstream (nx2 al 22/11 14:00 UTC, 17 commits nuevos), conservando GPL, LICHII_SMART_FAST, LICHII_WARM_TESS, XeSS, render scale y FSR 1440 de OoT, thrash guard, staging prewarm y LICHII_S2_FIXED_COUNTER.

Nuevo de upstream (para todos los juegos):
- #2421: en GPUs sin formato de profundidad D24 (caso Intel Arc) los sombreadores redondean la profundidad a valores D24 como en la Switch: menos z-fighting, sombras con rayas y objetos que parpadean/atraviesan.
- #2361: Windows conserva la protección de memoria rápida (fastmem) al liberar memoria del juego en partes: menos cuelgues/corrupción raros en juegos que mapean y desmapean mucho.
- #2415: el GPU retoma escrituras de registros partidas entre llamadas GPFIFO (gráficos rotos ocasionales).
- #2386: frame gen Optical Flow con búsqueda de bloques gruesa→fina (menos artefactos si usás frame gen).
- #2435 Xenoblade 2 (tablas de color), #2416 Hyrule Warriors (sombreador de decals), #2411 Wonder S2 Edition a 60 fps integrado, #2436 OpenGL, #2420 archivos en Windows.
- #2425: hilo de render separado (estilo Fritchii) — **apagado** (EDEN_NX2_RENDER_FOLLOWER); no se prende en la prueba porque no está probado con GPL.

Qué probar: OoT (sesión normal; mirar sombras, pasto y objetos lejanos que antes parpadeaban) y cualquier otro juego que tuviera parpadeo de geometría o z-fighting. Si usás frame gen, compararlo.

## Versión anterior (22/11, madrugada)

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
