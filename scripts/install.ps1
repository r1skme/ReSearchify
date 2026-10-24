[CmdletBinding()]
param(
    [string]$InstallDir = (Join-Path $env:LOCALAPPDATA 'Programs\ReSearchify'),
    [switch]$NoPath
)

$ErrorActionPreference = 'Stop'
$repo = 'r1skme/ReSearchify'
if (-not (Get-Command gh -ErrorAction SilentlyContinue)) {
    throw 'GitHub CLI (gh) is required. Or download the ZIP manually from the repository Releases page.'
}
$info = & gh release list --repo $repo --limit 1 --json tagName 2>&1
if ($LASTEXITCODE -ne 0) { throw "Cannot list releases: $info. If GitHub CLI requires sign-in, run gh auth login; otherwise download the public release manually." }
$releases = @($info | ConvertFrom-Json)
if ($releases.Count -eq 0 -or -not $releases[0].tagName) {
    throw 'No ready Windows release yet. Check GitHub Actions or build from source.'
}
$tag = [string]$releases[0].tagName
$temp = Join-Path ([System.IO.Path]::GetTempPath()) ('researchify-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $temp | Out-Null
try {
    & gh release download $tag --repo $repo --pattern 'ReSearchify-Windows-x64.zip' --pattern 'SHA256SUMS.txt' --dir $temp
    if ($LASTEXITCODE -ne 0) { throw 'Release download failed. Check GitHub access.' }
    $zip = Join-Path $temp 'ReSearchify-Windows-x64.zip'
    $sums = Join-Path $temp 'SHA256SUMS.txt'
    if (-not (Test-Path $zip) -or -not (Test-Path $sums)) { throw 'Archive or checksum file missing.' }
    # SHA256SUMS.txt lists all five platforms; select exactly one named Windows asset.
    $sumLines = @(
        Get-Content -LiteralPath $sums | Where-Object { $_ -match '^([0-9a-fA-F]{64})\s+\*?ReSearchify-Windows-x64\.zip$' }
    )
    if ($sumLines.Count -ne 1) { throw 'Missing or duplicate Windows x64 entry in SHA256SUMS.txt.' }
    $expected = ($sumLines[0] -split '\s+')[0].ToLowerInvariant()
    $actual = (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actual -ne $expected) { throw 'SHA-256 mismatch. Nothing installed.' }
    $expanded = Join-Path $temp 'unpacked'
    Expand-Archive -LiteralPath $zip -DestinationPath $expanded
    $executable = Join-Path $expanded 'rs.exe'
    if (-not (Test-Path $executable)) { throw 'rs.exe missing in the verified archive.' }
    New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null
    Copy-Item -LiteralPath $executable -Destination (Join-Path $InstallDir 'rs.exe') -Force
    foreach ($readme in @('README.md', 'README.ru.md', 'LICENSE', 'SOURCE.md')) {
        $source = Join-Path $expanded $readme
        if (Test-Path $source) { Copy-Item -LiteralPath $source -Destination (Join-Path $InstallDir $readme) -Force }
    }
    if (-not $NoPath) {
        $existing = [Environment]::GetEnvironmentVariable('Path', 'User')
        $directories = @($existing -split ';' | Where-Object { $_ -ne '' })
        if ($directories -notcontains $InstallDir) {
            [Environment]::SetEnvironmentVariable('Path', (($directories + $InstallDir) -join ';'), 'User')
        }
        if (($env:Path -split ';') -notcontains $InstallDir) { $env:Path += ";$InstallDir" }
    }
    Write-Host "Installed ReSearchify $tag to $InstallDir"
    Write-Host 'Next: select your source project explicitly, then run: rs init "FULL_PROJECT_PATH"'
    Write-Host 'Confirm with: rs status --root "FULL_PROJECT_PATH". Do not index Downloads by accident.'
    & (Join-Path $InstallDir 'rs.exe') --version
} finally {
    Remove-Item -LiteralPath $temp -Recurse -Force -ErrorAction SilentlyContinue
}
