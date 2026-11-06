param(
    [string]$Port = 'COM9',
    [string]$BuildDir = 'C:/nrf_ws/build-usb',
    [string]$Image = '',
    [switch]$SkipBuild
)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$pythonExe = 'C:/ncs/toolchains/4f5b6ad6dd/opt/bin/python.exe'

# The real path contains Chinese characters.  Use the ASCII junction for the
# Zephyr source directory as well as the build directory.
$sourceDir = 'C:/nrf_ws/project'
if (-not (Test-Path "$sourceDir/CMakeLists.txt")) {
    $sourceDir = $projectRoot
}

if (-not $SkipBuild) {
    $env:PYTHONUTF8 = '1'
    & C:/ncs/bin/nrfutil.exe sdk-manager toolchain launch --ncs-version v3.4.1 --chdir C:/ncs/v3.4.1 -- west build --sysbuild -b nrf52dk/nrf52832 -d "$BuildDir" $sourceDir
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

if (-not (Test-Path "$projectRoot/.tools/pylibs/smpclient")) {
    & $pythonExe -m pip install --no-build-isolation --target "$projectRoot/.tools/pylibs" 'smpclient[serial]==7.3.0'
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

if (-not $Image) {
    $candidate = Get-ChildItem "$BuildDir/*/zephyr/zephyr.signed.bin" -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if (-not $candidate) {
        throw "No signed image under $BuildDir. Build without -SkipBuild first."
    }
    $Image = $candidate.FullName
}

& $pythonExe "$PSScriptRoot/usb_flash.py" --port $Port --image $Image
exit $LASTEXITCODE
