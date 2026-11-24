@echo off
REM OoT: rapido con traza de GPU por pase (EDEN_DKB_FRAME_TRACE + EDEN_DKB_GPU_SITES): mide en que se va
REM el tiempo de la GPU en cada cuadro. Va un poco mas lento (un cuadro por segundo con marcas).
set "LICHII_OOT_PRESET=rapido"
set "EDEN_DKB_FRAME_TRACE=1"
set "EDEN_DKB_GPU_SITES=1"
call "%~dp0PRUEBA.bat"
