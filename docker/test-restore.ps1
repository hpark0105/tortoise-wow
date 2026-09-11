param([string]$BackupFile)
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path $PSScriptRoot -Parent
if (-not $BackupFile) {
    $latest = Get-ChildItem -LiteralPath (Join-Path $repoRoot 'local/backups') -Filter 'tortoise-*.sql' |
        Sort-Object Name -Descending | Select-Object -First 1
    if (-not $latest) { throw 'No complete backup found. Run docker/backup.ps1 first.' }
    $BackupFile = $latest.Name
}
if ($BackupFile -notmatch '^tortoise-[0-9]{8}T[0-9]{6}Z\.sql$') {
    throw 'Pass the backup filename, not an arbitrary path.'
}
# This unique project creates only a test DB. Its cleanup cannot address the
# personal server's tortoise-local_database volume.
$testProject = 'tortoise-restore-check-' + [guid]::NewGuid().ToString('N').Substring(0, 8)
Push-Location $repoRoot
try {
    docker compose -p $testProject up -d --wait --wait-timeout 900 db
    if ($LASTEXITCODE -ne 0) { throw 'Restore-test database did not start.' }
    docker compose -p $testProject exec -T db bash /ops/restore-check.sh $BackupFile
    if ($LASTEXITCODE -ne 0) { throw 'Restore validation failed.' }
} finally {
    docker compose -p $testProject down --volumes
    if ($LASTEXITCODE -ne 0) { Write-Warning "Cleanup failed for disposable project $testProject." }
    Pop-Location
}
