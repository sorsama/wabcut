<#
.SYNOPSIS
    Draws the MSI's artwork (banner.bmp, dialog.bmp next to this script) in Wabcut's design language v3 ("Echo",
    docs/design-language.md): a black island with the echo mark (violet clips, the longest in lime) and the wordmark.

.DESCRIPTION
    WixUIBannerBmp is 493 x 58 (the top strip of the inner pages; the page title is drawn over its left part in black)
    and WixUIDialogBmp is 493 x 312 (the welcome and finish pages; their text sits right of x = 180). Both stay light
    where the installer draws text. The mark is drawn here as vectors from the same geometry as
    src/app/resources/brand/wabcut-mark.svg. Run it again after a brand change and commit the two BMPs:

        powershell -NoProfile -ExecutionPolicy Bypass -File cmake/packaging/wix/Make-InstallerArt.ps1
#>
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$here = $PSScriptRoot
$repo = Resolve-Path (Join-Path $here '..\..\..')
$fonts = Join-Path $repo 'src\app\resources\fonts'

$paper = [System.Drawing.ColorTranslator]::FromHtml('#FFFFFF')      # light page (the installer's text is black)
$stage = [System.Drawing.ColorTranslator]::FromHtml('#000000')      # the black stage
$violet = [System.Drawing.ColorTranslator]::FromHtml('#9B6CFF')
$lime = [System.Drawing.ColorTranslator]::FromHtml('#D6F54C')
$muted = [System.Drawing.ColorTranslator]::FromHtml('#B4B4BA')

$collection = New-Object System.Drawing.Text.PrivateFontCollection
$collection.AddFontFile((Join-Path $fonts 'Manrope-ExtraBold.ttf'))
$collection.AddFontFile((Join-Path $fonts 'Manrope-SemiBold.ttf'))
$extraBold = $collection.Families | Where-Object { $_.Name -like '*ExtraBold*' } | Select-Object -First 1
$semiBold = $collection.Families | Where-Object { $_.Name -like '*SemiBold*' } | Select-Object -First 1
if (-not $extraBold) { $extraBold = $collection.Families[0] }
if (-not $semiBold) { $semiBold = $collection.Families[0] }

function New-Canvas([int]$width, [int]$height) {
    $bitmap = New-Object System.Drawing.Bitmap($width, $height, [System.Drawing.Imaging.PixelFormat]::Format24bppRgb)
    $g = [System.Drawing.Graphics]::FromImage($bitmap)
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $g.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::AntiAliasGridFit
    $g.Clear($paper)
    return $bitmap, $g
}

function New-RoundedRect([double]$x, [double]$y, [double]$w, [double]$h, [double]$r) {
    $path = New-Object System.Drawing.Drawing2D.GraphicsPath
    $d = [Math]::Min(2 * $r, [Math]::Min($w, $h))
    $path.AddArc($x, $y, $d, $d, 180, 90)
    $path.AddArc($x + $w - $d, $y, $d, $d, 270, 90)
    $path.AddArc($x + $w - $d, $y + $h - $d, $d, $d, 0, 90)
    $path.AddArc($x, $y + $h - $d, $d, $d, 90, 90)
    $path.CloseFigure()
    return $path
}

# The echo mark in its 120-unit box at ($x, $y), $size px: five clips and two fainter echoes (wabcut-mark.svg).
function Draw-Mark($g, [double]$x, [double]$y, [double]$size) {
    $u = $size / 120.0
    $bars = @(@(18, 28, $false), @(36, 52, $false), @(54, 78, $true), @(72, 52, $false), @(90, 28, $false))
    foreach ($layer in @(@(6, 46), @(20, 115), @(34, 255))) {
        foreach ($bar in $bars) {
            $colour = if ($bar[2]) { $lime } else { $violet }
            $brush = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb($layer[1], $colour))
            $g.FillPath($brush, (New-RoundedRect ($x + $layer[0] * $u) ($y + $bar[0] * $u) ($bar[1] * $u) (12 * $u) (6 * $u)))
            $brush.Dispose()
        }
    }
}

# ---- dialog.bmp: a black island on the left with the mark and the wordmark ------------------------------------------
$bitmap, $g = New-Canvas 493 312
$island = New-RoundedRect -24 -24 190 360 26
$g.FillPath((New-Object System.Drawing.SolidBrush($stage)), $island)
Draw-Mark $g 22 44 112
$white = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::White)
$g.DrawString('Wabcut', (New-Object System.Drawing.Font($extraBold, 26, [System.Drawing.FontStyle]::Regular,
            [System.Drawing.GraphicsUnit]::Pixel)), $white, 24, 168)
$g.DrawString('video editor', (New-Object System.Drawing.Font($semiBold, 13, [System.Drawing.FontStyle]::Regular,
            [System.Drawing.GraphicsUnit]::Pixel)), (New-Object System.Drawing.SolidBrush($muted)), 27, 202)
$g.Dispose()
$bitmap.Save((Join-Path $here 'dialog.bmp'), [System.Drawing.Imaging.ImageFormat]::Bmp)
$bitmap.Dispose()

# ---- banner.bmp: light strip, a black pill at the right with the mark on it ---------------------------------------
$bitmap, $g = New-Canvas 493 58
$g.FillPath((New-Object System.Drawing.SolidBrush($stage)), (New-RoundedRect 421 7 64 44 22))
Draw-Mark $g 432 8 42
$g.Dispose()
$bitmap.Save((Join-Path $here 'banner.bmp'), [System.Drawing.Imaging.ImageFormat]::Bmp)
$bitmap.Dispose()
Write-Host "wrote $here\dialog.bmp and $here\banner.bmp"
