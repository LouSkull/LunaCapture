$ErrorActionPreference = 'SilentlyContinue'

# Stop any running instance (current LunaCapture and the previous LunaShot name).
Get-Process -Name 'LunaCapture' -ErrorAction SilentlyContinue | Stop-Process -Force
Get-Process -Name 'LunaShot' -ErrorAction SilentlyContinue | Stop-Process -Force

Get-CimInstance Win32_Process | Where-Object {
    $_.Name -in @('electron.exe', 'python.exe', 'pythonw.exe') -and
    $_.CommandLine -and
    $_.CommandLine -match '(?i)[\\/]Luna(?:Shot|Capture)(?:_[^\\/]+)?[\\/]'
} | ForEach-Object {
    Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue
}
