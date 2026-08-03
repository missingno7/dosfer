param([string]$DosBox = 'C:\DOSBox-X\dosbox-x.exe')
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$stage = Join-Path $root 'build\optical_test'
New-Item -ItemType Directory -Force $stage | Out-Null
Copy-Item (Join-Path $root 'dos_sender\build\DOSFER.EXE') $stage -Force
Copy-Item (Join-Path $root 'test_vectors\SAMPLE.TXT') $stage -Force
if (-not (Test-Path $DosBox)) { throw "DOSBox-X not found: $DosBox" }
$arguments = "-fastlaunch -c `"mount d $stage`" -c d: -c `"DOSFER.EXE SAMPLE.TXT`""
Start-Process $DosBox -ArgumentList $arguments
