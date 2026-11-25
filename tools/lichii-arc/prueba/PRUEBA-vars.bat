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
REM    (23/11 13:30) AUTO_PASS en 0: Pedro prefiere el escalado del juego (ver 12).
set "LICHII_OOT_RENDER_SCALE=performance"
set "LICHII_OOT_ALLOW_PADDING=0"
set "LICHII_OOT_AUTO_PASS=0"
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
REM 14) (24/11) LICHII_TC_NO_GPU_WAIT=1: el recolector de texturas ya no borra imagenes que dibujo la
REM    GPU (cada una obliga a esperar a la GPU para copiarla de vuelta: las esperas "texture" del
REM    log) salvo pasando el umbral critico de memoria; primero borra las que se tiran sin copiar.
REM    Port de conongnang/Lichii (d7b86404f2). 0 = como antes. Si sube mucho la RAM: ponerlo en 0.
set "LICHII_TC_NO_GPU_WAIT=1"
REM 16) (24/11 tarde) LICHII_BOOT_DRIVER_THREADS=1: en "Loading Shaders" el driver compila con todos
REM    los hilos (antes la mitad) hasta que arranca el juego; despues vuelve a lo de siempre. Solo
REM    acorta la carga de shaders (de conongnang/Lichii f6612f5a6). 0 = como antes.
set "LICHII_BOOT_DRIVER_THREADS=1"
REM 17) (24/11 tarde) LICHII_OOT_FRAME_CARRY=1: con FPS Unlock y menos de 60 fps, el reloj del dia y
REM    la telarana del Arbol Deku ya no pierden las fracciones de cada cuadro (a 46 fps el dia iba al
REM    77 %% de la velocidad real; a 23-27 fps tambien se atrasa). De conongnang/Lichii 647d88045
REM    (Longshot). No cambia fps. 0 = como antes.
set "LICHII_OOT_FRAME_CARRY=1"
REM 19) (25/11) Menos trabajo de GPU por cuadro (OoT en la Claw esta limitado por la GPU, ~38 ms):
REM    LICHII_QUERY_WAIT_LAST=1: al copiar los resultados de las consultas de oclusion se espera solo
REM    la ultima de cada tanda y no cada una (como ya se hacia en AMD; ~1,6 ms por cuadro en la traza).
REM    LICHII_PRESENT_SKIP_1TO1=1: si la imagen ya llega del tamano de la pantalla (FSR 4/XeSS a 1080p),
REM    no se pasa otra vez por el FSR 1 del present (~0,7 ms; pierde solo el afilado extra del present).
REM    0 = como antes. Si aparecen objetos que parpadean o desaparecen: LICHII_QUERY_WAIT_LAST=0.
set "LICHII_QUERY_WAIT_LAST=1"
set "LICHII_PRESENT_SKIP_1TO1=1"
REM 11) (23/11) Generacion de cuadros LSFG x2 (Lossless Scaling, la misma del bat de 60 fps): el
REM    juego corre a lo que da y se ven el doble de cuadros. Solo en la config de esta sesion: el bat
REM    principal la vuelve a apagar cuando lo abris.
powershell -NoProfile -Command "$f='user\config\qt-config.ini'; (Get-Content $f) -replace '^frame_gen\\default=.*','frame_gen\default=false' -replace '^frame_gen=.*','frame_gen=true' -replace '^frame_gen_type\\default=.*','frame_gen_type\default=false' -replace '^frame_gen_type=.*','frame_gen_type=0' -replace '^frame_gen_multiplier\\default=.*','frame_gen_multiplier\default=false' -replace '^frame_gen_multiplier=.*','frame_gen_multiplier=2' | Set-Content $f"
REM 12) ESCALADOR DE OoT (elegible). Cambia solo esta palabra:
REM      native = el del juego (como la Switch 2: FSR 1 + SMAA; real pero pesado)
REM      fsr3   = AMD FSR 3.1
REM      xess   = Intel XeSS (rapido, se ve "dibujado")
REM      fsr4   = AMD FSR 4.1 por fsr4-vk (version comunitaria para Vulkan; experimental en Intel,
REM               si no arranca vuelve solo a FSR 3.1). PRUEBA.bat baja fsr4-vk.dll a user\fidelityfx.
set "LICHII_OOT_UPSCALER=fsr4"
set "LICHII_FSR4_PORT=0"
REM 16) (24/11) PRECISION DE GPU para bajar CPU: LICHII_OOT_GPU_ACC = alta (la del bat principal) | normal
REM      (menos sincronizacion CPU-GPU; puede haber algun glitch). Se escribe en qt-config.ini en cada
REM      arranque, asi PRUEBA.bat y los otros bats vuelven solos a "alta".
REM      El hilo de render (EDEN_NX2_RENDER_FOLLOWER=1) lo prende PRUEBA-normal-hilo.bat.
if not defined LICHII_OOT_GPU_ACC set "LICHII_OOT_GPU_ACC=alta"
set "PRUEBA_ACC=1"
if /i "%LICHII_OOT_GPU_ACC%"=="normal" set "PRUEBA_ACC=0"
powershell -NoProfile -Command "$f='user\config\qt-config.ini'; (Get-Content $f) -replace '^gpu_accuracy\\default=.*','gpu_accuracy\default=false' -replace '^gpu_accuracy=.*','gpu_accuracy=%PRUEBA_ACC%' | Set-Content $f"
REM 17) (24/11) PRECISION DE CPU: LICHII_OOT_CPU_ACC = auto (la de siempre) | unsafe (el traductor de CPU
REM      se salta mas chequeos; mas rapido, puede haber errores de calculo raros). Se vuelve a "auto" sola.
if not defined LICHII_OOT_CPU_ACC set "LICHII_OOT_CPU_ACC=auto"
set "PRUEBA_CPU=0"
if /i "%LICHII_OOT_CPU_ACC%"=="unsafe" set "PRUEBA_CPU=2"
powershell -NoProfile -Command "$f='user\config\qt-config.ini'; (Get-Content $f) -replace '^cpu_accuracy\\default=.*','cpu_accuracy\default=false' -replace '^cpu_accuracy=.*','cpu_accuracy=%PRUEBA_CPU%' | Set-Content $f"
REM 18) (24/11) RESOLUCION INTERNA DEL EMULADOR: LICHII_OOT_RES = 1x (normal) | 075 (todo a 0,75x: tambien el
REM      pase de 1920x1080 de 32 dibujos del final del juego, ~8 ms de GPU, y la salida de FSR 4). Vuelve sola a 1x.
if not defined LICHII_OOT_RES set "LICHII_OOT_RES=1x"
set "PRUEBA_RES=3"
set "PRUEBA_RES_DEF=true"
if /i "%LICHII_OOT_RES%"=="075" set "PRUEBA_RES=2"
if /i "%LICHII_OOT_RES%"=="075" set "PRUEBA_RES_DEF=false"
powershell -NoProfile -Command "$f='user\config\qt-config.ini'; (Get-Content $f) -replace '^resolution_setup\\default=.*','resolution_setup\default=%PRUEBA_RES_DEF%' -replace '^resolution_setup=.*','resolution_setup=%PRUEBA_RES%' | Set-Content $f"
REM 20) (25/11) CACHE DE TEXTURAS ASTC EN DISCO: la Arc 140V no tiene ASTC y convierte cada textura nueva
REM      en la GPU. Al entrar a una zona/escena nueva (25/11: 240 texturas, congelon de varios segundos)
REM      se guardan ya convertidas; la proxima vez se copian del disco. Hasta 2 GB en user\.
set "LICHII_ASTC_DISK_CACHE=1"
REM 21) (25/11) DIAGNOSTICO: copias de buferes de la GPU por origen, cada 10 s en el log (LICHII_BUF_COPIES).
set "LICHII_BUF_COPY_STATS=1"
REM 15) PRESETS DE OoT (24/11). Si LICHII_OOT_PRESET tiene valor, pisa escalador y escala de arriba:
REM      calidad = FSR 4 a 1440x810 -> 1080p (el mas lindo; ~24-27 fps a bateria)
REM      rapido  = FSR 4 a 960x540  -> 1080p (mas fps, FSR 4 disimula la baja resolucion)
REM      consola = el escalado propio del juego, como la Switch 2 (FSR 1 + SMAA)
REM      dibujo  = XeSS a 960x540 -> 1080p (el mas rapido, se ve "dibujado")
REM      Vacio (set "LICHII_OOT_PRESET=") = usa LICHII_OOT_UPSCALER y LICHII_OOT_RENDER_SCALE sueltos.
REM      Los bats PRUEBA-calidad/rapido/consola/dibujo.bat ya traen el suyo; esta linea es para PRUEBA.bat.
if not defined LICHII_OOT_PRESET set "LICHII_OOT_PRESET=calidad"
if /i "%LICHII_OOT_PRESET%"=="calidad" set "LICHII_OOT_UPSCALER=fsr4"
if /i "%LICHII_OOT_PRESET%"=="calidad" set "LICHII_OOT_RENDER_SCALE=performance"
if /i "%LICHII_OOT_PRESET%"=="rapido" set "LICHII_OOT_UPSCALER=fsr4"
if /i "%LICHII_OOT_PRESET%"=="rapido" set "LICHII_OOT_RENDER_SCALE=ultra"
if /i "%LICHII_OOT_PRESET%"=="consola" set "LICHII_OOT_UPSCALER=native"
if /i "%LICHII_OOT_PRESET%"=="consola" set "LICHII_OOT_RENDER_SCALE=performance"
if /i "%LICHII_OOT_PRESET%"=="dibujo" set "LICHII_OOT_UPSCALER=xess"
if /i "%LICHII_OOT_PRESET%"=="dibujo" set "LICHII_OOT_RENDER_SCALE=ultra"
if /i "%LICHII_OOT_UPSCALER%"=="native" set "EDEN_NX2_DLSS_REPLACEMENT=native"
if /i "%LICHII_OOT_UPSCALER%"=="fsr3" set "EDEN_NX2_DLSS_REPLACEMENT=fsr"
if /i "%LICHII_OOT_UPSCALER%"=="xess" set "EDEN_NX2_DLSS_REPLACEMENT=xess"
if /i "%LICHII_OOT_UPSCALER%"=="fsr4" set "EDEN_NX2_DLSS_REPLACEMENT=fsr"
if /i "%LICHII_OOT_UPSCALER%"=="fsr4" set "LICHII_FSR4_PORT=1"
REM      fsr4: baja fsr4-vk.dll a user\fidelityfx la primera vez y cuando cambia (aca y no en
REM      PRUEBA.bat, porque PRUEBA-vars.bat es lo que se actualiza solo en la Claw)
if /i "%LICHII_OOT_UPSCALER%"=="fsr4" if defined PRUEBA_URL call :bajar_fsr4
goto :eof

