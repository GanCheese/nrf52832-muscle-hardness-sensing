param(
    [string]$Message = ''
)

# Commit local changes, rebase on origin/main, then push.
# Run from anywhere:  powershell -ExecutionPolicy Bypass -File .\scripts\sync-github.ps1
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
Set-Location $projectRoot

if (-not (Test-Path '.git')) {
    throw "Not a Git repository: $projectRoot"
}

if (-not $Message) {
    $Message = 'Sync local changes ' + (Get-Date -Format 'yyyy-MM-dd HH:mm:ss')
}

git add -A
git diff --cached --quiet
if ($LASTEXITCODE -ne 0) {
    git commit -m $Message
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
} else {
    Write-Host 'No local changes to commit.'
}

git pull --rebase --autostash origin main
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

git push origin main
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host 'GitHub is up to date.'
