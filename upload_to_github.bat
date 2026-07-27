@echo off
setlocal EnableExtensions
cd /d "%~dp0"

rem Sends everything in this folder to GitHub in one step: stage, commit, push.
rem
rem Replaces the old "stage a github-source folder and drag it into the website"
rem flow, which only existed because GitHub's web uploader takes 100 files per
rem drag. With a real remote configured none of that is needed - git sends only
rem what actually changed.
rem
rem Usage:   upload_to_github.bat
rem          upload_to_github.bat "fixed the region editor"

echo.
echo  Luna Capture - upload to GitHub
echo  ==============================
echo.

rem ---------------------------------------------------------------- checks ---
where git.exe >nul 2>nul
if errorlevel 1 (
    echo  [ERROR] Git is not installed, or not on PATH.
    echo          Install "Git for Windows" from https://git-scm.com/download/win
    goto :fail
)

git rev-parse --is-inside-work-tree >nul 2>nul
if errorlevel 1 (
    echo  [ERROR] This folder is not a git repository.
    echo          Expected a .git folder in: %CD%
    goto :fail
)

git remote get-url origin >nul 2>nul
if errorlevel 1 (
    echo  [ERROR] No "origin" remote is configured, so there is nowhere to push.
    echo          Fix with:
    echo              git remote add origin https://github.com/LouSkull/LunaCapture.git
    goto :fail
)

for /f "delims=" %%r in ('git remote get-url origin') do set "REMOTE=%%r"
for /f "delims=" %%b in ('git rev-parse --abbrev-ref HEAD') do set "BRANCH=%%b"
echo  Repository : %REMOTE%
echo  Branch     : %BRANCH%
echo.

rem --------------------------------------------------------- what changed ---
git status --short
echo.

rem Is there anything staged/unstaged at all?
set "HAS_CHANGES="
for /f "delims=" %%s in ('git status --porcelain') do set "HAS_CHANGES=1"

rem Are there commits made earlier that never reached GitHub?
set "AHEAD=0"
git rev-parse --abbrev-ref --symbolic-full-name @{u} >nul 2>nul
if not errorlevel 1 (
    for /f "delims=" %%a in ('git rev-list --count @{u}..HEAD') do set "AHEAD=%%a"
)

if not defined HAS_CHANGES if "%AHEAD%"=="0" (
    echo  Nothing to upload - GitHub already matches this folder.
    goto :done
)

if not defined HAS_CHANGES (
    echo  No new file changes, but %AHEAD% commit^(s^) still need pushing.
    goto :push
)

rem ------------------------------------------------------ commit message ---
set "MESSAGE=%~1"
if not "%MESSAGE%"=="" goto :commit

set "MESSAGE="
set /p "MESSAGE=  Describe the change (Enter for a dated default): "
if not "%MESSAGE%"=="" goto :commit

for /f "delims=" %%t in ('powershell.exe -NoProfile -Command "Get-Date -Format \"yyyy-MM-dd HH:mm\""') do set "STAMP=%%t"
set "MESSAGE=Update %STAMP%"

:commit
echo.
echo  Committing: %MESSAGE%
git add -A
if errorlevel 1 (
    echo  [ERROR] Could not stage the changes.
    goto :fail
)
git commit -m "%MESSAGE%"
if errorlevel 1 (
    echo  [ERROR] Commit failed. See the message above.
    goto :fail
)

rem ------------------------------------------------------------- pushing ---
:push
echo.
echo  Pushing to GitHub...
git rev-parse --abbrev-ref --symbolic-full-name @{u} >nul 2>nul
if errorlevel 1 (
    git push -u origin "%BRANCH%"
) else (
    git push
)
if not errorlevel 1 goto :ok

rem A rejected push almost always means the remote moved on - typically an edit
rem made through the GitHub website. Replay our commits on top and retry once.
echo.
echo  Push rejected - GitHub has commits this folder does not. Rebasing...
git pull --rebase
if errorlevel 1 (
    echo.
    echo  [ERROR] The rebase hit a conflict, so nothing was uploaded.
    echo          Your commit is safe locally. Resolve it with:
    echo              git status              ^(see the conflicting files^)
    echo              git rebase --continue   ^(after fixing them^)
    echo              git rebase --abort      ^(to back out entirely^)
    goto :fail
)
git push
if errorlevel 1 (
    echo.
    echo  [ERROR] Push still failed. Your commit is safe locally - nothing is lost.
    goto :fail
)

:ok
echo.
echo  Done. Everything in this folder is now on GitHub.
echo  %REMOTE%
goto :done

:fail
echo.
pause
exit /b 1

:done
echo.
pause
exit /b 0