:bajar_fsr4
set "PRUEBA_FSR4_DIR=%~dp0user\fidelityfx"
if not exist "%PRUEBA_FSR4_DIR%" mkdir "%PRUEBA_FSR4_DIR%"
set "PRUEBA_FSR4_TIENE="
set "PRUEBA_FSR4_HAY="
if exist "%PRUEBA_FSR4_DIR%\fsr4-vk.dll.sha256" set /p PRUEBA_FSR4_TIENE=<"%PRUEBA_FSR4_DIR%\fsr4-vk.dll.sha256"
if not exist "%PRUEBA_FSR4_DIR%\fsr4-vk.dll" set "PRUEBA_FSR4_TIENE="
curl.exe -s -L --fail -o "%PRUEBA_FSR4_DIR%\fsr4-vk.dll.sha256.tmp" "%PRUEBA_URL%/fsr4-vk.dll.sha256" && set /p PRUEBA_FSR4_HAY=<"%PRUEBA_FSR4_DIR%\fsr4-vk.dll.sha256.tmp"
if defined PRUEBA_FSR4_HAY if not "%PRUEBA_FSR4_TIENE%"=="%PRUEBA_FSR4_HAY%" (
  echo Bajando FSR 4 ^(fsr4-vk^)...
  curl.exe -L --fail -o "%PRUEBA_FSR4_DIR%\fsr4-vk.dll.tmp" "%PRUEBA_URL%/fsr4-vk.dll" && move /y "%PRUEBA_FSR4_DIR%\fsr4-vk.dll.tmp" "%PRUEBA_FSR4_DIR%\fsr4-vk.dll" >nul && move /y "%PRUEBA_FSR4_DIR%\fsr4-vk.dll.sha256.tmp" "%PRUEBA_FSR4_DIR%\fsr4-vk.dll.sha256" >nul
)
if exist "%PRUEBA_FSR4_DIR%\fsr4-vk.dll.sha256.tmp" del "%PRUEBA_FSR4_DIR%\fsr4-vk.dll.sha256.tmp"
goto :eof
