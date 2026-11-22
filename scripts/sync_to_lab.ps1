# Copy the project (code + models; NOT .git, results, build, private notes) to the lab PC.
# Usage (PowerShell):  powershell -ExecutionPolicy Bypass -File scripts\sync_to_lab.ps1 [-Lab user@host]
# The lab address comes from -Lab, $env:BURSTSCHED_LAB, or scripts\lab_host.txt (not in git).
param([string]$Lab = "")

if (-not $Lab) { $Lab = $env:BURSTSCHED_LAB }
$hostFile = Join-Path $PSScriptRoot "lab_host.txt"
if (-not $Lab -and (Test-Path $hostFile)) { $Lab = (Get-Content $hostFile -TotalCount 1).Trim() }
if (-not $Lab) { Write-Host "Set the lab PC: -Lab user@host, or put user@host in $hostFile"; exit 1 }

$proj   = Split-Path -Parent $PSScriptRoot
$parent = Split-Path -Parent $proj
$name   = Split-Path -Leaf $proj

Write-Host "Syncing $proj -> ${Lab}:~/burstsched-work/$name"
# cmd /c so the tar stream is piped as raw bytes (Windows PowerShell 5.1 would corrupt it)
$remote = "mkdir -p ~/burstsched-work && tar -xzmf - -C ~/burstsched-work && chmod +x ~/burstsched-work/$name/scripts/*.sh"
cmd /c "tar -czf - --exclude=.git --exclude=results --exclude=build --exclude=__pycache__ --exclude=PROJECT_NOTES.md --exclude=CLAUDE.md --exclude=lab_host.txt -C `"$parent`" $name | ssh $Lab `"$remote`""
if ($LASTEXITCODE -eq 0) { Write-Host "done" } else { Write-Host "sync FAILED (exit $LASTEXITCODE)" }
