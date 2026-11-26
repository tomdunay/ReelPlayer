param([string]$OutDir)
Add-Type -AssemblyName System.Drawing
New-Item -ItemType Directory -Force $OutDir | Out-Null

function New-Logo([int]$size) {
    $bmp = New-Object System.Drawing.Bitmap $size, $size
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = 'AntiAlias'
    $g.Clear([System.Drawing.Color]::Transparent)
    $s = [float]$size

    # Rounded square with diagonal gradient (teal -> indigo)
    $r = $s * 0.22
    $path = New-Object System.Drawing.Drawing2D.GraphicsPath
    $path.AddArc(0, 0, $r*2, $r*2, 180, 90)
    $path.AddArc($s-$r*2-1, 0, $r*2, $r*2, 270, 90)
    $path.AddArc($s-$r*2-1, $s-$r*2-1, $r*2, $r*2, 0, 90)
    $path.AddArc(0, $s-$r*2-1, $r*2, $r*2, 90, 90)
    $path.CloseFigure()
    $rect = New-Object System.Drawing.RectangleF 0, 0, $s, $s
    $brush = New-Object System.Drawing.Drawing2D.LinearGradientBrush $rect, ([System.Drawing.Color]::FromArgb(255, 20, 184, 166)), ([System.Drawing.Color]::FromArgb(255, 79, 70, 229)), 45.0
    $g.FillPath($brush, $path)

    # Film reel: white disc with 5 holes
    $white = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::White)
    $cx = $s / 2; $cy = $s / 2; $R = $s * 0.34
    $reel = New-Object System.Drawing.Drawing2D.GraphicsPath
    $reel.AddEllipse($cx - $R, $cy - $R, $R * 2, $R * 2)
    $hr = $R * 0.2; $hd = $R * 0.62
    for ($i = 0; $i -lt 5; $i++) {
        $a = (-90 + $i * 72) * [Math]::PI / 180
        $hx = $cx + [Math]::Cos($a) * $hd; $hy = $cy + [Math]::Sin($a) * $hd
        $reel.AddEllipse([float]($hx - $hr), [float]($hy - $hr), [float]($hr * 2), [float]($hr * 2))
    }
    $reel.FillMode = 'Alternate'
    $g.FillPath($white, $reel)

    # Play triangle in the centre, in the gradient's indigo
    $tri = $R * 0.36
    $pts = [System.Drawing.PointF[]]@(
        (New-Object System.Drawing.PointF ([float]($cx - $tri * 0.6)), ([float]($cy - $tri))),
        (New-Object System.Drawing.PointF ([float]($cx - $tri * 0.6)), ([float]($cy + $tri))),
        (New-Object System.Drawing.PointF ([float]($cx + $tri)), ([float]$cy))
    )
    $g.FillPolygon((New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 67, 56, 202))), $pts)

    $g.Dispose()
    return $bmp
}

$sizes = 16, 24, 32, 48, 64, 128, 256
$pngs = @()
foreach ($sz in $sizes) {
    $bmp = New-Logo $sz
    $ms = New-Object System.IO.MemoryStream
    $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    $pngs += ,($ms.ToArray())
    if ($sz -eq 256) { $bmp.Save((Join-Path $OutDir 'logo.png'), [System.Drawing.Imaging.ImageFormat]::Png) }
    $bmp.Dispose()
}

# Pack PNG-compressed ICO
$out = New-Object System.IO.MemoryStream
$w = New-Object System.IO.BinaryWriter $out
$w.Write([UInt16]0); $w.Write([UInt16]1); $w.Write([UInt16]$sizes.Count)
$offset = 6 + 16 * $sizes.Count
for ($i = 0; $i -lt $sizes.Count; $i++) {
    $sz = $sizes[$i]; $len = $pngs[$i].Length
    $w.Write([byte]($(if ($sz -ge 256) {0} else {$sz})))
    $w.Write([byte]($(if ($sz -ge 256) {0} else {$sz})))
    $w.Write([byte]0); $w.Write([byte]0)
    $w.Write([UInt16]1); $w.Write([UInt16]32)
    $w.Write([UInt32]$len); $w.Write([UInt32]$offset)
    $offset += $len
}
foreach ($p in $pngs) { $w.Write($p) }
$w.Flush()
[System.IO.File]::WriteAllBytes((Join-Path $OutDir 'app.ico'), $out.ToArray())
"ok"
