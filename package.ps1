param(
  [ValidatePattern('^\d+\.\d+\.\d+$')]
  [string]$Version = "0.1.0",
  [switch]$SkipBuild,
  [switch]$SkipInstaller,
  [string]$BitwardenSourceRoot = ""
)

$ErrorActionPreference = "Stop"

$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
$Bin = Join-Path $Root "bin"
$Dist = Join-Path $Root "dist"
$EverythingDll = Join-Path $Bin "Everything64.dll"
$EverythingLicense = Join-Path $Root "third_party\Everything\LICENSE.txt"
$BitwardenExe = Join-Path $Bin "bw.exe"
$BitwardenThirdParty = Join-Path $Root "third_party\Bitwarden"
$BitwardenProvenance = Join-Path $BitwardenThirdParty "PROVENANCE.json"
$BitwardenVersion = "2026.8.0"
$BitwardenRepository = "https://github.com/TheItalianGame/quickpal-bitwarden.git"
$BitwardenCommit = "2e4b1fba6e02ca9822043ba89deb8809468e0b7b"

if (-not $SkipBuild) {
  $buildArgs = @{ Version = $Version }
  if ($BitwardenSourceRoot) {
    $buildArgs.BitwardenSourceRoot = $BitwardenSourceRoot
  }
  & (Join-Path $Root "build.ps1") @buildArgs
  if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

$requiredFiles = @(
  (Join-Path $Bin "QuickPal.exe"),
  (Join-Path $Bin "QuickPalChromeHost.exe"),
  (Join-Path $Bin "QuickPalChromeTabsExtension.zip"),
  $BitwardenExe,
  $EverythingDll,
  $EverythingLicense,
  (Join-Path $BitwardenThirdParty "NOTICE.txt"),
  (Join-Path $BitwardenThirdParty "LICENSE.txt"),
  (Join-Path $BitwardenThirdParty "LICENSE_GPL.txt"),
  (Join-Path $BitwardenThirdParty "LICENSE_BITWARDEN.txt"),
  $BitwardenProvenance,
  (Join-Path $Root "README.md")
)
foreach ($file in $requiredFiles) {
  if (-not (Test-Path -LiteralPath $file -PathType Leaf)) {
    throw "Required release file is missing: $file"
  }
}

$builtVersion = (Get-Item -LiteralPath (Join-Path $Bin "QuickPal.exe")).VersionInfo.ProductVersion
if ($builtVersion -ne $Version) {
  throw "QuickPal.exe is version $builtVersion but package version is $Version. Rebuild without -SkipBuild."
}

$signature = Get-AuthenticodeSignature -LiteralPath $EverythingDll
if ($signature.Status -ne [System.Management.Automation.SignatureStatus]::Valid -or
    $signature.SignerCertificate.Subject -notmatch 'voidtools') {
  throw "Everything64.dll does not have the expected valid voidtools signature."
}

$provenance = Get-Content -Raw -LiteralPath $BitwardenProvenance | ConvertFrom-Json
if ($provenance.repository -ne $BitwardenRepository -or
    $provenance.commit -ne $BitwardenCommit -or
    $provenance.version -ne $BitwardenVersion) {
  throw "Bitwarden build provenance does not match the pinned QuickPal fork."
}
$bitwardenHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $BitwardenExe).Hash
if ($bitwardenHash -ne $provenance.sha256) {
  throw "Bitwarden CLI SHA-256 does not match its source-build provenance."
}
$actualBitwardenVersion = (& $BitwardenExe --version).Trim()
if ($actualBitwardenVersion -ne $BitwardenVersion) {
  throw "Bitwarden CLI version mismatch. Expected $BitwardenVersion, got $actualBitwardenVersion"
}
$bitwardenServeHelp = (& $BitwardenExe serve --help 2>&1 | Out-String)
if ($LASTEXITCODE -ne 0 -or -not $bitwardenServeHelp.Contains("--auth-token-env")) {
  throw "Bitwarden CLI does not expose the required authenticated serve transport."
}

New-Item -ItemType Directory -Force -Path $Dist | Out-Null
$packageName = "QuickPal-$Version-win-x64"
$stage = Join-Path $Dist $packageName
$zip = Join-Path $Dist ($packageName + ".zip")
$msi = Join-Path $Dist ($packageName + ".msi")
$releaseSums = Join-Path $Dist ("QuickPal-$Version-SHA256SUMS.txt")

