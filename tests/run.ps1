# run.ps1 - genera las muestras, compila test_decoders.exe y lo corre.
#   .\run.ps1            todo
#   .\run.ps1 -NoGen     saltea la regeneracion de muestras
[CmdletBinding()]
param([switch]$NoGen)
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
Push-Location $root
try {
  if (-not $NoGen) {
    # (nada de ?? aca: esto tiene que correr tambien en la PowerShell 5.1 de Windows)
    $py = Get-Command python -ErrorAction SilentlyContinue
    if (-not $py) { $py = Get-Command py -ErrorAction SilentlyContinue }
    if (-not $py) { throw "python no encontrado (hace falta para generar las muestras)" }
    & $py.Source "gen_samples.py"
    if ($LASTEXITCODE -ne 0) { throw "gen_samples.py fallo" }
  }

  $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
  if (-not (Test-Path $vswhere)) { throw "vswhere.exe no encontrado (instala Visual Studio)" }
  $vsPath = & $vswhere -latest -products * -property installationPath
  if (-not $vsPath) {   # instancia "incompleta" (un update a medias): -latest la esconde, -all la muestra
    $vsPath = & $vswhere -all -prerelease -latest -products * -property installationPath
  }
  if (-not $vsPath) { throw "vswhere no encontro ninguna instalacion de Visual Studio" }
  $vcvars = Join-Path $vsPath "VC\Auxiliary\Build\vcvars64.bat"
  if (-not (Test-Path $vcvars)) { throw "vcvars64.bat no encontrado en $vsPath" }

  $cmd = @"
@echo off
call "$vcvars" >nul
if errorlevel 1 ( echo [vcvars fallo] & exit /b 1 )
cd /d "$root"
cl /nologo /O2 /MT /EHsc /W4 /std:c++17 /utf-8 test_decoders.cpp /Fe:test_decoders.exe /Fo:test_decoders.obj
exit /b %errorlevel%
"@
  $bat = Join-Path $env:TEMP "lux_test_build.cmd"
  Set-Content -LiteralPath $bat -Value $cmd -Encoding ASCII

  Write-Host "Compilando test_decoders.exe..." -ForegroundColor Cyan
  & cmd /c "`"$bat`""
  if ($LASTEXITCODE -ne 0) { throw "compilacion fallo (exit $LASTEXITCODE)" }
  Remove-Item -ErrorAction SilentlyContinue test_decoders.obj, syn.obj -Force

  & "$root\test_decoders.exe"
  $rc = $LASTEXITCODE
  if ($rc -ne 0) { Write-Host "TESTS EN ROJO" -ForegroundColor Red; exit $rc }
  Write-Host "TESTS OK" -ForegroundColor Green
}
finally { Pop-Location }
