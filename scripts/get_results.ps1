# Copy results/ from the lab PC into this project (skips the huge train_*.csv files).
# Usage (PowerShell):  powershell -ExecutionPolicy Bypass -File scripts\get_results.ps1 [-Lab user@host] [-WithTraining]
# The lab address comes from -Lab, $env:BURSTSCHED_LAB, or scripts\lab_host.txt (not in git).
param([string]$Lab = "", [switch]$WithTraining)

if (-not $Lab) { $Lab = $env:BURSTSCHED_LAB }
$hostFile = Join-Path $PSScriptRoot "lab_host.txt"
if (-not $Lab -and (Test-Path $hostFile)) { $Lab = (Get-Content $hostFile -TotalCount 1).Trim() }
if (-not $Lab) { Write-Host "Set the lab PC: -Lab user@host, or put user@host in $hostFile"; exit 1 }

$proj = Split-Path -Parent $PSScriptRoot
$name = Split-Path -Leaf $proj
$excl = if ($WithTraining) { "" } else { "--exclude='train_*.csv'" }

Write-Host "Fetching ${Lab}:~/burstsched-work/$name/results -> $proj\results"
cmd /c "ssh $Lab `"tar -czf - -C ~/burstsched-work/$name $excl results`" | tar -xzf - -C `"$proj`""
if ($LASTEXITCODE -eq 0) { Write-Host "done" } else { Write-Host "fetch FAILED (exit $LASTEXITCODE)" }
