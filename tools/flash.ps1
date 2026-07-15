# Build and flash the AuraGlasses firmware to the XIAO ESP32-S3.
#
#   powershell -File tools\flash.ps1 [-Port COM8] [-Monitor]
#
# Note: this board fails esptool's default reset ("Wrong boot mode detected"),
# so we flash with esptool directly using --before usb_reset.
param(
    [string]$Port = "COM8",
    [switch]$Monitor
)

$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
$fqbn = "esp32:esp32:XIAO_ESP32S3:PSRAM=opi,PartitionScheme=default_8MB"
$sketch = Join-Path $root "firmware\AuraGlasses"

python (Join-Path $PSScriptRoot "build_web.py")
if ($LASTEXITCODE -ne 0) { throw "build_web.py failed" }

arduino-cli compile -b $fqbn $sketch
if ($LASTEXITCODE -ne 0) { throw "compile failed" }

# Locate arduino-cli's build output for this sketch
$props = arduino-cli compile -b $fqbn $sketch --show-properties | Select-String '^build\.path=(.+)$'
$build = $props.Matches[0].Groups[1].Value

$esptool = Get-ChildItem "$env:LOCALAPPDATA\Arduino15\packages\esp32\tools\esptool_py\*\esptool.exe" |
    Select-Object -First 1 -ExpandProperty FullName
$bootApp0 = Get-ChildItem "$env:LOCALAPPDATA\Arduino15\packages\esp32\hardware\esp32\*\tools\partitions\boot_app0.bin" |
    Select-Object -First 1 -ExpandProperty FullName

& $esptool --chip esp32s3 --port $Port --baud 921600 --before usb_reset --after hard_reset write-flash -z `
    0x0 "$build\AuraGlasses.ino.bootloader.bin" `
    0x8000 "$build\AuraGlasses.ino.partitions.bin" `
    0xe000 $bootApp0 `
    0x10000 "$build\AuraGlasses.ino.bin"
if ($LASTEXITCODE -ne 0) { throw "flash failed" }

Write-Host "`nFlashed. Device will boot and join WiFi (watch for it at http://aura.local)." -ForegroundColor Green

if ($Monitor) {
    arduino-cli monitor -p $Port -c baudrate=115200
}
