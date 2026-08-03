@echo off
setlocal
set "HERE=%~dp0"
start "DOSFER 386SX-16" "%HERE%app\86Box.exe" -P "%HERE%vm\dosfer-486"

