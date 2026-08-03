$ErrorActionPreference = 'Stop'
$sdk = if ($env:ANDROID_HOME) { $env:ANDROID_HOME } else { "$env:LOCALAPPDATA\Android\Sdk" }
$adb = "$sdk\platform-tools\adb.exe"
$apk = Join-Path $PSScriptRoot '..\android_receiver\app\build\outputs\apk\debug\app-debug.apk'
if (-not (Test-Path $adb)) { throw "adb not found: $adb" }
if (-not (Test-Path $apk)) { throw "APK not found; build Android first: $apk" }
& $adb devices -l
& $adb install -r $apk
if ($LASTEXITCODE) { throw 'Install failed. Unlock the phone and authorize USB debugging.' }
& $adb shell am start -n org.dosfer.receiver/.MainActivity

