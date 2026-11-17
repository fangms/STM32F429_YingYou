param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Debug',
    [Parameter(Mandatory = $true)]
    [string]$OutputRoot
)
$ErrorActionPreference = 'Stop'
$firmwareRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$artifactRoot = [IO.Path]::GetFullPath($OutputRoot)
$repoPrefix = $firmwareRoot.TrimEnd([IO.Path]::DirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
if ($artifactRoot.Equals($firmwareRoot, [StringComparison]::OrdinalIgnoreCase) -or
    $artifactRoot.StartsWith($repoPrefix, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Build output must be outside the repository'
}
New-Item -ItemType Directory -Path $artifactRoot -Force | Out-Null
$versionHeader = Get-Content -LiteralPath (Join-Path $firmwareRoot 'Core/Inc/firmware_version.h') -Raw
$versionMatch = [regex]::Match($versionHeader, '#define H863_FIRMWARE_VERSION "(V[0-9]+\.[0-9]+)"')
if (-not $versionMatch.Success) { throw 'Firmware version missing' }
$firmwareVersion = $versionMatch.Groups[1].Value
$chinaTime = [TimeZoneInfo]::ConvertTimeBySystemTimeZoneId([DateTime]::UtcNow, 'China Standard Time')
$packageTag = $chinaTime.ToString('yyyyMMdd_HHmm')
$deliveryDir = Join-Path $artifactRoot 'firmware'
New-Item -ItemType Directory -Path $deliveryDir -Force | Out-Null
$deliveryName = "H863_${firmwareVersion}_${Configuration}_${packageTag}"
$toolBundle = Join-Path $env:LOCALAPPDATA 'stm32cube\bundles'
$gccBin = Join-Path $toolBundle 'gnu-tools-for-stm32\14.3.1+st.2\bin'
$cmakeExe = Join-Path $toolBundle 'cmake\4.3.1+st.1\bin\cmake.exe'
$ninjaBin = Join-Path $toolBundle 'ninja\1.13.2+st.1\bin'
$originalPath = $env:PATH
$runTag = Get-Date -Format 'yyyyMMdd_HHmmss_fff'
$buildDir = Join-Path $artifactRoot $Configuration
try {
    $env:PATH = "$gccBin;$(Split-Path $cmakeExe);$ninjaBin;$originalPath"
    & $cmakeExe --preset $Configuration -S $firmwareRoot -B $buildDir 2>&1 |
        Tee-Object -FilePath (Join-Path $artifactRoot "configure_${Configuration}_${runTag}.log")
    if ($LASTEXITCODE -ne 0) { throw "CMake configure failed: $LASTEXITCODE" }
    & $cmakeExe --build $buildDir --clean-first --parallel 8 2>&1 |
        Tee-Object -FilePath (Join-Path $artifactRoot "build_${Configuration}_${runTag}.log")
    if ($LASTEXITCODE -ne 0) { throw "Build failed: $LASTEXITCODE" }
    $elf = Join-Path $deliveryDir "$deliveryName.elf"
    $bin = Join-Path $deliveryDir "$deliveryName.bin"
    $hex = Join-Path $deliveryDir "$deliveryName.hex"
    Copy-Item -LiteralPath (Join-Path $buildDir 'STM32F429IGT6.elf') -Destination $elf
    Copy-Item -LiteralPath (Join-Path $buildDir 'STM32F429IGT6.map') -Destination (Join-Path $deliveryDir "$deliveryName.map")
    & (Join-Path $gccBin 'arm-none-eabi-objcopy.exe') -O binary $elf $bin
    if ($LASTEXITCODE -ne 0) { throw 'BIN export failed' }
    & (Join-Path $gccBin 'arm-none-eabi-objcopy.exe') -O ihex $elf $hex
    if ($LASTEXITCODE -ne 0) { throw 'HEX export failed' }
    Get-FileHash -LiteralPath $elf,$bin,$hex -Algorithm SHA256 |
        Select-Object Path,Hash | ConvertTo-Json |
        Set-Content -LiteralPath (Join-Path $deliveryDir "${deliveryName}_sha256.json") -Encoding utf8
    Write-Output "Build and versioned ELF/BIN/HEX export completed: $deliveryDir/$deliveryName"
}
finally {
    $env:PATH = $originalPath
}
