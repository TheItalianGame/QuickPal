param(
  [string]$SourceRoot = "",
  [switch]$Force
)

$ErrorActionPreference = "Stop"

$QuickPalRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$VendorRoot = Join-Path $QuickPalRoot "third_party\Bitwarden"
$VendorExe = Join-Path $VendorRoot "bw.exe"
$ProvenancePath = Join-Path $VendorRoot "PROVENANCE.json"
$Repository = "https://github.com/TheItalianGame/quickpal-bitwarden.git"
$Commit = "2e4b1fba6e02ca9822043ba89deb8809468e0b7b"
$Version = "2026.8.0"
$BuildNodeVersion = "24.17.0"
$BuildNpmVersion = "11.6.2"
$PackageNodeVersion = "22.22.0"
$PackageNpmVersion = "10.9.4"

function Test-BitwardenOutput([string]$Exe, [string]$ExpectedHash = "") {
  if (-not (Test-Path -LiteralPath $Exe -PathType Leaf)) {
    return $false
  }
  $actualHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $Exe).Hash
  if ($ExpectedHash -and $actualHash -ne $ExpectedHash) {
    return $false
  }
  $actualVersion = (& $Exe --version).Trim()
  if ($LASTEXITCODE -ne 0 -or $actualVersion -ne $Version) {
    return $false
  }
  $serveHelp = (& $Exe serve --help 2>&1 | Out-String)
  return $LASTEXITCODE -eq 0 -and
    $serveHelp.Contains("--auth-token-env") -and
    $serveHelp.Contains("--allow-unauthenticated")
}

function Copy-NormalizedLicense([string]$Source, [string]$Destination) {
  $text = Get-Content -Raw -LiteralPath $Source
  $text = [Regex]::Replace($text, '[ \t]+(?=\r?$)', '', [Text.RegularExpressions.RegexOptions]::Multiline)
  $text = $text.TrimEnd("`r", "`n") + [Environment]::NewLine
  [IO.File]::WriteAllText($Destination, $text, [Text.UTF8Encoding]::new($false))
}

if (-not $Force -and
    (Test-Path -LiteralPath $ProvenancePath -PathType Leaf) -and
    (Test-Path -LiteralPath $VendorExe -PathType Leaf)) {
  try {
    $provenance = Get-Content -Raw -LiteralPath $ProvenancePath | ConvertFrom-Json
    if ($provenance.repository -eq $Repository -and
        $provenance.commit -eq $Commit -and
        $provenance.version -eq $Version -and
        (Test-BitwardenOutput $VendorExe $provenance.sha256)) {
      Write-Host "Reusing verified QuickPal Bitwarden CLI $Version ($($Commit.Substring(0, 12)))"
      return
    }
  } catch {
    Write-Host "Cached QuickPal Bitwarden provenance is invalid; rebuilding from source"
  }
}

if (-not $SourceRoot -and $env:QUICKPAL_BITWARDEN_SOURCE) {
  $SourceRoot = $env:QUICKPAL_BITWARDEN_SOURCE
}
if (-not $SourceRoot) {
  $existingSibling = Join-Path (Split-Path -Parent $QuickPalRoot) "rainyzmk\clients"
  if (Test-Path -LiteralPath (Join-Path $existingSibling ".git")) {
    $SourceRoot = $existingSibling
  }
}

$managedSource = Join-Path $QuickPalRoot ".deps\quickpal-bitwarden"
if (-not $SourceRoot) {
  $SourceRoot = $managedSource
  if (-not (Test-Path -LiteralPath (Join-Path $SourceRoot ".git"))) {
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $SourceRoot) | Out-Null
    & git init $SourceRoot
    if ($LASTEXITCODE -ne 0) { throw "Could not initialize the Bitwarden source cache." }
    & git -C $SourceRoot remote add origin $Repository
    if ($LASTEXITCODE -ne 0) { throw "Could not configure the Bitwarden source remote." }
  }
  & git -C $SourceRoot fetch --depth 1 origin $Commit
  if ($LASTEXITCODE -ne 0) { throw "Could not fetch pinned Bitwarden commit $Commit." }
  & git -C $SourceRoot checkout --detach $Commit
  if ($LASTEXITCODE -ne 0) { throw "Could not check out pinned Bitwarden commit $Commit." }
}

