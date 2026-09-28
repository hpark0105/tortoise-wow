param([switch]$Once)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$localRoot = Join-Path $repoRoot 'local'
$reportPath = Join-Path $localRoot 'citizen-progression-monitor.log'
$statePath = Join-Path $localRoot 'citizen-progression-monitor.state.json'
$intervalSeconds = 600
$llmIntervalSeconds = 3600

New-Item -ItemType Directory -Path $localRoot -Force | Out-Null

function Invoke-CitizenQuery([string]$Query) {
    $output = $Query | & docker compose exec -T db bash -lc 'export MYSQL_PWD="$MARIADB_ROOT_PASSWORD"; mariadb --user=root --batch --skip-column-names tw_char' 2>$null
    if ($LASTEXITCODE -ne 0) {
        throw "Citizen monitor database query failed (exit $LASTEXITCODE)."
    }
    return @($output | ForEach-Object { [string]$_ })
}

function Get-CitizenMetric([string[]]$Rows, [string]$Prefix) {
    $row = $Rows | Where-Object { $_.StartsWith($Prefix) } | Select-Object -First 1
    if (!$row) { return @() }
    return ,($row.Substring($Prefix.Length).Split('|'))
}

function Get-LocalModelAdvice([string]$Metrics, [datetime]$Now, $State) {
    $lastAdvice = [datetime]::MinValue
    if ($State.lastLlmAdviceAt) {
        [datetime]::TryParse([string]$State.lastLlmAdviceAt, [ref]$lastAdvice) | Out-Null
    }
    if (($Now - $lastAdvice).TotalSeconds -lt $llmIntervalSeconds) { return $null }

    try {
        $models = Invoke-RestMethod -Uri 'http://127.0.0.1:8090/v1/models' -TimeoutSec 3
        $modelId = [string]$models.data[0].id
        if (!$modelId) { return 'unavailable:no-model-listed' }
        $body = @{
            model = $modelId
            temperature = 0.2
            max_tokens = 220
            messages = @(
                @{ role = 'system'; content = 'You are reviewing aggregate health metrics for a private World of Warcraft citizen-bot pilot. Give one concise, evidence-based recommendation for the next bot-logic or model-use improvement. State uncertainty. Never ask to modify live data, characters, or configuration automatically. Do not infer facts absent from the metrics.' }
                @{ role = 'user'; content = $Metrics }
            )
        } | ConvertTo-Json -Depth 6 -Compress
        $reply = Invoke-RestMethod -Uri 'http://127.0.0.1:8090/v1/chat/completions' `
            -Method Post -ContentType 'application/json' -Body $body -TimeoutSec 30
        $advice = [string]$reply.choices[0].message.content
        if (!$advice) { return 'unavailable:empty-model-response' }
        $State.lastLlmAdviceAt = $Now.ToString('o')
        return (($advice -replace '[\r\n]+', ' ' -replace '\s{2,}', ' ').Trim())
    }
    catch {
        return ('unavailable:' + $_.Exception.GetType().Name)
    }
}

function Write-CitizenSnapshot {
    $now = Get-Date
    $timestamp = $now.ToString('s')
    $query = @'
SELECT CONCAT('POP|',SUM(CASE WHEN p.char_guid IS NOT NULL AND b.owner_account_id IS NULL THEN 1 ELSE 0 END),'|',SUM(CASE WHEN p.char_guid IS NULL THEN 1 ELSE 0 END)) FROM characters c LEFT JOIN playerbot p ON p.char_guid=c.guid LEFT JOIN bot_ownership b ON b.char_guid=c.guid WHERE c.online=1;
SELECT CONCAT('LEVEL|',c.level,'|',COUNT(*),'|',SUM(c.xp)) FROM characters c JOIN playerbot p ON p.char_guid=c.guid LEFT JOIN bot_ownership b ON b.char_guid=c.guid WHERE c.online=1 AND b.owner_account_id IS NULL GROUP BY c.level ORDER BY c.level;
SELECT CONCAT('ZONE|',c.zone,'|',COUNT(*)) FROM characters c JOIN playerbot p ON p.char_guid=c.guid LEFT JOIN bot_ownership b ON b.char_guid=c.guid WHERE c.online=1 AND b.owner_account_id IS NULL GROUP BY c.zone ORDER BY c.zone;
SELECT CONCAT('JOB|',COALESCE(j.current_job,0),'|',COUNT(*)) FROM characters c JOIN playerbot p ON p.char_guid=c.guid LEFT JOIN bot_ownership b ON b.char_guid=c.guid LEFT JOIN bot_citizen_journal j ON j.char_guid=c.guid WHERE c.online=1 AND b.owner_account_id IS NULL GROUP BY COALESCE(j.current_job,0) ORDER BY COALESCE(j.current_job,0);
SELECT CONCAT('BOTLEVEL|',c.guid,'|',c.level) FROM characters c JOIN playerbot p ON p.char_guid=c.guid LEFT JOIN bot_ownership b ON b.char_guid=c.guid WHERE c.online=1 AND b.owner_account_id IS NULL;
SELECT CONCAT('FIGHT|',COUNT(*),'|',COALESCE(SUM(deaths),0),'|',COALESCE(SUM(is_complete),0),'|',COALESCE(SUM(effective_damage),0),'|',COALESCE(SUM(damage_taken),0),'|',COALESCE(SUM(casts_accepted),0),'|',COALESCE(SUM(casts_rejected),0)) FROM bot_learning_encounter WHERE captured_at >= UNIX_TIMESTAMP()-600;
SELECT CONCAT('SOCIAL|',COUNT(*)) FROM bot_social_acquaintance WHERE last_seen >= UNIX_TIMESTAMP()-600;
SELECT CONCAT('DANGER|',SUM(CASE WHEN safe_until>UNIX_TIMESTAMP() THEN 1 ELSE 0 END),'|',SUM(CASE WHEN updated_at>=UNIX_TIMESTAMP()-600 THEN 1 ELSE 0 END)) FROM bot_citizen_danger_memory;
'@
    $rows = Invoke-CitizenQuery $query
    $pop = Get-CitizenMetric $rows 'POP|'
    $citizensOnline = if ($pop.Count -gt 0) { [int]$pop[0] } else { 0 }
    $humansOnline = if ($pop.Count -gt 1) { [int]$pop[1] } else { 0 }
    $levels = @($rows | Where-Object { $_.StartsWith('LEVEL|') } | ForEach-Object {
        $v = $_.Split('|'); "$($v[1]):$($v[2])"
    }) -join ','
    $zones = @($rows | Where-Object { $_.StartsWith('ZONE|') } | ForEach-Object {
        $v = $_.Split('|'); "$($v[1]):$($v[2])"
    }) -join ','
    $jobs = @($rows | Where-Object { $_.StartsWith('JOB|') } | ForEach-Object {
        $v = $_.Split('|'); "$($v[1]):$($v[2])"
    }) -join ','
    $levelByGuid = @{}
    foreach ($row in ($rows | Where-Object { $_.StartsWith('BOTLEVEL|') })) {
        $v = $row.Split('|'); $levelByGuid[[uint32]$v[1]] = [int]$v[2]
    }

    $worldLogs = @(& docker compose logs --since 10m --no-color world 2>$null)
    $huntValid = 0; $huntInvalid = 0
    foreach ($line in $worldLogs) {
        if ($line -match '\[ZoneCitizen\] hunt guid:(\d+) target:(\d+) level:(\d+)') {
            $guid = [uint32]$Matches[1]; $targetLevel = [int]$Matches[3]
            if ($levelByGuid.ContainsKey($guid)) {
                $citizenLevel = $levelByGuid[$guid]
                if ($targetLevel -le $citizenLevel -and $targetLevel -ge [Math]::Max(1, $citizenLevel - 3)) { $huntValid++ }
                else { $huntInvalid++ }
            }
        }
    }
    $fight = Get-CitizenMetric $rows 'FIGHT|'
    $social = Get-CitizenMetric $rows 'SOCIAL|'
    $danger = Get-CitizenMetric $rows 'DANGER|'
    $intentSubmitted = @($worldLogs | Select-String -Pattern '\[Conversation\] submit bot:.*profile:0').Count
    $intentAccepted = @($worldLogs | Select-String -Pattern '\[WorldIntent\] accepted bot:').Count
    $intentRejected = @($worldLogs | Select-String -Pattern 'conversation submit refused|world intent.*(failed|timeout|rejected)').Count
    $relocations = @($worldLogs | Select-String -Pattern 'login queued guid:.*relocation:1').Count
    $retreats = @($worldLogs | Select-String -Pattern '\[ZoneCitizen\]\[Survival\] retreat(?:-start)? guid:').Count
    $retreatEscaped = @($worldLogs | Select-String -Pattern '\[Survival\] retreat-result .*result:escaped ').Count
    $retreatFailed = @($worldLogs | Select-String -Pattern '\[Survival\] retreat-result .*result:(stalled|timed-out) ').Count
    $retreatUnavailable = @($worldLogs | Select-String -Pattern '\[Survival\] retreat-unavailable guid:').Count
    $retreatDeferred = @($worldLogs | Select-String -Pattern '\[Survival\] retreat-retry .*deferred:1').Count
    $departs = @($worldLogs | Select-String -Pattern '\[ZoneCitizen\]\[Survival\] leaving-danger').Count
    $errors = @($worldLogs | Select-String -Pattern 'FATAL|Segmentation fault|\bERROR\b').Count

    $totalXp = 0L
    foreach ($row in ($rows | Where-Object { $_.StartsWith('LEVEL|') })) {
        $v = $row.Split('|'); $totalXp += [long]$v[3]
    }
    $state = @{}
    if (Test-Path -LiteralPath $statePath) {
        try { $state = Get-Content -LiteralPath $statePath -Raw | ConvertFrom-Json -AsHashtable }
        catch { $state = @{} }
    }
    $xpDelta = 'baseline'
    if ($null -ne $state.totalXp) { $xpDelta = [string]($totalXp - [long]$state.totalXp) }

    $fightSummary = if ($fight.Count -ge 7) {
        "records=$($fight[0]), deaths=$($fight[1]), complete=$($fight[2]), damage=$($fight[3]), taken=$($fight[4]), casts_ok=$($fight[5]), casts_rejected=$($fight[6])"
    } else { 'unavailable' }
    $socialCount = if ($social.Count) { [int]$social[0] } else { 0 }
    $activeDanger = if ($danger.Count) { [int]$danger[0] } else { 0 }

    $recommendations = [System.Collections.Generic.List[string]]::new()
    if ($huntInvalid -gt 0) { $recommendations.Add('Investigate out-of-band hunt telemetry before changing target-selection policy.') }
    if ($huntValid -eq 0 -and $citizensOnline -gt 50) { $recommendations.Add('No level-checked hunt events were observed; inspect target availability and patrol/rest transitions.') }
    if ($citizensOnline -gt 0 -and $activeDanger -gt ($citizensOnline * 0.20)) { $recommendations.Add('Active danger memories exceed 20% of online citizens; inspect relocation destinations and repeat deaths.') }
    if ($relocations -gt 0) { $recommendations.Add("Safety allocator moved $relocations citizens in this window; compare their next-window hunt and death rates.") }
    if ($intentSubmitted -gt 0 -or $intentAccepted -gt 0) { $recommendations.Add("Local-model world intents: submitted=$intentSubmitted accepted=$intentAccepted rejected=$intentRejected; compare accepted intents with hunt and recovery outcomes.") }
    if ($socialCount -eq 0) { $recommendations.Add('No shared-kill acquaintance events were recorded; citizen-to-citizen party assistance is not measured as active behavior yet.') }
    if ($errors -gt 0) { $recommendations.Add("Found $errors server error/fatal lines; inspect those log entries.") }
    if ($retreatFailed -gt 0 -or $retreatUnavailable -gt 0) { $recommendations.Add("Retreat outcomes: escaped=$retreatEscaped failed=$retreatFailed unavailable=$retreatUnavailable; inspect path and threat clearance before increasing population.") }
    if (!$recommendations.Count) { $recommendations.Add('No threshold alert; continue collecting comparable windows before tuning behavior.') }

    $metrics = "window=10m online=$citizensOnline human_online=$humansOnline levels={$levels} zones={$zones} jobs={$jobs} xp_delta_since_previous=$xpDelta hunt_in_band=$huntValid hunt_out_of_band=$huntInvalid fights={$fightSummary} social_rows=$socialCount active_danger=$activeDanger danger_updates=$($danger[1]) relocations=$relocations retreats=$retreats danger_departures=$departs local_model_intents{submitted=$intentSubmitted,accepted=$intentAccepted,rejected=$intentRejected} errors=$errors"
    $metrics += " retreat_outcomes{escaped=$retreatEscaped,failed=$retreatFailed,unavailable=$retreatUnavailable,deferred_events=$retreatDeferred}"
    $llmAdvice = Get-LocalModelAdvice $metrics $now $state
    if ($llmAdvice) { $state.lastLlmAdvice = $llmAdvice }
    $state.totalXp = $totalXp
    $state.lastSampleAt = $now.ToString('o')
    $state | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $statePath -Encoding UTF8

    $message = "[$timestamp] $metrics recommendations={$($recommendations -join ' ; ')}"
    if ($llmAdvice) { $message += " local_llm_advice={$llmAdvice}" }
    Add-Content -LiteralPath $reportPath -Value $message -Encoding UTF8
}

while ($true) {
    try { Write-CitizenSnapshot }
    catch {
        Add-Content -LiteralPath $reportPath -Value ("[$((Get-Date).ToString('s'))] monitor_error=$($_.Exception.GetType().Name)" ) -Encoding UTF8
    }
    if ($Once) { break }
    Start-Sleep -Seconds $intervalSeconds
}
