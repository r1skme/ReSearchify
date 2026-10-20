param(
    [string]$Executable = ".\build\Release\cs_bench.exe",
    [string]$Work = ".\benchmark-data",
    [ValidateSet("all", "generate", "create", "update", "search", "verify")]
    [string]$Phase = "all",
    [string]$IndexName = "index",
    [int]$Files = 100000,
    [int]$Lines = 50,
    [int]$Threads = 16,
    [int]$MemoryMB = 256,
    [int]$Repeats = 7,
    [ValidateSet("compact", "fast")]
    [string]$Profile = "compact"
)
$ErrorActionPreference = "Stop"

function Quote-WindowsArgument([string]$Value) {
    if ($Value.Length -gt 0 -and $Value -notmatch '[\s"]') { return $Value }
    $escaped = [regex]::Replace($Value, '(\\*)"', '$1$1\"')
    $escaped = [regex]::Replace($escaped, '(\\+)$', '$1$1')
    return '"' + $escaped + '"'
}

$exePath = (Resolve-Path -LiteralPath $Executable).Path
$arguments = @("--work", $Work, "--phase", $Phase, "--index", $IndexName,
               "--files", "$Files", "--lines", "$Lines", "--threads", "$Threads",
               "--memory-mb", "$MemoryMB", "--repeats", "$Repeats", "--profile", "$Profile")
$startInfo = New-Object System.Diagnostics.ProcessStartInfo
$startInfo.FileName = $exePath
$startInfo.Arguments = ($arguments | ForEach-Object { Quote-WindowsArgument $_ }) -join ' '
$startInfo.UseShellExecute = $false
$startInfo.RedirectStandardOutput = $false
$startInfo.RedirectStandardError = $false
$process = New-Object System.Diagnostics.Process
$process.StartInfo = $startInfo
$peakWorkingSet = [long]0
$peakPrivate = [long]0
$clock = [System.Diagnostics.Stopwatch]::StartNew()
if (-not $process.Start()) { throw "Could not start benchmark" }
try {
    while (-not $process.WaitForExit(50)) {
        $process.Refresh()
        if ($process.PeakWorkingSet64 -gt $peakWorkingSet) { $peakWorkingSet = $process.PeakWorkingSet64 }
        if ($process.PrivateMemorySize64 -gt $peakPrivate) { $peakPrivate = $process.PrivateMemorySize64 }
    }
    $process.WaitForExit()
    try {
        $process.Refresh()
        if ($process.PeakWorkingSet64 -gt $peakWorkingSet) { $peakWorkingSet = $process.PeakWorkingSet64 }
    } catch {
        # Some Windows versions stop exposing memory counters after process exit.
        # Periodic samples above remain available; no benchmark result is fabricated.
    }
    $exitCode = $process.ExitCode
    $clock.Stop()
    "MEMORY peak_working_set_bytes=$peakWorkingSet sampled_peak_private_bytes=$peakPrivate sample_ms=50 elapsed_seconds=$($clock.Elapsed.TotalSeconds) exit_code=$exitCode"
    "PeakWorkingSet includes runtime, stacks, filesystem buffers and allocator overhead. Private memory is sampled and may miss sub-50 ms spikes."
    if ($exitCode -ne 0) { throw "Benchmark failed with exit code $exitCode" }
} finally {
    $process.Dispose()
}
