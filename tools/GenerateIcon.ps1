param([Parameter(Mandatory = $true)][string]$OutputPath)
$ErrorActionPreference = 'Stop'

$sizes = @(16, 32, 48, 256)
$images = @()
foreach ($size in $sizes) {
    $stream = [System.IO.MemoryStream]::new()
    $writer = [System.IO.BinaryWriter]::new($stream)
    $maskStride = [int]([Math]::Ceiling($size / 32.0) * 4)
    $writer.Write([uint32]40)
    $writer.Write([int32]$size)
    $writer.Write([int32]($size * 2))
    $writer.Write([uint16]1)
    $writer.Write([uint16]32)
    $writer.Write([uint32]0)
    $writer.Write([uint32]($size * $size * 4))
    1..4 | ForEach-Object { $writer.Write([uint32]0) }
    for ($y = $size - 1; $y -ge 0; $y--) {
        for ($x = 0; $x -lt $size; $x++) {
            $u = [int][Math]::Floor($x * 32.0 / $size)
            $v = [int][Math]::Floor($y * 32.0 / $size)
            $inside = $u -ge 3 -and $u -lt 29 -and $v -ge 2 -and $v -lt 30
            $edge = $u -eq 3 -or $u -eq 28 -or $v -eq 2 -or $v -eq 29
            $ink = (([int][Math]::Floor($u / 3.0) + [int][Math]::Floor($v / 3.0)) % 2 -eq 0) -and $u + $v -gt 28
            $shade = if ($edge -or $ink) { [byte]37 } else { [byte]237 }
            $alpha = if ($inside) { [byte]255 } else { [byte]0 }
            $writer.Write($shade); $writer.Write($shade); $writer.Write($shade); $writer.Write($alpha)
        }
    }
    $writer.Write([byte[]]::new($maskStride * $size))
    $images += ,$stream.ToArray()
    $writer.Dispose()
}

[System.IO.Directory]::CreateDirectory([System.IO.Path]::GetDirectoryName($OutputPath)) | Out-Null
$file = [System.IO.File]::Create($OutputPath)
$writer = [System.IO.BinaryWriter]::new($file)
try {
    $writer.Write([uint16]0); $writer.Write([uint16]1); $writer.Write([uint16]$sizes.Count)
    $offset = 6 + 16 * $sizes.Count
    for ($i = 0; $i -lt $sizes.Count; $i++) {
        $dimension = if ($sizes[$i] -eq 256) { [byte]0 } else { [byte]$sizes[$i] }
        $writer.Write($dimension); $writer.Write($dimension)
        $writer.Write([byte]0); $writer.Write([byte]0)
        $writer.Write([uint16]1); $writer.Write([uint16]32)
        $writer.Write([uint32]$images[$i].Length); $writer.Write([uint32]$offset)
        $offset += $images[$i].Length
    }
    foreach ($image in $images) { $writer.Write([byte[]]$image) }
} finally {
    $writer.Dispose()
}
