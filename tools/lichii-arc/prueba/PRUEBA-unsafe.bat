@echo off
REM OoT: calidad con precision de CPU Unsafe (prueba para bajar el uso de CPU).
set "LICHII_OOT_PRESET=calidad"
set "LICHII_OOT_CPU_ACC=unsafe"
call "%~dp0PRUEBA.bat"
