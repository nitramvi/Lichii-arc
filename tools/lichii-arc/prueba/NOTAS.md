# Prueba Claw (rama `prueba`)

Una sola rama y un solo release (`prueba-claw`). En la Claw: `PRUEBA.bat` + `lichii-prueba.exe`; el bat baja solo cada versión nueva.

## Esta versión (22/11)

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
