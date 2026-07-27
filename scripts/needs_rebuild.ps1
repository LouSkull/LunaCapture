$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $root 'dist\LunaCapture.exe'
if (-not (Test-Path -LiteralPath $exe)) { exit 1 }
$builtAt = (Get-Item -LiteralPath $exe).LastWriteTimeUtc
$paths = @(
    (Join-Path $root 'src'),
    (Join-Path $root 'include'),
    (Join-Path $root 'tests'),
    (Join-Path $root 'assets'),
    (Join-Path $root 'CMakeLists.txt'),
    (Join-Path $root 'resources.rc'),
    (Join-Path $root 'build_release.bat')
)
foreach ($path in $paths) {
    if (-not (Test-Path -LiteralPath $path)) { continue }
    $item = Get-Item -LiteralPath $path
    if (-not $item.PSIsContainer) {
        if ($item.LastWriteTimeUtc -gt $builtAt) { exit 1 }
        continue
    }
    $newer = Get-ChildItem -LiteralPath $path -Recurse -File | Where-Object { $_.LastWriteTimeUtc -gt $builtAt } | Select-Object -First 1
    if ($null -ne $newer) { exit 1 }
}
exit 0
