@echo off
REM OoT: calidad con resolucion interna del emulador a 0,75x (prueba de GPU: achica el pase final de 1080p).
set "LICHII_OOT_PRESET=calidad"
set "LICHII_OOT_RES=075"
call "%~dp0PRUEBA.bat"
