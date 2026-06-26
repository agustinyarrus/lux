# build.ps1 - compila lux.exe (release, portable, sin consola) con MSVC.
#   .\build.ps1            compila
#   .\build.ps1 -Run       compila y abre una imagen de prueba
#   .\build.ps1 -Debug     build con simbolos y consola para diagnostico
[CmdletBinding()]
param(
  [switch]$Run,
  [switch]$Dbg   # build con simbolos + consola (no usar -Debug: lo reserva PowerShell)
)
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
Push-Location $root
try {
  # --- localizar MSVC ---
  $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
  if (-not (Test-Path $vswhere)) { throw "vswhere.exe no encontrado (instala Visual Studio)" }
  $vsPath = & $vswhere -latest -products * -property installationPath
  $vcvars = Join-Path $vsPath "VC\Auxiliary\Build\vcvars64.bat"
  if (-not (Test-Path $vcvars)) { throw "vcvars64.bat no encontrado en $vsPath" }

  if (-not (Test-Path "$root\lux.ico")) {
    Write-Warning "lux.ico no existe; generalo con .\gen-icon.ps1 (se compila sin icono)."
  }

  # --- flags ---
  if ($Dbg) {
    $clFlags  = '/nologo /Zi /Od /MTd /EHsc /DDEBUG /std:c++17 /utf-8'
    $linkExtra = '/SUBSYSTEM:CONSOLE /DEBUG'
  } else {
    $clFlags  = '/nologo /O2 /MT /EHsc /DNDEBUG /GL /Gy /std:c++17 /utf-8'
    $linkExtra = '/SUBSYSTEM:WINDOWS /LTCG /OPT:REF /OPT:ICF'
  }

  # compilar recursos solo si hay .rc + .ico
  $resObj = ''
  $rcLine = 'echo (sin recursos)'
  if ((Test-Path "$root\lux.rc") -and (Test-Path "$root\lux.ico")) {
    $rcLine = 'rc /nologo /fo lux.res lux.rc'
    $resObj = 'lux.res'
  }

  $cmd = @"
@echo off
call "$vcvars" >nul
if errorlevel 1 ( echo [vcvars fallo] & exit /b 1 )
$rcLine
if errorlevel 1 ( echo [rc fallo] & exit /b 1 )
cl $clFlags lux.cpp $resObj /Fe:lux.exe /link $linkExtra
exit /b %errorlevel%
"@
  $bat = Join-Path $env:TEMP "lux_build.cmd"
  Set-Content -LiteralPath $bat -Value $cmd -Encoding ASCII

  Write-Host "Compilando lux.exe..." -ForegroundColor Cyan
  & cmd /c "`"$bat`""
  if ($LASTEXITCODE -ne 0) { throw "compilacion fallo (exit $LASTEXITCODE)" }

  # limpiar intermedios
  Remove-Item -ErrorAction SilentlyContinue lux.obj, lux.res, lux.exp, lux.lib, lux.pdb -Force

  $exe = Get-Item "$root\lux.exe"
  $kb  = [math]::Round($exe.Length/1KB, 1)
  Write-Host ("OK -> lux.exe  ({0} KB)" -f $kb) -ForegroundColor Green

  if ($Run) {
    $sample = Get-ChildItem "$root\..\lumen\testimg" -Include *.jpg,*.png -Recurse -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($sample) { & "$root\lux.exe" $sample.FullName } else { & "$root\lux.exe" }
  }
}
finally { Pop-Location }
