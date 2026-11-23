@echo off
REM ==========================================================================
REM  Variables de la prueba de Lichii-arc (1 = prendida, 0 = apagada).
REM  Lo llama PRUEBA.bat despues de copiar la configuracion del bat principal.
REM  Se baja solo del release prueba-claw junto con cada exe nuevo.
REM ==========================================================================
set "LICHII_COMPUTE_SKIP=1"
set "LICHII_DRAW_HISTORY_GUARD=1"
set "LICHII_TC_THRASH_GUARD=1"
set "LICHII_STAGING_PREWARM=1"
set "LICHII_MEM_PREWARM=0"
set "LICHII_OOT_TEX_GAP=1"
REM 7) (noche) LICHII_ZERO_TRIM_CAP=1: la limpieza de memoria en ceros de Windows se hace en
REM    pedazos de 1 MiB (upstream #2233, de Fritchii): una sola llamada larga frenaba al juego.
set "LICHII_ZERO_TRIM_CAP=1"
REM 8) (madrugada) LICHII_FILE_LOG=1: anota en el log las carpetas del juego y los archivos de
REM    voces/sonido/idioma que lee, para ver por que salen voces en ingles. Solo diagnostico.
set "LICHII_FILE_LOG=1"
REM 9) (22/11) LICHII_S2_FIXED_COUNTER=1: en juegos de Switch 2 el reloj del juego no se acelera
REM    con CPU Boost/Overclock (de Orchid/mariohbd). Sin efecto con el reloj de CPU normal.
set "LICHII_S2_FIXED_COUNTER=1"
REM 10) (23/11 tarde) Escala de OoT ~720p CON XeSS: LICHII_OOT_RENDER_SCALE=0.667 dibuja la escena a
REM    1308x736 (era 1440x810) y LICHII_OOT_AUTO_PASS=1 reconoce el pase de agrandado del juego a esa
REM    escala para que XeSS lo reemplace (antes quedaba el escalado del juego, feo y lento).
REM    En el log tiene que aparecer "LICHII_UPSCALE_LEARNED" y despues "XeSS evaluated".
REM    SI SE VE PEOR O MAS LENTO: cambiar 0.667 por performance (la escala del bat principal).
set "LICHII_OOT_RENDER_SCALE=0.667"
set "LICHII_OOT_ALLOW_PADDING=1"
set "LICHII_OOT_AUTO_PASS=1"
REM 12) (23/11 tarde) LICHII_IMAGE_REAPER=1: las texturas que se borran se destruyen en un hilo
REM    aparte y no en el hilo de la GPU (de conongnang/Longshot: menos tirones al girar la camara y
REM    al entrar a zonas). 0 = como antes.
set "LICHII_IMAGE_REAPER=1"
REM 13) (23/11 noche) Menos CPU por cuadro (la Claw en OoT va con la CPU al ~100 %):
REM    LICHII_HOT_PAGES=1: las paginas que el juego reescribe en cada cuadro dejan de protegerse
REM    contra escritura (OoT: ~25.000 llamadas al sistema por segundo menos; -5 %% de tiempo por
REM    cuadro medido en PC). LICHII_GUEST_FAST=1: las escrituras del juego en memoria que la GPU
REM    nunca escribio no esperan el candado que el hilo de la GPU tiene mientras dibuja (las
REM    esperas "buffer" del log). Los dos de conongnang/Lichii y Fritchii. 0 = como antes.
REM    SI APARECEN TEXTURAS O GEOMETRIA ROTA: poner los dos en 0.
set "LICHII_HOT_PAGES=1"
set "LICHII_GUEST_FAST=1"
REM 11) (23/11) Generacion de cuadros LSFG x2 (Lossless Scaling, la misma del bat de 60 fps): el
REM    juego corre a lo que da y se ven el doble de cuadros. Solo en la config de esta sesion: el bat
REM    principal la vuelve a apagar cuando lo abris.
powershell -NoProfile -Command "$f='user\config\qt-config.ini'; (Get-Content $f) -replace '^frame_gen\\default=.*','frame_gen\default=false' -replace '^frame_gen=.*','frame_gen=true' -replace '^frame_gen_type\\default=.*','frame_gen_type\default=false' -replace '^frame_gen_type=.*','frame_gen_type=0' -replace '^frame_gen_multiplier\\default=.*','frame_gen_multiplier\default=false' -replace '^frame_gen_multiplier=.*','frame_gen_multiplier=2' | Set-Content $f"
