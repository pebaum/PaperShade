param(
    [ValidateSet('arm64', 'x64')][string[]]$Architectures = @('arm64', 'x64')
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$version = (Get-Content -LiteralPath (Join-Path $root 'VERSION') -Raw).Trim()
if ($version -notmatch '^\d+\.\d+\.\d+$') { throw 'VERSION must contain a semantic major.minor.patch version.' }
$distribution = Join-Path $root 'dist'
[System.IO.Directory]::CreateDirectory($distribution) | Out-Null

foreach ($architecture in $Architectures) {
    $build = Join-Path $root "build\$architecture"
    $package = Join-Path $distribution "PaperShade-$architecture"
    if (-not (Test-Path -LiteralPath (Join-Path $build 'Release\PaperShade.exe'))) {
        throw "Build the $architecture Release target before packaging."
    }
    if ((Get-Item -LiteralPath (Join-Path $build 'Release\PaperShade.exe')).VersionInfo.FileVersion -ne $version) {
        throw "The $architecture executable is not version $version. Rebuild before packaging."
    }
    & cmake --install $build --config Release --prefix $package
    if ($LASTEXITCODE -ne 0) { throw "Packaging $architecture failed." }
    Copy-Item -LiteralPath (Join-Path $package 'PaperShade.exe') `
        -Destination (Join-Path $distribution "PaperShade-$version-windows-$architecture.exe") -Force
    Compress-Archive -LiteralPath @(
        (Join-Path $package 'PaperShade.exe'),
        (Join-Path $package 'README.md'),
        (Join-Path $package 'Install.ps1')
    ) -DestinationPath (Join-Path $distribution "PaperShade-$version-windows-$architecture.zip") -Force
}

$sourcePaths = @(
    'src', 'core', 'tests', 'tools', 'docs', 'macOS', 'Package.swift',
    'CMakeLists.txt', 'CMakePresets.json', 'README.md', 'VERSION',
    '.github', '.gitattributes', '.gitignore'
) | ForEach-Object { Join-Path $root $_ }
Compress-Archive -LiteralPath $sourcePaths `
    -DestinationPath (Join-Path $distribution "PaperShade-$version-source.zip") -Force
Get-ChildItem -LiteralPath $distribution -Filter '*.zip' -File |
    Get-FileHash -Algorithm SHA256 | Format-Table -AutoSize
