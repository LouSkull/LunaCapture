$ErrorActionPreference = 'Stop'
$projectDirectory = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$builtExecutable = Join-Path $projectDirectory 'build\bin\LunaCapture.exe'
$installedExecutable = Join-Path $projectDirectory 'dist\LunaCapture.exe'
if (!(Test-Path -LiteralPath $builtExecutable)) { throw 'Build LunaCapture before restarting it.' }

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class LunaLocalRestart {
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr FindWindow(string name, string title);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr window, out uint processId);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr window);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr window, uint message, UIntPtr wParam, IntPtr lParam);
}
'@

$mainWindow = [LunaLocalRestart]::FindWindow('LunaCaptureMainWindow', $null)
if ($mainWindow -eq [IntPtr]::Zero) {
    # A process launched from a build shell may be on another desktop.
    $runningLocal = Get-Process -Name 'LunaCapture' -ErrorAction SilentlyContinue | Where-Object {
        $_.Path -eq $installedExecutable
    } | Select-Object -First 1
    if ($runningLocal) {
        $mainWindow = $runningLocal.MainWindowHandle
        if ($mainWindow -eq [IntPtr]::Zero) { throw 'Luna Capture is running without an accessible window; close it before updating.' }
    }
}
if ($mainWindow -ne [IntPtr]::Zero) {
    [uint32]$lunaProcessId = 0
    [void][LunaLocalRestart]::GetWindowThreadProcessId($mainWindow, [ref]$lunaProcessId)
    $lunaProcess = Get-Process -Id $lunaProcessId
    # Never close an installation outside this checkout.
    $processPath = [IO.Path]::GetFullPath($lunaProcess.Path)
    $allowedPaths = @($installedExecutable, (Join-Path $projectDirectory 'dist\LunaCapture-fixed.exe'), $builtExecutable)
    if ($processPath -notin $allowedPaths) { throw "Another Luna installation is running: $processPath" }
    $editorWindow = [LunaLocalRestart]::FindWindow('LunaCaptureRegionEditor', $null)
    if ($editorWindow -ne [IntPtr]::Zero -and [LunaLocalRestart]::IsWindowVisible($editorWindow)) {
        throw 'Finish or cancel the current region capture before restarting Luna Capture.'
    }
    # Use Luna's own Exit command. Its destructor joins the capture/save worker.
    if (![LunaLocalRestart]::PostMessage($mainWindow, 0x802B, [UIntPtr]9, [IntPtr]::Zero)) {
        throw 'Could not request a graceful exit from Luna Capture.'
    }
    if (!$lunaProcess.WaitForExit(5000)) { throw 'Luna Capture is still finishing work; no process was forced to stop.' }
}

Copy-Item -LiteralPath $builtExecutable -Destination $installedExecutable -Force
if ((Get-FileHash -LiteralPath $builtExecutable).Hash -ne (Get-FileHash -LiteralPath $installedExecutable).Hash) {
    throw 'The installed binary does not match the build.'
}
Start-Process -FilePath $installedExecutable -WorkingDirectory (Split-Path -Parent $installedExecutable) -WindowStyle Hidden
Write-Output 'Updated and restarted dist\LunaCapture.exe.'
