$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path $PSScriptRoot -Parent
$envFile = Join-Path $repoRoot '.env'
if (-not (Test-Path -LiteralPath $envFile)) {
    $rng = [System.Security.Cryptography.RandomNumberGenerator]::Create()
    try {
        $bytes = New-Object byte[] 32
        $rng.GetBytes($bytes)
        $dbPassword = [BitConverter]::ToString($bytes).Replace('-', '').ToLowerInvariant()
        $rng.GetBytes($bytes)
        $rootPassword = [BitConverter]::ToString($bytes).Replace('-', '').ToLowerInvariant()
        [IO.File]::WriteAllText($envFile, "DB_PASSWORD=$dbPassword`nDB_ROOT_PASSWORD=$rootPassword`nBUILD_JOBS=2`n", [Text.UTF8Encoding]::new($false))
    } finally { $rng.Dispose() }
    Write-Host 'Created local database credentials in .env.'
} else {
    Write-Host 'Keeping existing .env credentials.'
}
foreach ($relative in @('data', 'local/backups')) {
    New-Item -ItemType Directory -Force -Path (Join-Path $repoRoot $relative) | Out-Null
}
Write-Host 'Ready for: docker compose build'
