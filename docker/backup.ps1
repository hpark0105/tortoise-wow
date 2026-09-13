$ErrorActionPreference = 'Stop'
Push-Location (Split-Path $PSScriptRoot -Parent)
try {
    $running = @(docker compose ps --services --status running)
    if ($LASTEXITCODE -ne 0) { throw 'Could not query Docker services.' }
    if ('db' -notin $running) { throw 'Start the database before backing up.' }
    $writers = @($running | Where-Object { $_ -in @('world', 'realmd') })
    try {
        if ($writers.Count -gt 0) {
            docker compose stop @writers
            if ($LASTEXITCODE -ne 0) { throw 'Could not stop server processes cleanly.' }
        }
        docker compose exec -T db bash /ops/backup.sh
        if ($LASTEXITCODE -ne 0) { throw 'Backup failed; a .partial file is not a usable backup.' }
    } finally {
        if ($writers.Count -gt 0) {
            docker compose start @writers
            if ($LASTEXITCODE -ne 0) { Write-Warning 'Restart failed; inspect docker compose ps.' }
        }
    }
} finally { Pop-Location }
