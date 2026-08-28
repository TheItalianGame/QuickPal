$ErrorActionPreference = "Stop"

$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
$Src = Join-Path $Root "src"
$Bin = Join-Path $Root "bin"
New-Item -ItemType Directory -Force -Path $Bin | Out-Null

$Res = Join-Path $Bin "QuickPal.res"
$Exe = Join-Path $Bin "QuickPal.exe"

# A running instance holds a lock on the exe.
Get-Process -Name QuickPal -ErrorAction SilentlyContinue | ForEach-Object {
  Write-Host "Stopping running QuickPal (pid $($_.Id))"
  $_.Kill()
  $_.WaitForExit(5000) | Out-Null
}

& windres (Join-Path $Src "QuickPal.rc") -O coff -o $Res
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$Sources = Get-ChildItem -Path $Src -Recurse -Filter *.cpp | Sort-Object FullName | ForEach-Object { $_.FullName }
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
