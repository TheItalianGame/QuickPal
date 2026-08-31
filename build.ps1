param(
  [ValidatePattern('^\d+\.\d+\.\d+$')]
  [string]$Version = "0.1.0",
  [string]$BitwardenSourceRoot = ""
)

$ErrorActionPreference = "Stop"

$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
$Src = Join-Path $Root "src"
$Bin = Join-Path $Root "bin"
New-Item -ItemType Directory -Force -Path $Bin | Out-Null

$versionParts = @($Version.Split('.') | ForEach-Object { [int]$_ })
while ($versionParts.Count -lt 4) { $versionParts += 0 }
if ($versionParts | Where-Object { $_ -lt 0 -or $_ -gt 65535 }) {
  throw "Every version component must be between 0 and 65535: $Version"
}
$versionHeader = Join-Path $Bin "QuickPalVersion.h"
$versionHeaderLines = @(
  "#define QUICKPAL_VERSION_MAJOR $($versionParts[0])",
  "#define QUICKPAL_VERSION_MINOR $($versionParts[1])",
  "#define QUICKPAL_VERSION_PATCH $($versionParts[2])",
  "#define QUICKPAL_VERSION_BUILD $($versionParts[3])",
  "#define QUICKPAL_VERSION_STRING `"$Version`""
)
[IO.File]::WriteAllLines($versionHeader, $versionHeaderLines, [Text.UTF8Encoding]::new($false))

$Res = Join-Path $Bin "QuickPal.res"
$Exe = Join-Path $Bin "QuickPal.exe"
$BenchExe = Join-Path $Bin "QuickPalBench.exe"
$NativeHostExe = Join-Path $Bin "QuickPalChromeHost.exe"
$ExtensionDir = Join-Path $Root "chrome-extension"
$ExtensionZip = Join-Path $Bin "QuickPalChromeTabsExtension.zip"
$BitwardenVendorExe = Join-Path $Root "third_party\Bitwarden\bw.exe"
$BundledBitwardenExe = Join-Path $Bin "bw.exe"
$BitwardenProvenance = Join-Path $Root "third_party\Bitwarden\PROVENANCE.json"
$BitwardenVersion = "2026.8.0"
$BitwardenRepository = "https://github.com/TheItalianGame/quickpal-bitwarden.git"
$BitwardenCommit = "2e4b1fba6e02ca9822043ba89deb8809468e0b7b"

function Stop-BuildOutputProcess([string]$ProcessName, [string]$OutputPath) {
  $expectedPath = [IO.Path]::GetFullPath($OutputPath)
  Get-Process -Name $ProcessName -ErrorAction SilentlyContinue | ForEach-Object {
    $processPath = $null
    try { $processPath = $_.Path } catch { }
    if ($processPath -and
        [IO.Path]::GetFullPath($processPath).Equals($expectedPath, [StringComparison]::OrdinalIgnoreCase)) {
      Write-Host "Stopping source-build $ProcessName (pid $($_.Id))"
      $_.Kill()
      $_.WaitForExit(5000) | Out-Null
    }
  }
}

# Stop only processes running from this checkout. Never disturb an installed copy.
Stop-BuildOutputProcess "QuickPal" $Exe
Stop-BuildOutputProcess "QuickPalChromeHost" $NativeHostExe
Stop-BuildOutputProcess "QuickPalBench" $BenchExe

$buildBitwardenArgs = @{}
if ($BitwardenSourceRoot) {
  $buildBitwardenArgs.SourceRoot = $BitwardenSourceRoot
}
& (Join-Path $Root "build-bitwarden.ps1") @buildBitwardenArgs
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

if (-not (Test-Path -LiteralPath $BitwardenProvenance -PathType Leaf)) {
  throw "Bitwarden build provenance is missing: $BitwardenProvenance"
}
$provenance = Get-Content -Raw -LiteralPath $BitwardenProvenance | ConvertFrom-Json
if ($provenance.repository -ne $BitwardenRepository -or
    $provenance.commit -ne $BitwardenCommit -or
    $provenance.version -ne $BitwardenVersion) {
  throw "Bitwarden build provenance does not match the pinned QuickPal fork."
}
$bitwardenHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $BitwardenVendorExe).Hash
if ($bitwardenHash -ne $provenance.sha256) {
  throw "Bitwarden CLI SHA-256 does not match its source-build provenance."
}
$actualBitwardenVersion = (& $BitwardenVendorExe --version).Trim()
if ($actualBitwardenVersion -ne $BitwardenVersion) {
  throw "Bitwarden CLI version mismatch. Expected $BitwardenVersion, got $actualBitwardenVersion"
}
$bitwardenServeHelp = (& $BitwardenVendorExe serve --help 2>&1 | Out-String)
if ($LASTEXITCODE -ne 0 -or -not $bitwardenServeHelp.Contains("--auth-token-env")) {
  throw "Bitwarden CLI does not expose the required authenticated serve transport."
}
Copy-Item -LiteralPath $BitwardenVendorExe -Destination $BundledBitwardenExe -Force
Write-Host "Staged source-built QuickPal Bitwarden CLI $BitwardenVersion ($($BitwardenCommit.Substring(0, 12)))"

& windres -I $Bin (Join-Path $Src "QuickPal.rc") -O coff -o $Res
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$Sources = Get-ChildItem -Path $Src -Recurse -Filter *.cpp |
  Where-Object {
    $_.FullName -notlike (Join-Path $Src "native_host\*") -and
    $_.FullName -notlike (Join-Path $Src "bench\*")
  } |
  Sort-Object FullName |
  ForEach-Object { $_.FullName }
Write-Host ("Compiling {0} translation units" -f $Sources.Count)

$CompilerArgs = @(
  "-std=c++20"
  "-O2"
  "-DNDEBUG"
  "-DUNICODE"
  "-D_UNICODE"
  "-mwindows"
  "-municode"
  "-static"
  "-static-libgcc"
  "-static-libstdc++"
) + $Sources + @(
  $Res
  "-o"
  $Exe
  # Direct2D / DirectWrite / WIC replace the old GDI drawing path.
  "-ld2d1"
  "-ldwrite"
  "-lwindowscodecs"
  "-ldxguid"
  "-luuid"
  "-ldwmapi"
  "-limm32"
  # Everything HTTP API client.
  "-lwinhttp"
  "-lpsapi"
  "-lole32"
  "-loleaut32"
  "-lshell32"
  "-lshlwapi"
  "-lcomctl32"
  "-lgdi32"
  "-luser32"
  "-ladvapi32"
  "-lcrypt32"
  "-lbcrypt"
  "-lruntimeobject"
)

& g++ @CompilerArgs
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host "Built $Exe"

$BenchSources = Get-ChildItem -Path (Join-Path $Src "core") -Recurse -Filter *.cpp |
  Sort-Object FullName |
  ForEach-Object { $_.FullName }
$BenchSources += Join-Path $Src "ui\theme.cpp"
$BenchSources += Join-Path $Src "bench\search_bench.cpp"
Write-Host ("Compiling benchmark with {0} translation units" -f $BenchSources.Count)

$BenchArgs = @(
  "-std=c++20"
  "-O2"
  "-DNDEBUG"
  "-DUNICODE"
  "-D_UNICODE"
  "-static"
  "-static-libgcc"
  "-static-libstdc++"
) + $BenchSources + @(
  "-o"
  $BenchExe
  "-lwinhttp"
  "-luuid"
  "-lpsapi"
  "-lole32"
  "-loleaut32"
  "-lshell32"
  "-lshlwapi"
  "-lcomctl32"
  "-lgdi32"
  "-luser32"
  "-ladvapi32"
  "-lcrypt32"
  "-lbcrypt"
  "-lruntimeobject"
)

& g++ @BenchArgs
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host "Built $BenchExe"

Stop-BuildOutputProcess "QuickPalChromeHost" $NativeHostExe

$NativeHostSource = Join-Path $Src "native_host\chrome_tabs_host.cpp"
$NativeHostArgs = @(
  "-std=c++20"
  "-O2"
  "-DNDEBUG"
  "-DUNICODE"
  "-D_UNICODE"
  "-static"
  "-static-libgcc"
  "-static-libstdc++"
  $NativeHostSource
  "-o"
  $NativeHostExe
  "-luser32"
  "-ladvapi32"
)

& g++ @NativeHostArgs
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host "Built $NativeHostExe"

if (Test-Path $ExtensionDir) {
  if (Test-Path $ExtensionZip) {
    Remove-Item -Force -LiteralPath $ExtensionZip
  }
  Compress-Archive -Path (Join-Path $ExtensionDir "*") -DestinationPath $ExtensionZip -Force
  Write-Host "Packed $ExtensionZip"
}
