# smoke.ps1 - prueba de integracion: abre cada muestra con lux.exe de verdad y
# mide la ventana. Con "ventana pegada a la imagen" activado, el tamano refleja
# lo que decodifico, asi que sirve de senal de que la cadena entera funciono
# (decodeAny -> D2D -> render), incluida la parte que usa WIC y nanosvg.
#
# Corre con un %APPDATA% propio: no toca la config real de Lux.
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$exe  = Join-Path (Split-Path $root -Parent) 'lux.exe'
if (-not (Test-Path $exe)) { throw "no existe $exe (compila con .\build.ps1)" }

Add-Type @'
using System;
using System.Runtime.InteropServices;
public struct R { public int L, T, Rr, B; }
public class Win {
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out R r);
}
'@ -ErrorAction SilentlyContinue

$tmp = Join-Path $root '_smoke'
Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force (Join-Path $tmp 'Lux') | Out-Null
'{ "fitWindowToImage": true, "maximized": false, "winX": 60, "winY": 60, "winW": 800, "winH": 600 }' |
  Set-Content -LiteralPath (Join-Path $tmp 'Lux\config.json') -Encoding UTF8

# archivo -> ancho de imagen esperado (el alto de la ventana suma el cromo).
# El SVGZ se rasteriza a 1600 px en el lado mayor, de ahi el numero grande.
$cases = [ordered]@{
  'ascii_p3.ppm'  = 4;    'ref.xpm'    = 4;    'ilbm_raw.iff' = 4
  'ilbm_rle.iff'  = 4;    'ilbm_ehb.iff' = 4;  'pbm_chunky.iff' = 4
  'ref.mac'       = 576;  'ref.xwd'    = 4;    'ref10.dpx'  = 4
  'ref8.dpx'      = 4;    'ref.cin'    = 4;    'png.icns'   = 64
  'rle.icns'      = 32;   'ref.svgz'   = 1600; 'ref.ora'    = 4
  'ref.kra'       = 4;    'ref.ani'    = 32
}

$old = $env:APPDATA
$env:APPDATA = $tmp
$fail = 0
try {
  foreach ($f in $cases.Keys) {
    $path = Join-Path $root "samples\$f"
    if (-not (Test-Path $path)) { Write-Host "  [SKIP] $f (no generado)"; continue }
    $p = Start-Process $exe -ArgumentList "`"$path`"" -PassThru
    $w = 0; $h = 0
    for ($i = 0; $i -lt 40; $i++) {
      Start-Sleep -Milliseconds 100
      $p.Refresh()
      if ($p.HasExited) { break }
      if ($p.MainWindowHandle -ne 0) {
        $r = New-Object R
        if ([Win]::GetWindowRect($p.MainWindowHandle, [ref]$r)) {
          $w = $r.Rr - $r.L; $h = $r.B - $r.T
          if ($i -ge 6) { break }   # dar tiempo a que aplique el sizing
        }
      }
    }
    $died = $p.HasExited
    if (-not $died) { $p.CloseMainWindow() | Out-Null; Start-Sleep -Milliseconds 250 }
    if (-not $p.HasExited) { $p.Kill() }

    # applyWindowSizing() solo corre si hubo bitmap: si la decodificacion fallara,
    # la ventana se quedaria en los 800x600 de la config. Ademas escala para que
    # entre en pantalla y clampea a un minimo de 420x280, asi que:
    #   imagen chica  -> exactamente el minimo
    #   imagen grande -> mas alta que el minimo
    $want = $cases[$f]
    $okSize = (-not $died) -and $w -gt 0 -and -not ($w -eq 800 -and $h -eq 600)
    if ($want -ge 500) { $okSize = $okSize -and ($h -gt 300) }   # crecio con la imagen
    else               { $okSize = $okSize -and ($h -lt 400) }   # se quedo en el minimo
    if ($okSize) { Write-Host ("  [OK]   {0,-16} ventana {1}x{2}  (imagen {3} px)" -f $f, $w, $h, $want) }
    else { $fail++; Write-Host ("  [FAIL] {0,-16} ventana {1}x{2} murio={3} (imagen {4} px)" -f $f, $w, $h, $died, $want) -ForegroundColor Red }
  }
} finally {
  $env:APPDATA = $old
  Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
}
if ($fail) { Write-Host "$fail casos en rojo" -ForegroundColor Red; exit 1 }
Write-Host "smoke OK" -ForegroundColor Green
