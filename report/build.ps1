# Build the report PDF (MiKTeX/TeX Live pdflatex). Run from anywhere:
#   powershell -ExecutionPolicy Bypass -File report\build.ps1
# Figures must exist first:  python analysis/plots.py data/final figures/paper --paper
$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot
# MiKTeX would otherwise try to download missing packages and can hang waiting for it
$extra = @(); if ((& pdflatex --version | Select-Object -First 1) -match "MiKTeX") { $extra = @("-disable-installer") }
foreach ($pass in 1, 2) {   # second pass resolves figure/table/citation numbers
    & pdflatex -interaction=nonstopmode -halt-on-error @extra main.tex | Out-Null
    if ($LASTEXITCODE -ne 0) { Get-Content main.log -Tail 30; throw "pdflatex failed (pass $pass)" }
}
Copy-Item main.pdf BurstSched_report.pdf -Force
Write-Host "wrote report\BurstSched_report.pdf"
