#Requires -Version 7
[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$brand = Join-Path $repo 'res\tmux-workspace'
$output = Join-Path $brand 'images'
New-Item -ItemType Directory -Path $output -Force | Out-Null
$source = [Drawing.Image]::FromFile((Join-Path $brand 'app-icon.png'))
function Write-IconPng([int]$Size, [string]$Path, [int]$ArtworkSize = $Size) {
    $bitmap = [Drawing.Bitmap]::new($Size, $Size, [Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $graphics = [Drawing.Graphics]::FromImage($bitmap)
    try {
        $graphics.Clear([Drawing.Color]::Transparent)
        $graphics.CompositingQuality = [Drawing.Drawing2D.CompositingQuality]::HighQuality
        $graphics.InterpolationMode = [Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
        $graphics.PixelOffsetMode = [Drawing.Drawing2D.PixelOffsetMode]::HighQuality
        $inset = [int][Math]::Floor(($Size - $ArtworkSize) / 2)
        $graphics.DrawImage($source, [Drawing.Rectangle]::new($inset, $inset, $ArtworkSize, $ArtworkSize))
        $bitmap.Save($Path, [Drawing.Imaging.ImageFormat]::Png)
    }
    finally { $graphics.Dispose(); $bitmap.Dispose() }
}
try {
    foreach ($asset in @(@{Name='Square44x44Logo';Size=44;Art=44}, @{Name='Square150x150Logo';Size=150;Art=72}, @{Name='StoreLogo';Size=50;Art=50})) {
        Write-IconPng $asset.Size (Join-Path $output ($asset.Name + '.png')) $asset.Art
        foreach ($scale in 100,125,150,200,400) {
            Write-IconPng ([int][Math]::Ceiling($asset.Size * $scale / 100)) (Join-Path $output ($asset.Name + ".scale-$scale.png")) ([int][Math]::Ceiling($asset.Art * $scale / 100))
        }
    }
    $sizes = 16,20,24,30,32,36,40,48,60,64,72,80,96,128,256
    foreach ($size in $sizes) {
        foreach ($suffix in '', '_altform-unplated', '_altform-lightunplated') {
            Write-IconPng $size (Join-Path $output "Square44x44Logo.targetsize-$size$suffix.png")
        }
    }
    $icoSizes = 16,24,32,48,64,128,256
    $entries = [Collections.Generic.List[byte[]]]::new()
    foreach ($size in $icoSizes) { $entries.Add([IO.File]::ReadAllBytes((Join-Path $output "Square44x44Logo.targetsize-$size.png"))) }
    $stream = [IO.File]::Create((Join-Path $brand 'app-icon.ico'))
    $writer = [IO.BinaryWriter]::new($stream)
    try {
        $writer.Write([uint16]0); $writer.Write([uint16]1); $writer.Write([uint16]$icoSizes.Count)
        $offset = 6 + 16 * $icoSizes.Count
        for ($index = 0; $index -lt $icoSizes.Count; $index++) {
            $sizeByte = if ($icoSizes[$index] -eq 256) { 0 } else { $icoSizes[$index] }
            $writer.Write([byte]$sizeByte); $writer.Write([byte]$sizeByte)
            $writer.Write([byte]0); $writer.Write([byte]0)
            $writer.Write([uint16]1); $writer.Write([uint16]32)
            $writer.Write([uint32]$entries[$index].Length); $writer.Write([uint32]$offset)
            $offset += $entries[$index].Length
        }
        foreach ($entry in $entries) { $writer.Write([byte[]]$entry) }
    }
    finally { $writer.Dispose(); $stream.Dispose() }
}
finally { $source.Dispose() }
Write-Host 'Generated Windows package PNGs and a multi-resolution executable icon.'
