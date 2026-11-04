# Run the built game for N seconds from PowerShell (no shell wrapper between us and the process: the real exit code).
#   powershell -File scripts/ps_run.ps1 <log name> [seconds]     (extra env such as TOMO_SAMPLE=1 set by the caller)
#   TOMO_WORK / TOMO_DUMP: another build folder and game dump (default build\work, build\dump), e.g. an update's build
param([string]$Log = 'run.log', [int]$Seconds = 40)
$repo = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$work = if ($env:TOMO_WORK) { $env:TOMO_WORK } else { "$repo\build\work" }
$dump = if ($env:TOMO_DUMP) { $env:TOMO_DUMP } else { "$repo\build\dump" }
Set-Location $work
if (-not $env:SSBU_ROMFS) { $env:SSBU_ROMFS = "$dump\romfs" }
if (-not $env:SSBU_ERROR_DIALOG) { $env:SSBU_ERROR_DIALOG = '0' }
if (-not $env:SSBU_OS_CONSOLE_CORES) { $env:SSBU_OS_CONSOLE_CORES = '1' }
$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = "$work\tomodachi_native.exe"
$psi.Arguments = "`"$dump\exefs\main.elf`" $Seconds"
$psi.WorkingDirectory = $work     # Set-Location does not move the process directory: the exe needs build\work for ../cache and ../dump
$psi.UseShellExecute = $false
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError = $true
$p = [System.Diagnostics.Process]::Start($psi)
$out = $p.StandardOutput.ReadToEndAsync()
$err = $p.StandardError.ReadToEndAsync()
if (-not $p.WaitForExit(($Seconds + 90) * 1000)) { Write-Output "still running after the limit: killed"; $p.Kill() }
$p.WaitForExit()
[IO.File]::WriteAllText("$work\$Log", $out.Result + $err.Result + "`nexit code $($p.ExitCode) (0x$('{0:X8}' -f $p.ExitCode))`n")
Write-Output ("exit code {0} (0x{1:X8}), {2} log lines" -f $p.ExitCode, $p.ExitCode, ($out.Result -split "`n").Count)
