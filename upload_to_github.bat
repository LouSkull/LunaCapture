@echo off
rem Sends this whole folder to GitHub: stage, commit, push. Double-click it, or
rem pass the commit message: upload_to_github.bat "fixed the region editor"
rem
rem Deliberately a one-line launcher. The real work is in the PowerShell script
rem because this run rewrites the repository it lives in, and cmd.exe reads a
rem batch file by byte offset as it goes - so a .bat that commits itself resumes
rem mid-line on shifted bytes and starts executing garbage.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\upload_to_github.ps1" %* & exit /b
