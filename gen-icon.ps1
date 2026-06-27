# gen-icon.ps1 - genera el squircle azul-noche + sparkle (icono ORIGINAL de Lux).
# Correr con Windows PowerShell 5.1:  powershell -NoProfile -File .\gen-icon.ps1
#
# OJO: el repo ya trae iconos curados -> lux.ico (icono de la app) y lux-file.ico (icono de
#      los archivos asociados). Este script SOBREESCRIBE lux.ico con el diseño generado;
#      corrélo solo si querés volver al icono original.
Add-Type -AssemblyName System.Drawing

$ACC  = [Drawing.Color]::FromArgb(255,0x7A,0xA2,0xF7)  # accent Nocturne
$ACC2 = [Drawing.Color]::FromArgb(255,0x6C,0x98,0xF0)
$HOT  = [Drawing.Color]::FromArgb(255,0xEA,0xF1,0xFF)

function New-Squircle($rect, $rad) {
  $p = New-Object Drawing.Drawing2D.GraphicsPath
  $d = $rad * 2
  $p.AddArc($rect.Left,            $rect.Top,             $d, $d, 180, 90)
  $p.AddArc($rect.Right - $d,      $rect.Top,             $d, $d, 270, 90)
  $p.AddArc($rect.Right - $d,      $rect.Bottom - $d,     $d, $d,   0, 90)
  $p.AddArc($rect.Left,            $rect.Bottom - $d,     $d, $d,  90, 90)
  $p.CloseFigure()
  return $p
}

function Add-Star($cx, $cy, $Rout, $Rin) {
  $pts = New-Object System.Collections.ArrayList
  for ($k = 0; $k -lt 8; $k++) {
    $ang = ([math]::PI / 180.0) * (-90 + $k * 45)
    if ($k % 2 -eq 0) { $r = $Rout } else { $r = $Rin }
    [void]$pts.Add((New-Object Drawing.PointF([single]($cx + $r * [math]::Cos($ang)), [single]($cy + $r * [math]::Sin($ang)))))
  }
  $path = New-Object Drawing.Drawing2D.GraphicsPath
  $path.AddPolygon([Drawing.PointF[]]$pts.ToArray([Drawing.PointF]))
  return $path
}

