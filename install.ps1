$ErrorActionPreference = "Stop"
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

Write-Host "======================================" -ForegroundColor Cyan
Write-Host "       Installing LIME Hypervisor     " -ForegroundColor Cyan
Write-Host "======================================" -ForegroundColor Cyan

# Setup target directory
$InstallDir = "$env:USERPROFILE\.lime\bin"
if (!(Test-Path -Path $InstallDir)) {
    New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null
}

$ExePath = Join-Path $InstallDir "lime.exe"
$DownloadUrl = "https://github.com/ak495867/Lime/releases/latest/download/lime-windows-amd64.exe"

Write-Host "==> Fetching latest binary from GitHub..." -ForegroundColor Blue
Invoke-WebRequest -Uri $DownloadUrl -OutFile $ExePath

# Add to PATH if not already there
$UserPath = [Environment]::GetEnvironmentVariable("PATH", "User")
if ($UserPath -notmatch [regex]::Escape($InstallDir)) {
    $NewPath = if ($UserPath) { "$UserPath;$InstallDir" } else { $InstallDir }
    [Environment]::SetEnvironmentVariable("PATH", $NewPath, "User")
    $env:PATH = "$env:PATH;$InstallDir"
    Write-Host "==> Added $InstallDir to user PATH." -ForegroundColor Yellow
}

Write-Host "==> LIME installed successfully! ??" -ForegroundColor Green
Write-Host "Restart your terminal and run 'lime --help' to get started." -ForegroundColor Cyan
