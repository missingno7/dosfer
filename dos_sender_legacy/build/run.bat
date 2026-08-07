@echo off
rem Clean legacy sender is fixed to V40 and 320x200.
rem Default video timing is custom ~59.94 Hz.
DOSFER.EXE /WINDOW:64 /HOLD:50 tst.zip

rem Original Mode 0Dh ~70 Hz timing:
rem DOSFER.EXE /VIDEO:320_70 /WINDOW:64 /HOLD:50 tst.zip
