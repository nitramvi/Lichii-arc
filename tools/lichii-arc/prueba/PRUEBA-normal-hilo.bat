@echo off
REM OoT: calidad con precision Normal + hilo de render (prueba para bajar el uso de CPU).
set "LICHII_OOT_PRESET=calidad"
set "LICHII_OOT_GPU_ACC=normal"
set "EDEN_NX2_RENDER_FOLLOWER=1"
call "%~dp0PRUEBA.bat"
