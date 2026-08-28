$ErrorActionPreference = "Stop"

$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
$Src = Join-Path $Root "src"
$Bin = Join-Path $Root "bin"
New-Item -ItemType Directory -Force -Path $Bin | Out-Null

$Res = Join-Path $Bin "QuickPal.res"
$Exe = Join-Path $Bin "QuickPal.exe"
$NativeHostExe = Join-Path $Bin "QuickPalChromeHost.exe"
$ExtensionDir = Join-Path $Root "chrome-extension"
$ExtensionZip = Join-Path $Bin "QuickPalChromeTabsExtension.zip"

# A running instance holds a lock on the exe.
Get-Process -Name QuickPal -ErrorAction SilentlyContinue | ForEach-Object {
  Write-Host "Stopping running QuickPal (pid $($_.Id))"
  $_.Kill()
  $_.WaitForExit(5000) | Out-Null
}
Get-Process -Name QuickPalChromeHost -ErrorAction SilentlyContinue | ForEach-Object {
  Write-Host "Stopping running QuickPalChromeHost (pid $($_.Id))"
  $_.Kill()
  $_.WaitForExit(5000) | Out-Null
}

& windres (Join-Path $Src "QuickPal.rc") -O coff -o $Res
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$Sources = Get-ChildItem -Path $Src -Recurse -Filter *.cpp |
  Where-Object { $_.FullName -notlike (Join-Path $Src "native_host\*") } |
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
)

& g++ @CompilerArgs
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host "Built $Exe"

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
