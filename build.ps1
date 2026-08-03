$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$watcom = if ($env:WATCOM) { $env:WATCOM } elseif (Test-Path 'C:\WATCOM') { 'C:\WATCOM' } else { $null }
if (-not $watcom) { throw 'Set WATCOM to an Open Watcom 2 installation.' }
$env:WATCOM = $watcom
Push-Location "$root\dos_sender"
try { & cmd /c build.bat; if ($LASTEXITCODE) { throw 'DOS build failed' } } finally { Pop-Location }
Push-Location "$root\android_receiver"
try { & .\gradlew.bat :app:testDebugUnitTest :app:assembleDebug --no-daemon --console=plain; if ($LASTEXITCODE) { throw 'Android build failed' } } finally { Pop-Location }
& python "$root\tools\generate_vectors.py" --check
& python -m unittest discover -s "$root\tools\tests" -v
$out = "$root\build\artifacts"
New-Item -ItemType Directory -Force $out | Out-Null
Copy-Item "$root\dos_sender\build\DOSFER.EXE" "$out\DOSFER.EXE" -Force
Copy-Item "$root\android_receiver\app\build\outputs\apk\debug\app-debug.apk" "$out\DOSFER-Receiver-debug.apk" -Force
Copy-Item "$root\test_vectors\SAMPLE.TXT" "$out\SAMPLE.TXT" -Force
Write-Host "Artifacts: $out"

