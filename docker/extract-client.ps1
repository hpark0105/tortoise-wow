param(
    [Parameter(Mandatory = $true)]
    [string]$ClientPath,
    [switch]$VerifyOnly
)
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path $PSScriptRoot -Parent
$clientDirectory = (Resolve-Path -LiteralPath $ClientPath).Path
if (-not (Test-Path -LiteralPath (Join-Path $clientDirectory 'Data') -PathType Container)) {
    throw 'ClientPath must be the game folder containing its Data directory.'
}
$outputDirectory = Join-Path $repoRoot 'data'
if ($VerifyOnly) {
    $outputDirectory = Join-Path $repoRoot ('local/dbc-check-' + [guid]::NewGuid().ToString('N').Substring(0, 8))
} else {
    foreach ($folder in @('dbc', 'maps', 'vmaps', 'mmaps', 'Buildings')) {
        $existing = Join-Path $outputDirectory $folder
        if ((Test-Path -LiteralPath $existing) -and (Get-ChildItem -LiteralPath $existing | Select-Object -First 1)) {
            throw "Existing extraction data found in $existing. Use -VerifyOnly to check another client; move old extraction folders aside before replacing them."
        }
    }
}
New-Item -ItemType Directory -Force -Path $outputDirectory | Out-Null
$baseArgs = @('run', '--rm', '--user', '0', '--workdir', '/output',
    '--mount', "type=bind,source=$clientDirectory,target=/client,readonly",
    '--mount', "type=bind,source=$outputDirectory,target=/output",
    '--mount', "type=bind,source=$PSScriptRoot,target=/ops,readonly")
function Invoke-Extractor([string]$Executable, [string[]]$ToolArgs, [int]$SuccessCode = 0) {
    docker @baseArgs --entrypoint $Executable tortoise-local:dev @ToolArgs
    if ($LASTEXITCODE -ne $SuccessCode) { throw "Extraction step failed: $Executable (exit $LASTEXITCODE)" }
}
Invoke-Extractor 'mapextractor' @('-i', '/client', '-o', '/output', '-e', '2')
Invoke-Extractor 'python3' @('/ops/verify_dbc.py', '/output/dbc', '/opt/tortoise/dbc_verification/dbc_verifier.py')
if ($VerifyOnly) {
    Write-Host "DBC check passed. Verification files are in $outputDirectory."
    return
}
Invoke-Extractor 'mapextractor' @('-i', '/client', '-o', '/output', '-e', '1')
Invoke-Extractor 'vmapextractor' @('-d', '/client')
New-Item -ItemType Directory -Force -Path (Join-Path $outputDirectory 'vmaps') | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $outputDirectory 'mmaps') | Out-Null
Invoke-Extractor 'vmap_assembler' @('Buildings', 'vmaps')
# This repository's generator returns 1 on successful completion, not 0.
Invoke-Extractor 'MoveMapGen' @('--silent') 1
Write-Host 'Extraction finished. Review tool output, then run docker compose up -d.'