$SourceRoot = [IO.Path]::GetFullPath($SourceRoot)
if (-not (Test-Path -LiteralPath (Join-Path $SourceRoot ".git"))) {
  throw "Bitwarden source is not a Git checkout: $SourceRoot"
}
$actualRemote = (& git -C $SourceRoot remote get-url origin).Trim().TrimEnd('/')
if ($LASTEXITCODE -ne 0 -or $actualRemote -ne $Repository) {
  throw "Bitwarden source origin must be $Repository; found $actualRemote"
}
$actualCommit = (& git -C $SourceRoot rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0 -or $actualCommit -ne $Commit) {
  throw "Bitwarden source must be pinned to $Commit; found $actualCommit"
}
$trackedChanges = (& git -C $SourceRoot status --porcelain --untracked-files=no | Out-String).Trim()
if ($LASTEXITCODE -ne 0 -or $trackedChanges) {
  throw "Bitwarden source has tracked changes. Build only the clean pinned commit."
}

Push-Location $SourceRoot
try {
  $dependencyMarkers = @(
    "node_modules\webpack\bin\webpack.js",
    "node_modules\cross-env\dist\bin\cross-env.js",
    "node_modules\@yao-pkg\pkg\lib-es5\bin.js"
  )
  $dependenciesReady = -not ($dependencyMarkers | Where-Object {
    -not (Test-Path -LiteralPath (Join-Path $SourceRoot $_) -PathType Leaf)
  })
  if ($dependenciesReady) {
    Write-Host "Reusing the installed dependency tree for the pinned Bitwarden source"
  } else {
    Write-Host "Installing dependencies for QuickPal Bitwarden CLI $Version"
    & npx.cmd --yes --package "node@$BuildNodeVersion" --package "npm@$BuildNpmVersion" --call `
      "npm ci --ignore-scripts --no-audit --no-fund"
    if ($LASTEXITCODE -ne 0) { throw "Bitwarden dependency installation failed." }
  }

  Write-Host "Building QuickPal Bitwarden CLI from $($Commit.Substring(0, 12))"
  & npx.cmd --yes --package "node@$BuildNodeVersion" --package "npm@$BuildNpmVersion" --call `
    "npm run build:oss:prod --workspace apps/cli"
  if ($LASTEXITCODE -ne 0) { throw "Bitwarden CLI source build failed." }

  & npx.cmd --yes --package "node@$PackageNodeVersion" --package "npm@$PackageNpmVersion" --call `
    "npm run clean --workspace apps/cli && npm run package:oss:win --workspace apps/cli"
  if ($LASTEXITCODE -ne 0) { throw "Bitwarden CLI Windows packaging failed." }
} finally {
  Pop-Location
}

$builtExe = Join-Path $SourceRoot "apps\cli\dist\oss\windows\bw.exe"
if (-not (Test-BitwardenOutput $builtExe)) {
  throw "The source-built Bitwarden CLI failed its version or secure-serve verification."
}

New-Item -ItemType Directory -Force -Path $VendorRoot | Out-Null
Copy-Item -LiteralPath $builtExe -Destination $VendorExe -Force
foreach ($license in @("LICENSE.txt", "LICENSE_GPL.txt", "LICENSE_BITWARDEN.txt")) {
  Copy-NormalizedLicense (Join-Path $SourceRoot $license) (Join-Path $VendorRoot $license)
}

$sha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $VendorExe).Hash
$provenance = [ordered]@{
  repository = $Repository
  commit = $Commit
  version = $Version
  sha256 = $sha256
  buildNode = $BuildNodeVersion
  buildNpm = $BuildNpmVersion
  packageNode = $PackageNodeVersion
  packageNpm = $PackageNpmVersion
  generatedAtUtc = [DateTime]::UtcNow.ToString("o")
}
[IO.File]::WriteAllText(
  $ProvenancePath,
  ($provenance | ConvertTo-Json) + [Environment]::NewLine,
  [Text.UTF8Encoding]::new($false)
)
Write-Host "Built and verified QuickPal Bitwarden CLI $Version"
Write-Host "Source commit: $Commit"
Write-Host "SHA-256: $sha256"
