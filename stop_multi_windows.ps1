param(
    [string]$LogDir = ""
)

$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not $LogDir) {
    $LogDir = Join-Path $root "logs"
}

if (-not (Test-Path -LiteralPath $LogDir)) {
    Write-Host "Log dir not found: $LogDir"
    New-Item -ItemType Directory -Path $LogDir | Out-Null
}

function Stop-PidFileProcess {
    param([System.IO.FileInfo]$File)

    $pidText = Get-Content -LiteralPath $File.FullName -Raw -Encoding UTF8
    $pidText = $pidText.Trim()
    if (-not $pidText) {
        Remove-Item -LiteralPath $File.FullName -Force
        return
    }

    $processId = [int]$pidText
    $proc = Get-Process -Id $processId -ErrorAction SilentlyContinue
    if ($proc) {
        Write-Host "Stopping PID $processId from $($File.Name)"
        Stop-Process -Id $processId -Force
        Wait-Process -Id $processId -Timeout 5 -ErrorAction SilentlyContinue
    } else {
        Write-Host "PID $processId is not running"
    }

    Remove-Item -LiteralPath $File.FullName -Force
}

function Stop-ProjectPythonProcesses {
    $scriptNames = @(
        "win_multi_video_server.py",
        "win_video_server.py",
        "ocr_screen_bot.py"
    )

    $rootPattern = [Regex]::Escape($root)
    $currentPid = $PID

    $procs = Get-CimInstance Win32_Process | Where-Object {
        if (-not $_.CommandLine) {
            return $false
        }
        if ($_.ProcessId -eq $currentPid) {
            return $false
        }
        if ($_.CommandLine -notmatch $rootPattern) {
            return $false
        }
        foreach ($name in $scriptNames) {
            if ($_.CommandLine -like "*$name*") {
                return $true
            }
        }
        return $false
    }

    foreach ($proc in $procs) {
        Write-Host "Stopping leftover $($proc.Name) PID $($proc.ProcessId)"
        Stop-Process -Id $proc.ProcessId -Force -ErrorAction SilentlyContinue
        Wait-Process -Id $proc.ProcessId -Timeout 5 -ErrorAction SilentlyContinue
    }
}

function Wait-LogFilesReleased {
    $logFiles = @(
        "windows_multi_dashboard.log",
        "windows_multi_dashboard.err.log",
        "windows_multi.summary.log"
    )

    foreach ($name in $logFiles) {
        $path = Join-Path $LogDir $name
        if (-not (Test-Path -LiteralPath $path)) {
            continue
        }

        for ($i = 0; $i -lt 20; $i++) {
            try {
                $stream = [System.IO.File]::Open($path, "Open", "ReadWrite", "None")
                $stream.Close()
                break
            } catch {
                Start-Sleep -Milliseconds 250
            }
        }
    }
}

$pidFiles = @()
$dashboardPid = Join-Path $LogDir "windows_multi_dashboard.pid"
if (Test-Path -LiteralPath $dashboardPid) {
    $pidFiles += Get-Item -LiteralPath $dashboardPid
}
$pidFiles += Get-ChildItem -LiteralPath $LogDir -Filter "windows_instance_*.pid" -ErrorAction SilentlyContinue

if ($pidFiles) {
    foreach ($file in $pidFiles) {
        Stop-PidFileProcess -File $file
    }
} else {
    Write-Host "No Windows receiver pid files found in $LogDir"
}

Stop-ProjectPythonProcesses
Wait-LogFilesReleased

$leftovers = Get-CimInstance Win32_Process | Where-Object {
    $_.CommandLine -and
    $_.CommandLine -match [Regex]::Escape($root) -and
    ($_.CommandLine -like "*win_multi_video_server.py*" -or
     $_.CommandLine -like "*win_video_server.py*" -or
     $_.CommandLine -like "*ocr_screen_bot.py*")
}
if ($leftovers) {
    Write-Host "Warning: some project services are still running:"
    $leftovers | Select-Object ProcessId,Name,CommandLine | Format-Table -AutoSize
} else {
    Write-Host "All Windows receiver and automation services stopped."
}
