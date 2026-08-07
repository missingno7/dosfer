@echo off
rem Clean legacy sender is fixed to V40 and 320x200.
rem Default video timing is custom ~59.94 Hz.


rem Original Mode 0Dh ~70 Hz timing:
rem DOSFER.EXE /VIDEO:320_70 /WINDOW:64 /HOLD:50 tst.zip

rem DOSFER.EXE /WINDOW:66 /HOLD:50 /RGB3 tst.zip


DOSFER.EXE /RGB3 /HOLD:100 /WINDOW:66 tst.zip