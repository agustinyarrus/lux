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
  # formatos nuevos
  'ref.acbm'      = 4;    'rle.ras'    = 4;    'ref8.pcx'   = 4
  'ref4.pcx'      = 4;    'ref1.pcx'   = 4;    'ref.dcx'    = 4
  'ref.pi1'       = 320;  'ref.pc1'    = 320;  'ref.neo'    = 320
  'ref.scr'       = 256;  'ref.koa'    = 320;  'ref.img'    = 16
  'ref.tim'       = 8;    'ref.pix'    = 4;    'bc1.dds'    = 4
  'bc3.dds'       = 4;    'ref.vtf'    = 4;    'ref.ktx'    = 4
  'ref.fits'      = 4;    'ref.dcm'    = 4;    'impl.dcm'   = 4
  'ref.wmf'       = 1600; 'v1.xcf'     = 4;    'v11.xcf'    = 4
  'ref.ppm.gz'    = 4
}

# Casos donde importa la forma de la ventana, no solo que haya decodificado algo.
$shapes = @{ 'rot90.tif' = 'alta' }     # 600x300 con EXIF Orientation=6 -> se ve 300x600

# Archivos que se fabrican en el momento porque necesitan Windows: un metarchivo
# EMF de verdad, un "RAW" con la vista previa JPEG adentro y un ejecutable con icono.
$gen = Join-Path $tmp 'gen'
New-Item -ItemType Directory -Force $gen | Out-Null
try {
  Add-Type -AssemblyName System.Drawing
  $emf = Join-Path $gen 'vector.emf'
  $tmpBmp = New-Object System.Drawing.Bitmap 1, 1
  $g0 = [System.Drawing.Graphics]::FromImage($tmpBmp)
  $hdc = $g0.GetHdc()
  $mf = New-Object System.Drawing.Imaging.Metafile($emf, $hdc,
        (New-Object System.Drawing.RectangleF 0, 0, 240, 160),
        [System.Drawing.Imaging.MetafileFrameUnit]::Pixel)
  $g0.ReleaseHdc($hdc); $g0.Dispose(); $tmpBmp.Dispose()
  $gm = [System.Drawing.Graphics]::FromImage($mf)
  $gm.FillRectangle([System.Drawing.Brushes]::Tomato, 10, 10, 220, 140)
  $gm.DrawLine([System.Drawing.Pens]::Navy, 0, 0, 240, 160)
  $gm.Dispose(); $mf.Dispose()

  # RAW falso: basura, un JPEG de 900x600 y mas basura. Lux tiene que encontrarlo.
  $jb = New-Object System.Drawing.Bitmap 900, 600
  $gj = [System.Drawing.Graphics]::FromImage($jb)
  $gj.Clear([System.Drawing.Color]::SteelBlue)
  $gj.FillEllipse([System.Drawing.Brushes]::Gold, 100, 100, 400, 300)
  $gj.Dispose()
  $ms = New-Object System.IO.MemoryStream
  $jb.Save($ms, [System.Drawing.Imaging.ImageFormat]::Jpeg)
  $jb.Dispose()
  $rnd = New-Object byte[] 4096
  (New-Object Random 7).NextBytes($rnd)
  $raw = Join-Path $gen 'fake.nef'
  $fs = [System.IO.File]::Create($raw)
  $fs.Write($rnd, 0, $rnd.Length)
  $b = $ms.ToArray(); $fs.Write($b, 0, $b.Length)
  $fs.Write($rnd, 0, 512)
  $fs.Close(); $ms.Dispose()
} catch { Write-Host "  [SKIP] no se pudieron fabricar EMF/RAW: $_" -ForegroundColor Yellow }

# ruta completa -> ancho esperado, para los que no viven en samples\
$extra = [ordered]@{}
if (Test-Path (Join-Path $gen 'vector.emf')) { $extra[(Join-Path $gen 'vector.emf')] = 1600 }
if (Test-Path (Join-Path $gen 'fake.nef'))   { $extra[(Join-Path $gen 'fake.nef')]   = 900 }
$shell = Join-Path $env:WINDIR 'system32\shell32.dll'
if (Test-Path $shell) { $extra[$shell] = 256 }

$old = $env:APPDATA
$env:APPDATA = $tmp
$fail = 0
try {
  $all = [ordered]@{}
  foreach ($k in $cases.Keys) { $all[(Join-Path $root "samples\$k")] = $cases[$k] }
  foreach ($k in $shapes.Keys) { $all[(Join-Path $root "samples\$k")] = 0 }
  foreach ($k in $extra.Keys)  { $all[$k] = $extra[$k] }
  foreach ($full in $all.Keys) {
    $path = $full
    $f = Split-Path $full -Leaf
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
    #   want = 0      -> caso de forma: la ventana tiene que quedar mas alta que ancha
    $want = $all[$full]
    $okSize = (-not $died) -and $w -gt 0 -and -not ($w -eq 800 -and $h -eq 600)
    if ($want -eq 0)        { $okSize = $okSize -and ($h -gt $w) }   # rotada por EXIF
    elseif ($want -ge 500)  { $okSize = $okSize -and ($h -gt 300) }  # crecio con la imagen
    else                    { $okSize = $okSize -and ($h -lt 400) }  # se quedo en el minimo
    if ($okSize) { Write-Host ("  [OK]   {0,-16} ventana {1}x{2}  (imagen {3} px)" -f $f, $w, $h, $want) }
    else { $fail++; Write-Host ("  [FAIL] {0,-16} ventana {1}x{2} murio={3} (imagen {4} px)" -f $f, $w, $h, $died, $want) -ForegroundColor Red }
  }
} finally {
  $env:APPDATA = $old
  Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
}
if ($fail) { Write-Host "$fail casos en rojo" -ForegroundColor Red; exit 1 }
Write-Host "smoke OK" -ForegroundColor Green
