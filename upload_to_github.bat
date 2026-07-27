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
rem
rem Written with goto labels rather than parenthesised if-blocks on purpose:
rem cmd mis-parses escaped parentheses inside a block, which silently breaks the
rem block open and runs the following comment text as commands.

echo.
echo  Luna Capture - upload to GitHub
echo  ==============================
echo.

rem ---------------------------------------------------------------- checks ---
where git.exe >nul 2>nul
if errorlevel 1 goto :no_git

git rev-parse --is-inside-work-tree >nul 2>nul
if errorlevel 1 goto :no_repo

git remote get-url origin >nul 2>nul
if errorlevel 1 goto :no_remote

for /f "delims=" %%r in ('git remote get-url origin') do set "REMOTE=%%r"
for /f "delims=" %%b in ('git rev-parse --abbrev-ref HEAD') do set "BRANCH=%%b"
echo  Repository : %REMOTE%
echo  Branch     : %BRANCH%
echo.

rem --------------------------------------------------------- what changed ---
git status --short
echo.

rem Anything added, edited or deleted since the last commit?
set "HAS_CHANGES="
for /f "delims=" %%s in ('git status --porcelain') do set "HAS_CHANGES=1"

rem Commits made earlier that never reached GitHub.
set "AHEAD=0"
git rev-parse --abbrev-ref --symbolic-full-name @{u} >nul 2>nul
if errorlevel 1 goto :counted
for /f "delims=" %%a in ('git rev-list --count @{u}..HEAD') do set "AHEAD=%%a"

:counted
if defined HAS_CHANGES goto :message
if not "%AHEAD%"=="0" goto :push_only
echo  Nothing to upload - GitHub already matches this folder.
goto :done

:push_only
echo  No new file changes, but %AHEAD% earlier commit/commits still need pushing.
goto :push

rem ------------------------------------------------------ commit message ---
:message
set "MESSAGE=%~1"
if not "%MESSAGE%"=="" goto :commit

set "MESSAGE="
set /p "MESSAGE=  Describe the change, or press Enter for a dated default: "
if not "%MESSAGE%"=="" goto :commit

for /f "delims=" %%t in ('powershell.exe -NoProfile -Command "Get-Date -Format \"yyyy-MM-dd HH:mm\""') do set "STAMP=%%t"
set "MESSAGE=Update %STAMP%"

:commit
echo.
echo  Committing: %MESSAGE%
git add -A
if errorlevel 1 goto :stage_failed
git commit -m "%MESSAGE%"
if errorlevel 1 goto :commit_failed

rem ------------------------------------------------------------- pushing ---
:push
echo.
echo  Pushing to GitHub...
git rev-parse --abbrev-ref --symbolic-full-name @{u} >nul 2>nul
if errorlevel 1 goto :first_push
git push
if errorlevel 1 goto :rebase_retry
goto :ok

:first_push
git push -u origin "%BRANCH%"
if errorlevel 1 goto :rebase_retry
goto :ok

rem A rejected push almost always means the remote moved on - usually an edit
rem made through the GitHub website. Replay our commits on top and retry once.
:rebase_retry
echo.
echo  Push rejected - GitHub has commits this folder does not. Rebasing...
git pull --rebase
if errorlevel 1 goto :conflict
git push
if errorlevel 1 goto :push_failed
goto :ok

rem ------------------------------------------------------------- outcomes ---
:ok
echo.
echo  Done. Everything in this folder is now on GitHub.
echo  %REMOTE%
goto :done

:no_git
echo  [ERROR] Git is not installed, or not on PATH.
echo          Install "Git for Windows" from https://git-scm.com/download/win
goto :fail

:no_repo
echo  [ERROR] This folder is not a git repository.
echo          Expected a .git folder in: %CD%
goto :fail

:no_remote
echo  [ERROR] No "origin" remote is configured, so there is nowhere to push.
echo          Fix with:
echo              git remote add origin https://github.com/LouSkull/LunaCapture.git
goto :fail

:stage_failed
echo  [ERROR] Could not stage the changes.
goto :fail

:commit_failed
echo  [ERROR] Commit failed. See the message above.
goto :fail

:conflict
echo.
echo  [ERROR] The rebase hit a conflict, so nothing was uploaded.
echo          Your commit is safe locally. Resolve it with:
echo              git status               - see the conflicting files
echo              git rebase --continue    - after fixing them
echo              git rebase --abort       - to back out entirely
goto :fail

:push_failed
echo.
echo  [ERROR] Push still failed. Your commit is safe locally - nothing is lost.
goto :fail

:fail
echo.
pause
exit /b 1

:done
echo.
pause
exit /b 0
