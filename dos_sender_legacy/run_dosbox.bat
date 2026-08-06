@echo off
if "%DOSBOX%"=="" set DOSBOX=dosbox
%DOSBOX% -c "mount c %CD%" -c "c:" -c "build\DOSFER.EXE /CAL"

