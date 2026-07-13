param(
    [string]$RkHost = "192.168.110.86",
    [int]$Count = 2,
    [int]$HttpPort = 18091,
    [int]$BaseVideoPort = 9001,
    [int]$BaseControlPort = 9002,
    [int]$RkRecoverPort = 18110,
    [int]$PortStep = 10,
    [string]$LogDir = "",
    [switch]$NoBrowser,
    [switch]$LegacySeparate
)

$ErrorActionPreference = "Stop"
$env:PYTHONUTF8 = "1"
$env:PYTHONIOENCODING = "utf-8"

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$singleScript = Join-Path $root "start_windows.ps1"
$multiServer = Join-Path $root "win_multi_video_server.py"

if (-not $LogDir) {
    $LogDir = Join-Path $root "logs"
}

if (-not (Test-Path -LiteralPath $LogDir)) {
    New-Item -ItemType Directory -Path $LogDir | Out-Null
}

$python = Get-Command python -ErrorAction SilentlyContinue
if (-not $python) {
    $python = Get-Command py -ErrorAction SilentlyContinue
}
if (-not $python) {
    throw "Python was not found. Install Python 3 or add it to PATH."
}

$summaryLog = Join-Path $LogDir "windows_multi.summary.log"
Set-Content -LiteralPath $summaryLog -Value "" -Encoding UTF8

function Write-Summary {
    param([string]$Message)
    $line = "[{0}] {1}" -f (Get-Date -Format "yyyy-MM-dd HH:mm:ss"), $Message
    Write-Host $line
    Add-Content -LiteralPath $summaryLog -Value $line -Encoding UTF8
}

if ($Count -lt 1) {
    throw "Count must be >= 1"
}

if ($LegacySeparate) {
    if (-not (Test-Path -LiteralPath $singleScript)) {
        throw "start_windows.ps1 was not found: $singleScript"
    }

    Write-Summary "Starting $Count legacy separate Windows receiver instance(s)"
    Write-Summary "RK3568 control host: $RkHost"
    Write-Summary "Log dir: $LogDir"

    for ($i = 0; $i -lt $Count; $i++) {
        $httpPort = $HttpPort + ($i * $PortStep)
        $videoPort = $BaseVideoPort + ($i * $PortStep)
        $controlPort = $BaseControlPort + ($i * $PortStep)
        $stdoutLog = Join-Path $LogDir "windows_instance_${i}.log"
        $stderrLog = Join-Path $LogDir "windows_instance_${i}.err.log"
        $pidFile = Join-Path $LogDir "windows_instance_${i}.pid"

        Set-Content -LiteralPath $stdoutLog -Value "" -Encoding UTF8
        Set-Content -LiteralPath $stderrLog -Value "" -Encoding UTF8

        Write-Summary "Instance $i"
        Write-Summary "  Browser UI:  http://127.0.0.1:$httpPort/"
        Write-Summary "  Video input: Windows TCP port $videoPort"
        Write-Summary "  Control out: RK3568 $RkHost`:$controlPort"

        $args = @(
            "-NoExit",
            "-ExecutionPolicy", "Bypass",
            "-File", $singleScript,
            "-RkHost", $RkHost,
            "-HttpPort", "$httpPort",
            "-VideoPort", "$videoPort",
            "-ControlPort", "$controlPort"
        )

        if ($NoBrowser) {
            $args += "-NoBrowser"
        }

        $process = Start-Process powershell.exe `
            -ArgumentList $args `
            -WindowStyle Hidden `
            -RedirectStandardOutput $stdoutLog `
            -RedirectStandardError $stderrLog `
            -PassThru

        Set-Content -LiteralPath $pidFile -Value $process.Id -Encoding UTF8
        Write-Summary "  pid: $($process.Id)"
    }

    exit 0
}

if (-not (Test-Path -LiteralPath $multiServer)) {
    throw "win_multi_video_server.py was not found: $multiServer"
}

$stdoutLog = Join-Path $LogDir "windows_multi_dashboard.log"
$stderrLog = Join-Path $LogDir "windows_multi_dashboard.err.log"
$pidFile = Join-Path $LogDir "windows_multi_dashboard.pid"

Set-Content -LiteralPath $stdoutLog -Value "" -Encoding UTF8
Set-Content -LiteralPath $stderrLog -Value "" -Encoding UTF8

Write-Summary "Starting unified Windows multi-device receiver"
Write-Summary "Browser UI:  http://127.0.0.1:$HttpPort/"
Write-Summary "RK3568 host: $RkHost"
Write-Summary "Count:       $Count"
Write-Summary "Video ports: $BaseVideoPort + instance * $PortStep"
Write-Summary "Control:     $BaseControlPort + instance * $PortStep"
Write-Summary "RK recover:  $RkHost`:$RkRecoverPort"
Write-Summary "stdout:      $stdoutLog"
Write-Summary "stderr:      $stderrLog"

$args = @(
    $multiServer,
    "--http-port", "$HttpPort",
    "--control-host", $RkHost,
    "--rk-recover-host", $RkHost,
    "--rk-recover-port", "$RkRecoverPort",
    "--count", "$Count",
    "--base-video-port", "$BaseVideoPort",
    "--base-control-port", "$BaseControlPort",
    "--port-step", "$PortStep"
)

if (-not $NoBrowser) {
    $args += "--open-browser"
}

$process = Start-Process $python.Source `
    -ArgumentList $args `
    -WindowStyle Hidden `
    -RedirectStandardOutput $stdoutLog `
    -RedirectStandardError $stderrLog `
    -PassThru

Set-Content -LiteralPath $pidFile -Value $process.Id -Encoding UTF8
Write-Summary "pid:         $($process.Id)"
