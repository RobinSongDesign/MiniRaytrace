# Stages the Windows yak package layout (design doc §6.4/§8 F1) and, if the
# yak CLI is available, builds the .yak file. Windows ships zero Vulkan
# binaries by design (§6.3) — mrt.dll relies on the GPU driver's own Vulkan
# loader at run time, so nothing Vulkan-related is staged here.
#
# Usage: pwsh rhino/yak/build-win.ps1 [-Configuration Release] [-OidnDir <path>]
#
# -OidnDir, if given, is expected to contain OpenImageDenoise*.dll and
# tbb12.dll (issue F4) and gets copied in as an optional extra; omit it to
# ship without OIDN (the plugin degrades to pure accumulation - PRD R5).

param(
    [string]$Configuration = "Release",
    [string]$OidnDir = ""
)

$ErrorActionPreference = "Stop"
$RepoRoot = Resolve-Path "$PSScriptRoot\..\.."
$RhinoProj = Join-Path $RepoRoot "rhino\MiniRaytrace.Rhino\MiniRaytrace.Rhino.csproj"
$Staging = Join-Path $PSScriptRoot "staging\win"

Write-Host "== building MiniRaytrace.Rhino ($Configuration) =="
dotnet build $RhinoProj -c $Configuration
if ($LASTEXITCODE -ne 0) { throw "dotnet build failed" }

$BinDir = Join-Path $RepoRoot "rhino\MiniRaytrace.Rhino\bin\$Configuration"
$NativeDll = Join-Path $BinDir "runtimes\win-x64\native\mrt.dll"
if (-not (Test-Path $NativeDll)) {
    throw "mrt.dll not found at $NativeDll — build the native 'mrt' CMake target first (see docs/rhino-integration-design.md §6.4), or pass -p:MrtNativeDll=<path> to the csproj build."
}

Write-Host "== staging $Staging =="
if (Test-Path $Staging) {
    # If some process is parked with $Staging as its working directory the
    # directory itself can't be removed - clearing its contents is enough.
    try { Remove-Item -Recurse -Force $Staging -ErrorAction Stop }
    catch { Get-ChildItem $Staging | Remove-Item -Recurse -Force }
}
New-Item -ItemType Directory -Force -Path "$Staging\runtimes\win-x64\native" | Out-Null

Copy-Item (Join-Path $BinDir "MiniRaytrace.Rhino.rhp") $Staging
Copy-Item $NativeDll "$Staging\runtimes\win-x64\native\"
Copy-Item (Join-Path $PSScriptRoot "manifest.yml") $Staging

if ($OidnDir -ne "") {
    if (-not (Test-Path $OidnDir)) { throw "-OidnDir '$OidnDir' does not exist" }
    Write-Host "== staging OIDN from $OidnDir (issue F4) =="
    Copy-Item (Join-Path $OidnDir "OpenImageDenoise*.dll") "$Staging\runtimes\win-x64\native\" -ErrorAction SilentlyContinue
    Copy-Item (Join-Path $OidnDir "tbb12.dll") "$Staging\runtimes\win-x64\native\" -ErrorAction SilentlyContinue
}

Write-Host "== staged files =="
Get-ChildItem -Recurse $Staging | ForEach-Object { Write-Host "  $($_.FullName.Substring($Staging.Length + 1))" }

$Yak = Get-Command yak.exe -ErrorAction SilentlyContinue
if ($null -eq $Yak) {
    Write-Host ""
    Write-Host "yak.exe not found on PATH - staged files are ready under $Staging."
    Write-Host "Install the yak CLI (ships with Rhino, e.g. 'C:\Program Files\Rhino 8\System\Yak.exe')"
    Write-Host "then run 'yak build' from inside $Staging to produce the .yak package,"
    Write-Host "and 'yak install <file>.yak' or the in-Rhino Package Manager to verify (F1 acceptance)."
    exit 0
}

Push-Location $Staging
try {
    # --platform win pins the package's platform tag explicitly (verified
    # against a real local Yak.exe 0.14.3 - without it, an X64-targeted .rhp
    # produces a "should be AnyCPU" warning and yak guesses "any"). The
    # rh8_NN portion of the output filename is derived by yak itself from
    # the RhinoCommon version the .rhp references - not independently
    # controllable here.
    & $Yak.Source build --platform win
    if ($LASTEXITCODE -ne 0) { throw "yak build failed" }
} finally {
    Pop-Location
}

Write-Host "== done: $(Get-ChildItem $Staging -Filter *.yak | Select-Object -First 1 -ExpandProperty FullName) =="
