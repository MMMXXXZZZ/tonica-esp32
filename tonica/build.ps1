# Build (and optionally flash/monitor) the firmware with ESP-IDF.
#   .\build.ps1                 build
#   .\build.ps1 flash COM5      build + flash
param([string]$Action = "build", [string]$Port = "")
Set-Location $PSScriptRoot
Remove-Item Env:\MSYSTEM, Env:\MSYSTEM_PREFIX -ErrorAction Ignore
$env:IDF_PATH = "$HOME\esp\esp-idf"
. "$env:IDF_PATH\export.ps1" | Out-Null
if ($Action -eq "build") {
  if (-not (Test-Path sdkconfig)) { idf.py set-target esp32s3 2>&1 | Out-Null }
  # settings.conf is the one file to edit; write it into sdkconfig (sdkconfig.defaults alone can't override it)
  python -I apply_settings.py
  if ($LASTEXITCODE -ne 0) { Write-Error "settings.conf is invalid"; exit 1 }
  idf.py build 2>&1 | Tee-Object -FilePath build.log | Select-Object -Last 25
}
elseif ($Action -eq "flash") { idf.py -p $Port -b 921600 flash 2>&1 | Tee-Object -FilePath flash.log | Select-Object -Last 15 }