function Assert-DistChild([string]$Path) {
  $distFull = [IO.Path]::GetFullPath($Dist).TrimEnd('\', '/')
  $pathFull = [IO.Path]::GetFullPath($Path)
  $prefix = $distFull + [IO.Path]::DirectorySeparatorChar
  if (-not $pathFull.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) {
    throw "Refusing to modify a path outside the release directory: $pathFull"
  }
  return $pathFull
}

$stage = Assert-DistChild $stage
$zip = Assert-DistChild $zip
$msi = Assert-DistChild $msi
$releaseSums = Assert-DistChild $releaseSums
if (Test-Path -LiteralPath $stage) {
  Remove-Item -LiteralPath $stage -Recurse -Force
}
if (Test-Path -LiteralPath $zip) {
  Remove-Item -LiteralPath $zip -Force
}
if (Test-Path -LiteralPath $msi) {
  Remove-Item -LiteralPath $msi -Force
}
if (Test-Path -LiteralPath $releaseSums) {
  Remove-Item -LiteralPath $releaseSums -Force
}

$stageBin = Join-Path $stage "bin"
$stageLicense = Join-Path $stage "licenses"
$stageBitwardenLicense = Join-Path $stageLicense "Bitwarden"
New-Item -ItemType Directory -Force -Path $stageBin, $stageLicense, $stageBitwardenLicense | Out-Null

Copy-Item -LiteralPath (Join-Path $Bin "QuickPal.exe") -Destination $stageBin
Copy-Item -LiteralPath (Join-Path $Bin "QuickPalChromeHost.exe") -Destination $stageBin
Copy-Item -LiteralPath (Join-Path $Bin "QuickPalChromeTabsExtension.zip") -Destination $stageBin
Copy-Item -LiteralPath $BitwardenExe -Destination $stageBin
Copy-Item -LiteralPath $EverythingDll -Destination $stageBin
Copy-Item -LiteralPath (Join-Path $Root "chrome-extension") -Destination $stage -Recurse
Copy-Item -LiteralPath (Join-Path $Root "README.md") -Destination $stage
Copy-Item -LiteralPath $EverythingLicense -Destination (Join-Path $stageLicense "Everything.txt")
Copy-Item -LiteralPath (Join-Path $BitwardenThirdParty "NOTICE.txt") -Destination $stageBitwardenLicense
Copy-Item -LiteralPath (Join-Path $BitwardenThirdParty "LICENSE.txt") -Destination $stageBitwardenLicense
Copy-Item -LiteralPath (Join-Path $BitwardenThirdParty "LICENSE_GPL.txt") -Destination $stageBitwardenLicense
Copy-Item -LiteralPath (Join-Path $BitwardenThirdParty "LICENSE_BITWARDEN.txt") -Destination $stageBitwardenLicense
Copy-Item -LiteralPath $BitwardenProvenance -Destination $stageBitwardenLicense

$manifestPath = Join-Path $stage "SHA256SUMS.txt"
$manifestLines = Get-ChildItem -LiteralPath $stage -Recurse -File |
  Sort-Object FullName |
  ForEach-Object {
    $relative = [IO.Path]::GetRelativePath($stage, $_.FullName).Replace('\', '/')
    $hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $_.FullName).Hash.ToLowerInvariant()
    "$hash  $relative"
  }
[IO.File]::WriteAllLines($manifestPath, $manifestLines, [Text.UTF8Encoding]::new($false))

Compress-Archive -Path (Join-Path $stage "*") -DestinationPath $zip -CompressionLevel Optimal

$releaseFiles = @($zip)
if (-not $SkipInstaller) {
  $wix = Get-Command wix.exe -ErrorAction SilentlyContinue
  if (-not $wix) {
    $dotnetWix = Join-Path $env:USERPROFILE ".dotnet\tools\wix.exe"
    if (Test-Path -LiteralPath $dotnetWix -PathType Leaf) {
      $wix = Get-Item -LiteralPath $dotnetWix
    }
  }
  if (-not $wix) {
    throw "WiX 6 was not found. Install it with: dotnet tool install --global wix --version 6.0.2"
  }
  $wixPath = if ($wix -is [System.Management.Automation.ApplicationInfo]) {
    $wix.Source
  } else {
    $wix.FullName
  }

  & $wixPath build (Join-Path $Root "installer\QuickPal.wxs") `
    -arch x64 `
    -d "ReleaseRoot=$stage" `
    -d "AppVersion=$Version" `
    -pdbtype none `
    -o $msi
  if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
  $releaseFiles += $msi
}

$releaseLines = foreach ($file in $releaseFiles) {
  $item = Get-Item -LiteralPath $file
  $hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $file).Hash.ToLowerInvariant()
  "$hash  $($item.Name)"
}
[IO.File]::WriteAllLines($releaseSums, $releaseLines, [Text.UTF8Encoding]::new($false))

foreach ($file in $releaseFiles) {
  $item = Get-Item -LiteralPath $file
  $hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $file).Hash
  Write-Host "Built $($item.FullName)"
  Write-Host "Size: $($item.Length) bytes"
  Write-Host "SHA-256: $hash"
}
Write-Host "Built $releaseSums"
