# Apply positron patches to third-party submodules.
# Run after `git submodule update --init --recursive`.
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$patchDir = Split-Path -Parent $MyInvocation.MyCommand.Path

Write-Host "[patches] Applying BlackBone patch..."
Push-Location "$root\third_party\Blackbone"
git apply --whitespace=nowarn "$patchDir\blackbone.patch" 2>$null
if ($LASTEXITCODE -ne 0) {
    Write-Host "[patches] Already applied or conflict — skipping."
} else {
    Write-Host "[patches] Applied successfully."
}
Pop-Location
Write-Host "[patches] Done."
