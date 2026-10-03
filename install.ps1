$ErrorActionPreference = "Stop"

Write-Host "=> Installing LIME..." -ForegroundColor Cyan

# Setup target directory
$InstallDir = "$env:USERPROFILE\.lime\bin"
if (!(Test-Path -Path $InstallDir)) {
    New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null
}

$ExePath = Join-Path $InstallDir "lime.exe"
$DownloadUrl = "https://github.com/ak495867/Lime/releases/latest/download/lime-windows-amd64.exe"

Write-Host "=> Downloading LIME from $DownloadUrl"
Invoke-WebRequest -Uri $DownloadUrl -OutFile $ExePath

# Add to PATH if not already there
$UserPath = [Environment]::GetEnvironmentVariable("PATH", "User")
if ($UserPath -notmatch [regex]::Escape($InstallDir)) {
    $NewPath = "$UserPath;$InstallDir"
    [Environment]::SetEnvironmentVariable("PATH", $NewPath, "User")
    $env:PATH = "$env:PATH;$InstallDir"
    Write-Host "=> Added $InstallDir to user PATH." -ForegroundColor Yellow
}

Write-Host "=> LIME installed successfully! You can now run 'lime' from any command prompt." -ForegroundColor Green
