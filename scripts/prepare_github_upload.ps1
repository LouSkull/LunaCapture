# Stages a clean, minimal copy of the project for uploading to GitHub by hand.
#
# GitHub's web uploader accepts at most 100 files per drag-and-drop, so this
# reports the exact file count and, when needed, tells you how to split the
# upload by folder. Nothing here touches the working tree.

[CmdletBinding()]
param(
    [string]$OutputName = 'github-source'
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$destination = Join-Path $root $OutputName

Write-Host ''
Write-Host '  Luna Capture - GitHub source staging' -ForegroundColor Cyan
Write-Host '  ====================================='
Write-Host ''

# --- collect the file list -------------------------------------------------
# git already knows exactly what belongs in the repository: everything tracked,
# plus everything untracked that .gitignore does not exclude. That is a far more
# reliable definition than any hand-written list, which drifts the moment a file
# is added.
$useGit = $false
try {
    $null = & git -C $root rev-parse --is-inside-work-tree 2>$null
    $useGit = $LASTEXITCODE -eq 0
} catch {
    $useGit = $false
}

$relativePaths = @()
if ($useGit) {
    $relativePaths = & git -C $root ls-files --cached --others --exclude-standard
    Write-Host '  Source list: git (tracked + unignored files)'
} else {
    # Fallback for a copy of the sources without a .git folder.
    Write-Host '  Source list: folder scan (git not available)' -ForegroundColor Yellow
    $folders = @('src', 'include', 'installer', 'tests', 'assets', 'cmake', 'scripts', 'plugins', 'website', 'docs')
    foreach ($folder in $folders) {
        $path = Join-Path $root $folder
        if (-not (Test-Path $path)) { continue }
        Get-ChildItem $path -Recurse -File | ForEach-Object {
            $relativePaths += $_.FullName.Substring($root.Length + 1).Replace('\', '/')
        }
    }
    foreach ($name in @('CMakeLists.txt', 'VERSION', 'README.md', 'license.txt', '.gitignore',
                        'resources.rc', 'build_release.bat', 'install_and_run.bat', 'diagnose.bat')) {
        if (Test-Path (Join-Path $root $name)) { $relativePaths += $name }
    }
}

# Drop anything git still has in its index but which no longer exists on disk
# (deleted-but-not-committed files), plus our own output folder.
$missing = @()
$staged = @()
foreach ($relative in $relativePaths) {
    if ($relative -like "$OutputName/*") { continue }
    $source = Join-Path $root ($relative -replace '/', '\')
    if (Test-Path $source -PathType Leaf) { $staged += $relative } else { $missing += $relative }
}

if ($staged.Count -eq 0) {
    Write-Host '  [ERROR] No files were found to stage.' -ForegroundColor Red
    exit 1
}

# --- copy ------------------------------------------------------------------
if (Test-Path $destination) { Remove-Item $destination -Recurse -Force }
New-Item -ItemType Directory -Path $destination -Force | Out-Null

foreach ($relative in $staged) {
    $source = Join-Path $root ($relative -replace '/', '\')
    $target = Join-Path $destination ($relative -replace '/', '\')
    $targetDir = Split-Path -Parent $target
    if (-not (Test-Path $targetDir)) { New-Item -ItemType Directory -Path $targetDir -Force | Out-Null }
    Copy-Item $source $target -Force
}

# --- report ----------------------------------------------------------------
Write-Host ''
Write-Host "  Staged $($staged.Count) file(s) into: $OutputName\"
if ($missing.Count -gt 0) {
    Write-Host ''
    Write-Host "  Skipped $($missing.Count) file(s) git still tracks but that no longer exist on disk:" -ForegroundColor Yellow
    $missing | Select-Object -First 6 | ForEach-Object { Write-Host "    $_" -ForegroundColor DarkGray }
    if ($missing.Count -gt 6) { Write-Host "    ... and $($missing.Count - 6) more" -ForegroundColor DarkGray }
    Write-Host '    Run "git rm --cached <file>" (or commit the deletions) to clean the index.' -ForegroundColor DarkGray
}

Write-Host ''
Write-Host '  Files per top-level entry:'
$groups = $staged | ForEach-Object {
    if ($_ -match '/') { ($_ -split '/')[0] } else { '(root files)' }
} | Group-Object | Sort-Object Count -Descending

foreach ($group in $groups) {
    $flag = if ($group.Count -gt 100) { '  <-- too big for one drag' } else { '' }
    $colour = if ($group.Count -gt 100) { 'Yellow' } else { 'Gray' }
    Write-Host ("    {0,-16} {1,3}{2}" -f $group.Name, $group.Count, $flag) -ForegroundColor $colour
}

Write-Host ''
if ($staged.Count -le 100) {
    Write-Host "  This fits in a single upload ($($staged.Count) of 100 allowed)." -ForegroundColor Green
    Write-Host '  On the GitHub repo page: Add file > Upload files, then drag everything'
    Write-Host "  inside $OutputName\ (select all, not the folder itself)."
} else {
    Write-Host "  Too many for one upload ($($staged.Count) files, limit is 100)." -ForegroundColor Yellow
    Write-Host '  Upload one top-level folder at a time using the counts above; each drag'
    Write-Host '  keeps its folder structure, so the layout comes out correct either way.'
}

Write-Host ''
Write-Host '  Note: the 100-file limit only applies to the web uploader.' -ForegroundColor DarkGray
Write-Host '        "git add . && git commit && git push" has no such limit and is' -ForegroundColor DarkGray
Write-Host '        the better option if you have git set up.' -ForegroundColor DarkGray
Write-Host ''
