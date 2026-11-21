# Copy the project (code + models; NOT .git, results, build) to the lab PC.
# Usage (PowerShell):  powershell -ExecutionPolicy Bypass -File scripts\sync_to_lab.ps1
param([string]$Lab = "user@lab-pc")

$proj   = Split-Path -Parent $PSScriptRoot
$parent = Split-Path -Parent $proj
$name   = Split-Path -Leaf $proj

Write-Host "Syncing $proj -> ${Lab}:~/burstsched-work/$name"
# cmd /c so the tar stream is piped as raw bytes (Windows PowerShell 5.1 would corrupt it)
$remote = "mkdir -p ~/burstsched-work && tar -xzmf - -C ~/burstsched-work && chmod +x ~/burstsched-work/$name/scripts/*.sh"
cmd /c "tar -czf - --exclude=.git --exclude=results --exclude=build --exclude=__pycache__ -C `"$parent`" $name | ssh $Lab `"$remote`""
if ($LASTEXITCODE -eq 0) { Write-Host "done" } else { Write-Host "sync FAILED (exit $LASTEXITCODE)" }
