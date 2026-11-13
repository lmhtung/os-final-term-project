# Build the project using process-local ESP-IDF toolchain configuration.
$ErrorActionPreference = 'Stop'
$profilePath = 'C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1'
$profileText = Get-Content -LiteralPath $profilePath -Raw
$toolPath = [regex]::Match($profileText, '(?m)^\s*"PATH=([^"\r\n]+)"').Groups[1].Value
if (-not $toolPath) { throw 'Cannot read tool PATH from the installed ESP-IDF profile.' }
foreach ($entry in [regex]::Matches($profileText, '"([A-Z_]+)"\s*=\s*"([^"\r\n]*)"')) {
    [Environment]::SetEnvironmentVariable($entry.Groups[1].Value, $entry.Groups[2].Value, 'Process')
}
$env:PATH = $toolPath + ';' + $env:PATH
$env:ESP_IDF_VERSION = '6.1'
$env:IDF_VERSION = '6.1'
$env:PYTHONUTF8 = '1'
$env:IDF_CCACHE_ENABLE = '0'
$projectDirectory = Split-Path -Parent $PSScriptRoot
Push-Location -LiteralPath $projectDirectory
try {
    & "$env:IDF_PYTHON_ENV_PATH\Scripts\python.exe" "$env:IDF_PATH\tools\idf.py" build
    if ($LASTEXITCODE -ne 0) { throw "ESP-IDF build failed with exit code $LASTEXITCODE" }
} finally {
    Pop-Location
}