function Draw-Lux($g, $S) {
  $g.SmoothingMode     = [Drawing.Drawing2D.SmoothingMode]::AntiAlias
  $g.InterpolationMode = [Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
  $g.PixelOffsetMode   = [Drawing.Drawing2D.PixelOffsetMode]::HighQuality
  $g.Clear([Drawing.Color]::Transparent)

  # squircle de fondo (gradiente vertical)
  $pad  = [single]([math]::Max(1, $S * 0.045))
  $rect = New-Object Drawing.RectangleF($pad, $pad, [single]($S - 2*$pad), [single]($S - 2*$pad))
  $rad  = [single]($S * 0.225)
  $sq   = New-Squircle $rect $rad
  $c1 = [Drawing.Color]::FromArgb(255,0x13,0x17,0x21)
  $c2 = [Drawing.Color]::FromArgb(255,0x0A,0x0C,0x12)
  $lg = New-Object Drawing.Drawing2D.LinearGradientBrush($rect, $c1, $c2, [single]90)
  $g.FillPath($lg, $sq)
  $pen = New-Object Drawing.Pen([Drawing.Color]::FromArgb(16,255,255,255), [single]([math]::Max(1, $S*0.008)))
  $g.DrawPath($pen, $sq)

  $cx = [single]($S/2); $cy = [single]($S/2)

  # halo / glow
  $gr = [single]($S * 0.44)
  $glow = New-Object Drawing.Drawing2D.GraphicsPath
  $glow.AddEllipse([single]($cx-$gr), [single]($cy-$gr), [single](2*$gr), [single](2*$gr))
  $pgb = New-Object Drawing.Drawing2D.PathGradientBrush($glow)
  $pgb.CenterPoint  = New-Object Drawing.PointF($cx, $cy)
  $pgb.CenterColor  = [Drawing.Color]::FromArgb(130,0x7A,0xA2,0xF7)
  $pgb.SurroundColors = @([Drawing.Color]::FromArgb(0,0x7A,0xA2,0xF7))
  $g.FillPath($pgb, $glow)

  # estrella principal de 4 puntas con centro caliente
  $star = Add-Star $cx $cy ([single]($S*0.34)) ([single]($S*0.085))
  $sp = New-Object Drawing.Drawing2D.PathGradientBrush($star)
  $sp.CenterPoint     = New-Object Drawing.PointF($cx, $cy)
  $sp.CenterColor     = $HOT
  $sp.SurroundColors  = @($ACC2)
  $g.FillPath($sp, $star)

  # nucleo brillante
  $cr = [single]($S*0.05)
  $core = New-Object Drawing.SolidBrush([Drawing.Color]::FromArgb(235,255,255,255))
  $g.FillEllipse($core, [single]($cx-$cr), [single]($cy-$cr), [single](2*$cr), [single](2*$cr))

  # sparkle secundario (solo en tamanos grandes)
  if ($S -ge 48) {
    $sx = [single]($cx + $S*0.21); $sy = [single]($cy - $S*0.21)
    $s2 = Add-Star $sx $sy ([single]($S*0.085)) ([single]($S*0.022))
    $b2 = New-Object Drawing.SolidBrush([Drawing.Color]::FromArgb(230,0xBF,0xD2,0xFB))
    $g.FillEllipse($b2, [single]($sx-$S*0.012), [single]($sy-$S*0.012), [single]($S*0.024), [single]($S*0.024))
    $g.FillPath((New-Object Drawing.SolidBrush([Drawing.Color]::FromArgb(220,0xBF,0xD2,0xFB))), $s2)
    $s2.Dispose()
  }

  $lg.Dispose(); $pen.Dispose(); $sq.Dispose(); $pgb.Dispose(); $glow.Dispose()
  $sp.Dispose(); $star.Dispose(); $core.Dispose()
}

$sizes = @(16,24,32,48,64,128,256)
$pngs  = @()
foreach ($s in $sizes) {
  $bmp = New-Object Drawing.Bitmap($s, $s, [Drawing.Imaging.PixelFormat]::Format32bppArgb)
  $g   = [Drawing.Graphics]::FromImage($bmp)
  Draw-Lux $g $s
  $g.Dispose()
  $ms = New-Object IO.MemoryStream
  $bmp.Save($ms, [Drawing.Imaging.ImageFormat]::Png)
  $pngs += , $ms.ToArray()    # coma unaria: no aplanar el byte[]
  $bmp.Dispose(); $ms.Dispose()
}

$icoPath = Join-Path $PSScriptRoot 'lux.ico'
$fs = [IO.File]::Create($icoPath)
$bw = New-Object IO.BinaryWriter($fs)
$bw.Write([UInt16]0); $bw.Write([UInt16]1); $bw.Write([UInt16]$sizes.Count)
$offset = 6 + 16 * $sizes.Count
for ($i = 0; $i -lt $sizes.Count; $i++) {
  $s = $sizes[$i]; $len = $pngs[$i].Length
  $dim = if ($s -ge 256) { 0 } else { $s }
  $bw.Write([Byte]$dim); $bw.Write([Byte]$dim)
  $bw.Write([Byte]0);    $bw.Write([Byte]0)
  $bw.Write([UInt16]1);  $bw.Write([UInt16]32)
  $bw.Write([UInt32]$len); $bw.Write([UInt32]$offset)
  $offset += $len
}
foreach ($p in $pngs) { $bw.Write($p) }
$bw.Flush(); $bw.Dispose(); $fs.Dispose()

Write-Host ("lux.ico generado: {0:N1} KB ({1} resoluciones)" -f ((Get-Item $icoPath).Length/1KB), $sizes.Count) -ForegroundColor Green
