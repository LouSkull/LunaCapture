# Sends everything in the repository to GitHub in one step: stage, commit, push.
#
# The logic lives here rather than in the .bat on purpose. cmd.exe reads a batch
# file incrementally, by byte offset, while it runs - and this script commits and
# rebases the very folder it lives in, so a run would rewrite the .bat underneath
# cmd and make it resume mid-line on garbage. PowerShell parses the whole file
# before executing a single line, so rewriting it mid-run is harmless.

param([Parameter(ValueFromRemainingArguments = $true)][string[]]$MessageParts)

$ErrorActionPreference = 'Continue'

function Write-Step($text) { Write-Host "  $text" }
function Fail($lines) {
    Write-Host ''
    foreach ($line in $lines) { Write-Host "  $line" -ForegroundColor Red }
    Write-Host ''
    Read-Host '  Press Enter to close'
    exit 1
}

Set-Location (Split-Path $PSScriptRoot -Parent)

Write-Host ''
Write-Host '  Luna Capture - upload to GitHub'
Write-Host '  =============================='
Write-Host ''

# ------------------------------------------------------------------- checks ---
if (-not (Get-Command git.exe -ErrorAction SilentlyContinue)) {
    Fail @('[ERROR] Git is not installed, or not on PATH.',
           '        Install "Git for Windows" from https://git-scm.com/download/win')
}

git rev-parse --is-inside-work-tree *> $null
if ($LASTEXITCODE -ne 0) {
    Fail @('[ERROR] This folder is not a git repository.', "        Looked in: $(Get-Location)")
}

$remote = git remote get-url origin 2>$null
if ($LASTEXITCODE -ne 0 -or -not $remote) {
    Fail @('[ERROR] No "origin" remote is configured, so there is nowhere to push.',
           '        Fix with:',
           '            git remote add origin https://github.com/LouSkull/LunaCapture.git')
}

$branch = (git rev-parse --abbrev-ref HEAD).Trim()
Write-Step "Repository : $remote"
Write-Step "Branch     : $branch"
Write-Host ''

# Ask GitHub what it actually has before comparing. Without this the ahead and
# behind counts are answered from a stale cached ref, so edits made on the
# website stay invisible and the script wrongly reports "already matches".
Write-Step 'Checking GitHub...'
git fetch --quiet origin
Write-Host ''

# -------------------------------------------------------------- what changed ---
$dirty = @(git status --porcelain)
if ($dirty.Count -gt 0) { git status --short; Write-Host '' }

$ahead = 0
$behind = 0
git rev-parse --abbrev-ref --symbolic-full-name '@{u}' *> $null
$hasUpstream = ($LASTEXITCODE -eq 0)
if ($hasUpstream) {
    $ahead = [int](git rev-list --count '@{u}..HEAD')
    $behind = [int](git rev-list --count 'HEAD..@{u}')
}

# ------------------------------------------------------------------ actions ---
if ($dirty.Count -eq 0 -and $ahead -eq 0 -and $behind -eq 0) {
    Write-Step 'Nothing to upload - GitHub already matches this folder.'
    Write-Host ''
    Read-Host '  Press Enter to close'
    exit 0
}

# Nothing of ours to send, but the website has newer commits. Bring them down so
# this folder stops silently drifting behind the repository.
if ($dirty.Count -eq 0 -and $ahead -eq 0 -and $behind -gt 0) {
    Write-Step "Nothing to upload, but GitHub has $behind newer commit(s). Downloading..."
    Write-Host ''
    git pull --rebase
    if ($LASTEXITCODE -ne 0) {
        Fail @('[ERROR] The rebase hit a conflict. Resolve it with:',
               '            git status               - see the conflicting files',
               '            git rebase --continue    - after fixing them',
               '            git rebase --abort       - to back out entirely')
    }
    Write-Host ''
    Write-Step 'This folder is now up to date with GitHub.'
    Write-Host ''
    Read-Host '  Press Enter to close'
    exit 0
}

if ($dirty.Count -gt 0) {
    $message = ($MessageParts -join ' ').Trim()
    if (-not $message) { $message = (Read-Host '  Describe the change, or press Enter for a dated default').Trim() }
    if (-not $message) { $message = "Update $(Get-Date -Format 'yyyy-MM-dd HH:mm')" }

    Write-Host ''
    Write-Step "Committing: $message"
    git add -A
    if ($LASTEXITCODE -ne 0) { Fail @('[ERROR] Could not stage the changes.') }
    git commit -m $message
    if ($LASTEXITCODE -ne 0) { Fail @('[ERROR] Commit failed. See the message above.') }
} else {
    Write-Step "No new file changes, but $ahead earlier commit(s) still need pushing."
}

# ------------------------------------------------------------------ pushing ---
Write-Host ''
Write-Step 'Pushing to GitHub...'
if ($hasUpstream) { git push } else { git push -u origin $branch }

if ($LASTEXITCODE -ne 0) {
    # A rejected push almost always means the remote moved on - usually an edit
    # made through the GitHub website. Replay our commits on top and retry once.
    Write-Host ''
    Write-Step 'Push rejected - GitHub has commits this folder does not. Rebasing...'
    git pull --rebase
    if ($LASTEXITCODE -ne 0) {
        Fail @('[ERROR] The rebase hit a conflict, so nothing was uploaded.',
               '        Your commit is safe locally. Resolve it with:',
               '            git status               - see the conflicting files',
               '            git rebase --continue    - after fixing them',
               '            git rebase --abort       - to back out entirely')
    }
    git push
    if ($LASTEXITCODE -ne 0) {
        Fail @('[ERROR] Push still failed. Your commit is safe locally - nothing is lost.')
    }
}

Write-Host ''
Write-Step 'Done. Everything in this folder is now on GitHub.'
Write-Step $remote
Write-Host ''
Read-Host '  Press Enter to close'
exit 0
