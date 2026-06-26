# install.ps1 — instala/desinstala Lux con acceso directo en el menú de inicio y "Abrir con".
#
#   .\install.ps1              instala (per-machine si sos admin, si no per-user, sin UAC)
#   .\install.ps1 -Machine     fuerza Program Files (se auto-eleva con UAC)
#   .\install.ps1 -PerUser     fuerza per-user (LOCALAPPDATA, sin admin)
#   .\install.ps1 -Uninstall   desinstala (agregá -Machine si lo instalaste elevado)
#
# Idempotente. Per-machine escribe en Program Files + HKLM; per-user en LOCALAPPDATA + HKCU.

param(
  [switch]$Uninstall,
  [switch]$Machine,
  [switch]$PerUser
)
$ErrorActionPreference = 'Stop'

$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
           ).IsInRole([Security.Principal.WindowsBuiltinRole]::Administrator)

# Elegir scope: -Machine o -PerUser explícito; por defecto machine si admin, si no per-user.
if ($Machine -and $PerUser) { throw "Elegí -Machine o -PerUser, no ambos." }
if (-not $Machine -and -not $PerUser) { if ($isAdmin) { $Machine = $true } else { $PerUser = $true } }

# -Machine sin admin -> auto-elevar (UAC). Si el UAC se cancela, caer a per-user.
if ($Machine -and -not $isAdmin) {
  Write-Host "Instalación per-machine: elevando (UAC)..." -ForegroundColor Yellow
  $a = @('-NoProfile','-ExecutionPolicy','Bypass','-File', "`"$PSCommandPath`"", '-Machine')
  if ($Uninstall) { $a += '-Uninstall' }
  try {
    Start-Process powershell.exe -Verb RunAs -ArgumentList $a -Wait -ErrorAction Stop
    return
  } catch {
    Write-Warning "UAC cancelado; instalo per-user (sin admin)."
    $Machine = $false; $PerUser = $true
  }
}

if ($Machine) {
  $InstallDir   = "$env:ProgramFiles\Lux"
  $startMenuDir = "$env:ProgramData\Microsoft\Windows\Start Menu\Programs"
  $hive = 'HKLM:'
} else {
  $InstallDir   = "$env:LOCALAPPDATA\Programs\Lux"
  $startMenuDir = "$env:APPDATA\Microsoft\Windows\Start Menu\Programs"
  $hive = 'HKCU:'
}

try { Start-Transcript -Path "$env:TEMP\lux_install.log" -Force | Out-Null } catch {}

$AppName   = 'Lux'
$Version   = '1.0.0'
$Publisher = 'Agustin Yarrus'
$exeName   = 'lux.exe'
$src       = $PSScriptRoot
$scopeFlag = if ($Machine) { '-Machine' } else { '-PerUser' }

$exts = @('jpg','jpeg','jpe','jfif','jif','png','gif','bmp','dib','ico','cur','tif','tiff',
          'dds','jxr','wdp','hdp','webp','heic','heif','avif','jxl',
          'tga','hdr','ppm','pgm','pbm','pnm','pic','psd')

$startMenu = Join-Path $startMenuDir "$AppName.lnk"
$uninstKey = "$hive\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\$AppName"
$appsKey   = "$hive\SOFTWARE\Classes\Applications\$exeName"

Get-Process lux -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 400

if ($Uninstall) {
  Remove-Item $startMenu -Force -ErrorAction SilentlyContinue
  Remove-Item $uninstKey -Recurse -Force -ErrorAction SilentlyContinue
  Remove-Item $appsKey   -Recurse -Force -ErrorAction SilentlyContinue
  if (Test-Path $InstallDir) { Remove-Item $InstallDir -Recurse -Force -ErrorAction SilentlyContinue }
  Write-Host "Lux desinstalado ($scopeFlag)."
  try { Stop-Transcript | Out-Null } catch {}
  return
}

# --- copiar archivos ----------------------------------------------------
if (-not (Test-Path (Join-Path $src $exeName))) { throw "No encuentro $exeName en $src (compilá con .\build.ps1)" }
New-Item -ItemType Directory -Force $InstallDir | Out-Null
Copy-Item (Join-Path $src $exeName) (Join-Path $InstallDir $exeName) -Force
foreach ($f in 'lux.ico','README.md','LICENSE','install.ps1') {
  if (Test-Path (Join-Path $src $f)) { Copy-Item (Join-Path $src $f) (Join-Path $InstallDir $f) -Force }
}
$exe = Join-Path $InstallDir $exeName

# --- acceso directo en el menú de inicio --------------------------------
$wsh = New-Object -ComObject WScript.Shell
$lnk = $wsh.CreateShortcut($startMenu)
$lnk.TargetPath       = $exe
$lnk.WorkingDirectory = $InstallDir
$lnk.IconLocation     = "$exe,0"
$lnk.Description       = 'Visor de imágenes dark y minimalista'
$lnk.Save()

# --- entrada en Agregar o quitar programas ------------------------------
New-Item -Path $uninstKey -Force | Out-Null
Set-ItemProperty $uninstKey DisplayName     $AppName
Set-ItemProperty $uninstKey DisplayIcon     "$exe,0"
Set-ItemProperty $uninstKey DisplayVersion  $Version
Set-ItemProperty $uninstKey Publisher       $Publisher
Set-ItemProperty $uninstKey InstallLocation $InstallDir
Set-ItemProperty $uninstKey UninstallString "powershell.exe -NoProfile -ExecutionPolicy Bypass -File `"$InstallDir\install.ps1`" -Uninstall $scopeFlag"
Set-ItemProperty $uninstKey NoModify 1 -Type DWord
Set-ItemProperty $uninstKey NoRepair 1 -Type DWord
try { Set-ItemProperty $uninstKey EstimatedSize ([math]::Round((Get-Item $exe).Length/1KB)) -Type DWord } catch {}

# --- registrar para "Abrir con" (no pisa el default actual) -------------
New-Item -Path "$appsKey\shell\open\command" -Force | Out-Null
Set-ItemProperty $appsKey '(default)' $AppName
Set-ItemProperty $appsKey 'FriendlyAppName' $AppName
New-Item -Path "$appsKey\DefaultIcon" -Force | Out-Null
Set-ItemProperty "$appsKey\DefaultIcon" '(default)' "$exe,0"
Set-ItemProperty "$appsKey\shell\open\command" '(default)' "`"$exe`" `"%1`""
New-Item -Path "$appsKey\SupportedTypes" -Force | Out-Null
foreach ($e in $exts) { Set-ItemProperty "$appsKey\SupportedTypes" ".$e" '' }

$scopeTxt = if ($Machine) { 'per-machine' } else { 'per-user' }
Write-Host "Lux $Version instalado ($scopeTxt) en $InstallDir" -ForegroundColor Green
Write-Host "Acceso directo: $startMenu"
try { Stop-Transcript | Out-Null } catch {}
