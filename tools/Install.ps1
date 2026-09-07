param([string]$SourceDirectory = $PSScriptRoot)
$ErrorActionPreference = 'Stop'

$source = Join-Path $SourceDirectory 'PaperShade.exe'
if (-not (Test-Path -LiteralPath $source -PathType Leaf)) {
    throw 'Extract the portable package first, then run Install.ps1 next to PaperShade.exe.'
}
$reader = [System.IO.BinaryReader]::new([System.IO.File]::OpenRead($source))
try {
    $reader.BaseStream.Position = 0x3c
    $peOffset = $reader.ReadInt32()
    $reader.BaseStream.Position = $peOffset + 4
    $machine = $reader.ReadUInt16()
} finally {
    $reader.Dispose()
}
$architecture = [System.Runtime.InteropServices.RuntimeInformation]::OSArchitecture.ToString()
$expected = switch ($architecture) {
    'Arm64' { 0xaa64 }
    'X64' { 0x8664 }
    default { throw "Unsupported native Windows architecture: $architecture" }
}
if ($machine -ne $expected) {
    throw "Wrong package for this PC. Download the $architecture package."
}

$destination = Join-Path $env:LOCALAPPDATA 'Programs\PaperShade'
[System.IO.Directory]::CreateDirectory($destination) | Out-Null
Copy-Item -LiteralPath $source -Destination (Join-Path $destination 'PaperShade.exe') -Force
Copy-Item -LiteralPath (Join-Path $SourceDirectory 'README.md') -Destination $destination -Force
$programs = [Environment]::GetFolderPath('Programs')
$shell = New-Object -ComObject WScript.Shell
$shortcut = $shell.CreateShortcut((Join-Path $programs 'PaperShade.lnk'))
$shortcut.TargetPath = Join-Path $destination 'PaperShade.exe'
$shortcut.WorkingDirectory = $destination
$shortcut.Description = 'Native grayscale, e-ink, and PS1 desktop filters'
$shortcut.IconLocation = "$destination\PaperShade.exe,0"
$shortcut.Save()

Write-Output "Installed PaperShade ($architecture) to $destination"
Write-Output 'Open PaperShade from Start. Startup with Windows is off unless you enable it in the tray menu.'
