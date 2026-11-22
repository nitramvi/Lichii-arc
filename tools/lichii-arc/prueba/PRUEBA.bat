@echo off
setlocal EnableExtensions DisableDelayedExpansion
REM ==========================================================================
REM  PRUEBA - Lichii-arc (MSI Claw 8 AI+, Arc 140V)
REM
REM  Siempre el mismo bat y el mismo exe (lichii-prueba.exe). Cada vez que la
REM  rama "prueba" compila una version nueva, este bat la baja sola al abrirlo
REM  (compara lichii-prueba.version con la del release "prueba-claw").
REM  Que trae cada version: notas del release prueba-claw en GitHub.
REM
REM  Este bat NO modifica OoT-Lichii-Arc.bat: copia sus lineas "set" (la misma
REM  configuracion), suma las variables de prueba y lanza lichii-prueba.exe
REM  en lugar de lichii-menu3.exe. Va en la misma carpeta que esos archivos.
REM ==========================================================================
cd /d "%~dp0"
set "PRUEBA_EXE=lichii-prueba.exe"
set "PRUEBA_BASE=%~dp0OoT-Lichii-Arc.bat"
set "PRUEBA_URL=https://github.com/nitramvi/Lichii-arc/releases/download/prueba-claw"
set "PRUEBA_TIENE="
if exist "%~dp0lichii-prueba.version" set /p PRUEBA_TIENE=<"%~dp0lichii-prueba.version"
if not exist "%~dp0%PRUEBA_EXE%" set "PRUEBA_TIENE="
set "PRUEBA_HAY="
curl.exe -s -L --fail -o "%~dp0lichii-prueba.version.tmp" "%PRUEBA_URL%/lichii-prueba.version" && set /p PRUEBA_HAY=<"%~dp0lichii-prueba.version.tmp"
if exist "%~dp0lichii-prueba.version.tmp" del "%~dp0lichii-prueba.version.tmp"
if defined PRUEBA_HAY if not "%PRUEBA_TIENE%"=="%PRUEBA_HAY%" (
  echo Bajando la version nueva de prueba: %PRUEBA_HAY%
  curl.exe -L --fail -o "%~dp0%PRUEBA_EXE%.tmp" "%PRUEBA_URL%/lichii-prueba.exe" && move /y "%~dp0%PRUEBA_EXE%.tmp" "%~dp0%PRUEBA_EXE%" >nul && (echo %PRUEBA_HAY%)>"%~dp0lichii-prueba.version"
)
if not exist "%~dp0%PRUEBA_EXE%" (
  echo No pude bajar el exe de prueba. Bajalo a mano del release prueba-claw, renombralo a %PRUEBA_EXE% y copialo a %~dp0
  pause
  exit /b 1
)
if not exist "%PRUEBA_BASE%" (
  echo No encuentro OoT-Lichii-Arc.bat en esta carpeta: lanzo el exe sin configuracion extra.
  goto :nuevas
)

REM --- 1) La misma configuracion que el bat principal (sus lineas "set") ---
for /f "usebackq tokens=* delims=" %%L in (`findstr /r /i /c:"^[ 	]*set[ 	][ 	]*[^/]" "%PRUEBA_BASE%"`) do (
  set "PRUEBA_LINEA=%%L"
  call :aplicar
)

:nuevas
REM --- 2) Variables nuevas de esta prueba (1 = prendida, 0 = apagada) ---
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

REM --- 3) Lanzar con los mismos argumentos que el bat principal ---
set "PRUEBA_LANZAR="
if exist "%PRUEBA_BASE%" (
  for /f "usebackq tokens=* delims=" %%L in (`findstr /i /c:"lichii-menu3.exe" "%PRUEBA_BASE%" ^| findstr /v /i /r /c:"^[ 	]*rem" /c:"^[ 	]*::" /c:"^[ 	]*echo" /c:"^[ 	]*set[ 	]" /c:"^[ 	]*if[ 	]"`) do (
    if not defined PRUEBA_LANZAR set "PRUEBA_LANZAR=%%L"
  )
)
if not defined PRUEBA_LANZAR (
  echo No encontre la linea que lanza lichii-menu3.exe: abro %PRUEBA_EXE% solo, elegi OoT en la lista.
  start "" "%~dp0%PRUEBA_EXE%"
  goto :fin
)
call set "PRUEBA_LANZAR=%%PRUEBA_LANZAR:lichii-menu3.exe=%PRUEBA_EXE%%%"
echo Lanzando: %PRUEBA_LANZAR%
call %PRUEBA_LANZAR%
goto :fin

:aplicar
REM Si una variable apunta al exe principal, que apunte al de la prueba.
call set "PRUEBA_LINEA=%%PRUEBA_LINEA:lichii-menu3.exe=%PRUEBA_EXE%%%"
call %PRUEBA_LINEA%
exit /b 0

:fin
endlocal
