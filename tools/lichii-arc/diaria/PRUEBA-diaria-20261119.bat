@echo off
setlocal EnableExtensions DisableDelayedExpansion
REM ==========================================================================
REM  PRUEBA diaria 20261119 - Lichii-arc (MSI Claw 8 AI+, Arc 140V)
REM
REM  Que prueba:
REM   1) LICHII_COMPUTE_SKIP=1 (port de upstream #2104): en un juego de Switch 2,
REM      un "compute dispatch" cuyo shader todavia se esta compilando ya no
REM      congela el juego esperando al driver: sigue la misma regla que los
REM      dibujos (Bounded: espera dentro del presupuesto del frame y despues lo
REM      saltea), pero SOLO si lo que escribe ya tiene datos de la GPU de frames
REM      anteriores (una pasada que el juego repite cada frame). Upstream medio
REM      congelones de 0,8 a 7 s en una corrida de OoT con cache vacia por esto.
REM   2) LICHII_DRAW_HISTORY_GUARD=1 (mismo PR): un dibujo nativo solo se puede
REM      saltear si el test de profundidad esta realmente prendido y todos los
REM      render targets donde pinta ya existian en un frame anterior. Evita que
REM      se pierda para siempre un dibujo que "hornea" una textura una sola vez
REM      (posible causa de manchas/basura tras un corte de camara).
REM   3) (corrida 12:47) LICHII_TC_THRASH_GUARD=1: idea de Fritchii rehecha para
REM      nuestro codigo. En el bosque la memoria libre de Windows (commit) baja a
REM      ~2 GB y el emulador borra 40 texturas por frame que no se usaron hace
REM      solo 10 frames; al girar la camara las vuelve a crear = saltos. Ahora
REM      recuerda las borradas y, si el juego las vuelve a pedir, las deja vivir mas.
REM   4) LICHII_STAGING_PREWARM=1: crea de antemano los bufferes de carga que uso
REM      la sesion anterior (congelon al entrar al bosque). Jugalo DOS veces.
REM   5) LICHII_MEM_PREWARM=0: apaga la reserva de 1,7 GB de memoria del bat
REM      principal, que no achicaba los congelones y le quitaba commit a las texturas.
REM
REM  Como probar: borra (o renombra) la cache de shaders de OoT para forzar
REM  compilacion, entra al Bosque Kokiri y mira la escena de la telarana.
REM  En el log (user\log) busca "LICHII_COMPUTE_SKIP", "compute dispatches
REM  skipped", "Bounded pipeline wait", "LICHII_TC_THRASH_GUARD" (cada 20 s)
REM  y "Staging prewarm". Para un A/B, pone en 0 una de las dos
REM  variables de abajo.
REM
REM  Este bat NO modifica OoT-Lichii-Arc.bat: copia sus lineas "set" (la misma
REM  configuracion), suma las variables nuevas y lanza lichii-diaria-20261119.exe
REM  en lugar de lichii-menu3.exe. Va en la misma carpeta que esos archivos.
REM ==========================================================================
cd /d "%~dp0"
set "PRUEBA_EXE=lichii-diaria-20261119.exe"
set "PRUEBA_BASE=%~dp0OoT-Lichii-Arc.bat"
REM Si falta el exe de prueba, lo baja solo del release (curl viene con Windows 10/11).
if not exist "%~dp0%PRUEBA_EXE%" (
  echo Bajando %PRUEBA_EXE% del release diaria-20261119...
  curl.exe -L --fail -o "%~dp0%PRUEBA_EXE%" "https://github.com/nitramvi/Lichii-arc/releases/download/prueba-20261119/lichii-diaria-20261119.exe"
)
if not exist "%~dp0%PRUEBA_EXE%" (
  echo No pude bajar %PRUEBA_EXE%. Bajalo a mano del release diaria-20261119 y copialo a %~dp0
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
