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
REM  configuracion), suma las variables de prueba (PRUEBA-vars.bat, que se
REM  actualiza sola con cada version) y lanza lichii-prueba.exe
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
  curl.exe -s -L --fail -o "%~dp0PRUEBA-vars.bat.tmp" "%PRUEBA_URL%/PRUEBA-vars.bat" && move /y "%~dp0PRUEBA-vars.bat.tmp" "%~dp0PRUEBA-vars.bat" >nul
  curl.exe -L --fail -o "%~dp0%PRUEBA_EXE%.tmp" "%PRUEBA_URL%/lichii-prueba.exe" && move /y "%~dp0%PRUEBA_EXE%.tmp" "%~dp0%PRUEBA_EXE%" >nul && (echo %PRUEBA_HAY%)>"%~dp0lichii-prueba.version"
)
if not exist "%~dp0PRUEBA-vars.bat" curl.exe -s -L --fail -o "%~dp0PRUEBA-vars.bat" "%PRUEBA_URL%/PRUEBA-vars.bat"
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
REM --- FSR 4 (fsr4-vk): se baja a user\fidelityfx la primera vez y cuando cambia ---
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

REM --- 2) Variables de esta prueba: estan en PRUEBA-vars.bat, que se baja junto con el exe ---
if exist "%~dp0PRUEBA-vars.bat" call "%~dp0PRUEBA-vars.bat"

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
