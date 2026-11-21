# Copy results/ from the lab PC into this project (skips the huge train_*.csv files).
# Usage (PowerShell):  powershell -ExecutionPolicy Bypass -File scripts\get_results.ps1
param([string]$Lab = "user@lab-pc", [switch]$WithTraining)

$proj = Split-Path -Parent $PSScriptRoot
$name = Split-Path -Leaf $proj
$excl = if ($WithTraining) { "" } else { "--exclude='train_*.csv'" }

Write-Host "Fetching ${Lab}:~/burstsched-work/$name/results -> $proj\results"
cmd /c "ssh $Lab `"tar -czf - -C ~/burstsched-work/$name $excl results`" | tar -xzf - -C `"$proj`""
if ($LASTEXITCODE -eq 0) { Write-Host "done" } else { Write-Host "fetch FAILED (exit $LASTEXITCODE)" }
