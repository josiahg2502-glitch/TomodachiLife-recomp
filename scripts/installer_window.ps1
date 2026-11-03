# The older installer window of the Tomodachi Life: Living the Dream native PC port (GPL-3.0), run by hand: scripts\bootstrap.ps1
# puts a private Python into deps\python and then runs this file in its own PowerShell (or: powershell -NoProfile -STA -File
# scripts\installer_window.ps1, where the PC's execution policy lets local scripts run). The release's way is the launcher's INSTALL screen.
# Windows PowerShell 5.1 + System.Windows.Forms only; no modules, nothing is installed, nothing is sent anywhere.
#
# What it does: looks for your own game files (scripts\find_inputs.py --json) and checks the PC (scripts\precheck.py --json),
# both in the background so the window never freezes; lets you change every path; then runs
#   python -u scripts\install.py --no-gui --progress-json [--base F | --dump D] [--update F] [--keys F]
#          [--firmware D] [--build-dir D] [--jobs N] [--dry-run] [--no-runtime] [--no-shortcuts] [--no-self-update]
# (--build-dir: the "Install location" field, passed when it is not <repo>\build; install.py remembers it in build_location.txt)
# and shows its steps, one progress bar, the time left and the log. install.py is resumable, so after an error or a Close the
# button says "Resume". Lines "##ssbu {json}" on stdout are progress events (schema below), every other line goes to the log
# box and to build\logs\installer_window.log.
#
# Event schema (one JSON object per line after the prefix "##ssbu "; every field but "ev" is optional):
#   {"ev":"plan","steps":[{"id":"update","title":"Check for updates","est_s":5}, ...]}
#   {"ev":"step","id":"compile","state":"start"|"done"|"skip"|"fail"|"dry","secs":12.3,"msg":"..."}
#   {"ev":"progress","id":"compile","done":120,"total":1162}
#   {"ev":"done","ok":true,"msg":"..."}      {"ev":"error","msg":"...","hint":"...","action":"msvc"|""}
#   (action "msvc": the stop needs the MSVC Build Tools; the window then shows its "Install Build Tools" button)
# Without events it reads setup.py's lines ("[6/11] lift ...", "[6/11] lift: ok  19.3s", "... would run", "... skipped").
#
# Test switches (no input needed, nothing is installed): -SelfTest <seconds> [-SelfTestKind json|fallback|error] feeds a scripted
# event sequence through the same parser and prints the final state; -NoRun runs only the two scans, prints what was filled in
# and closes; -Python <exe> overrides the Python (default: deps\python\python.exe, else build\python_path.txt).
#
# Ideas (the wizard idea, auto-filled folders each with a status line, and the "never kill by name" care) after WindowSmash's
# tools\install_windowsmash.ps1 by SNO2207 (Show-Wizard); this is a separate implementation.
param(
    [string]$Python = '',
    [int]$SelfTest = 0,
    [ValidateSet('json', 'fallback', 'error')][string]$SelfTestKind = 'json',
    [switch]$NoRun,
    [string]$Shot = ''          # test only: save a picture of the window (PNG) just before it closes
)
$ErrorActionPreference = 'Continue'
Set-StrictMode -Off

$repo = Split-Path -Parent $PSScriptRoot
# The install location (dump, build, logs, saves, .nsz cache): <repo>\build, or the folder chosen in the "Install location"
# field and remembered by install.py in build_location.txt (scripts\build_location.py). python_path.txt (bootstrap.ps1)
# always stays in <repo>\build.
$defaultBuild = Join-Path $repo 'build'
$locFile = Join-Path $repo 'build_location.txt'
function Get-StoredBuild {
    if (Test-Path -LiteralPath $locFile) {
        try {
            $t = [IO.File]::ReadAllLines($locFile, [Text.Encoding]::UTF8) | Select-Object -First 1
            if ($t) { $t = $t.Trim().Trim('"'); if ($t) { return [Environment]::ExpandEnvironmentVariables($t) } }
        } catch {}
    }
    return ''
}
$storedBuild = Get-StoredBuild
$buildDir = $(if ($storedBuild) { $storedBuild } else { $defaultBuild })
$logDir = [IO.Path]::Combine($buildDir, 'logs')
try { if (-not [IO.Directory]::Exists([IO.Path]::GetPathRoot($logDir))) { $logDir = [IO.Path]::Combine($defaultBuild, 'logs') } } catch {}  # a drive that is gone
$logFile = [IO.Path]::Combine($logDir, 'installer_window.log')
$testMode = ($SelfTest -gt 0) -or $NoRun.IsPresent

# ---------------------------------------------------------------------------------------------------- native helpers
Add-Type -TypeDefinition @'
using System;
using System.Collections.Concurrent;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;

public static class SsbuNative {
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
}

// Runs one process with redirected output. The data-received handlers are C# (a PowerShell script block cannot run on the
// thread-pool threads these events come from); lines are queued and the window drains the queue with a Timer.
public class SsbuProc {
    public ConcurrentQueue<string> Lines;
    public StringBuilder Output = new StringBuilder();     // capture mode: the whole stdout
    public StringBuilder ErrorText = new StringBuilder();  // capture mode: the whole stderr
    public volatile bool Done;
    public int ExitCode = -1;
    public int Pid;
    public string StartError = "";
    bool capture;
    Process p;

    public SsbuProc(ConcurrentQueue<string> q, bool captureMode) { Lines = q; capture = captureMode; }

    public bool Start(string exe, string args, string cwd) {
        try {
            ProcessStartInfo psi = new ProcessStartInfo(exe, args);
            psi.WorkingDirectory = cwd;
            psi.UseShellExecute = false;
            psi.CreateNoWindow = true;
            psi.RedirectStandardOutput = true;
            psi.RedirectStandardError = true;
            psi.StandardOutputEncoding = Encoding.UTF8;
            psi.StandardErrorEncoding = Encoding.UTF8;
            psi.EnvironmentVariables["PYTHONIOENCODING"] = "utf-8";
            psi.EnvironmentVariables["PYTHONUTF8"] = "1";
            psi.EnvironmentVariables["PYTHONUNBUFFERED"] = "1";
            p = new Process();
            p.StartInfo = psi;
            p.OutputDataReceived += delegate(object s, DataReceivedEventArgs e) {
                if (e.Data == null) return;
                if (capture) { lock (Output) { Output.AppendLine(e.Data); } } else { Lines.Enqueue(e.Data); }
            };
            p.ErrorDataReceived += delegate(object s, DataReceivedEventArgs e) {
                if (e.Data == null) return;
                if (capture) { lock (ErrorText) { ErrorText.AppendLine(e.Data); } } else { Lines.Enqueue(e.Data); }
            };
            p.Start();
            Pid = p.Id;
            p.BeginOutputReadLine();
            p.BeginErrorReadLine();
            Thread t = new Thread(delegate() {
                try { p.WaitForExit(); ExitCode = p.ExitCode; } catch (Exception) { }
                Done = true;
            });
            t.IsBackground = true;
            t.Start();
            return true;
        } catch (Exception ex) {
            StartError = ex.Message;
            Done = true;
            return false;
        }
    }

    public string GetOutput() { lock (Output) { return Output.ToString(); } }
    public string GetErrorText() { lock (ErrorText) { return ErrorText.ToString(); } }

    // Stops ONLY the process tree this object started, by its PID (never by name).
    public void Kill() {
        if (Pid <= 0 || Done) return;
        try {
            ProcessStartInfo k = new ProcessStartInfo("taskkill.exe", "/PID " + Pid + " /T /F");
            k.CreateNoWindow = true;
            k.UseShellExecute = false;
            Process kp = Process.Start(k);
            if (kp != null) kp.WaitForExit(15000);
        } catch (Exception) { }
    }
}
'@

try { [void][SsbuNative]::SetProcessDPIAware() } catch {}     # sharp text on scaled displays (before any window exists)
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
[System.Windows.Forms.Application]::EnableVisualStyles()
[System.Windows.Forms.Application]::SetUnhandledExceptionMode([System.Windows.Forms.UnhandledExceptionMode]::CatchException)

# ---------------------------------------------------------------------------------------------------- python
function Resolve-Python {
    if ($Python) { return $Python }
    $p = Join-Path $repo 'deps\python\python.exe'
    if (Test-Path -LiteralPath $p) { return $p }
    $f = Join-Path $defaultBuild 'python_path.txt'
    if (Test-Path -LiteralPath $f) {
        $t = (Get-Content -LiteralPath $f -TotalCount 1 -ErrorAction SilentlyContinue)
        if ($t) { $t = $t.Trim().Trim('"'); if ($t -and (Test-Path -LiteralPath $t)) { return $t } }
    }
    return ''
}
$script:py = Resolve-Python

function Q([string]$s) {          # one command-line argument, quoted the way CreateProcess expects
    $t = [regex]::Replace($s, '(\\*)"', '$1$1\"')
    $t = [regex]::Replace($t, '(\\+)$', '$1$1')
    return '"' + $t + '"'
}

# ---------------------------------------------------------------------------------------------------- state
$script:q = New-Object 'System.Collections.Concurrent.ConcurrentQueue[string]'
$script:scanFind = $null; $script:scanPre = $null
$script:found = $null; $script:foundDone = $false; $script:preDone = $false
$script:job = $null            # SsbuProc of the running install/update job
$script:running = $false
$script:jobKind = ''           # 'install' | 'update'
$script:jobDry = $false
$script:jobStart = [DateTime]::Now
$script:steps = New-Object System.Collections.ArrayList
$script:stepIdx = @{}
$script:jsonSeen = $false
$script:fallbackN = 0
$script:errMsg = ''; $script:errHint = ''; $script:errAction = ''
$script:doneOk = $null; $script:doneMsg = ''
$script:buildGb = 8.0; $script:dumpGb = 17.0      # what a build and the unpacked game take (precheck.py sends its own numbers)
$script:locChangedAt = $null; $script:preAgain = $false
$script:lastExit = $null
$script:failedBefore = $false
$script:succeeded = $false
$script:logPending = New-Object System.Text.StringBuilder
$script:filePending = New-Object System.Text.StringBuilder
$script:suppressStatus = $false
$script:testDump = New-Object System.Collections.ArrayList
$script:fakeRun = $false; $script:fakeDone = $false; $script:fakeCode = 0

# setup.py's own steps, by their share of a first build (used only when no plan event arrives)
$script:fallbackWeights = @{ deps = 1; stage = 1; convert = 1; tables = 1; lift = 4; sdk = 2; compile = 80; link = 3; prepare = 5 }

# ---------------------------------------------------------------------------------------------------- UI
$fontUi = New-Object System.Drawing.Font('Segoe UI', 9)
$fontBold = New-Object System.Drawing.Font('Segoe UI', 9, [System.Drawing.FontStyle]::Bold)
$fontTitle = New-Object System.Drawing.Font('Segoe UI', 13, [System.Drawing.FontStyle]::Bold)
$fontMono = New-Object System.Drawing.Font('Consolas', 9)
# dark mode: follows Windows (Settings > Personalisation > Colours > app mode); SSBU_INSTALLER_THEME=dark / light forces one
$script:dark = $false
try {
    $tm = [string]$env:SSBU_INSTALLER_THEME
    if ($tm -eq 'dark') { $script:dark = $true }
    elseif ($tm -ne 'light') {
        $v = Get-ItemProperty -Path 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Themes\Personalize' -Name AppsUseLightTheme -ErrorAction Stop
        $script:dark = ($v.AppsUseLightTheme -eq 0)
    }
} catch {}
if ($script:dark) {
    $cOk = [System.Drawing.Color]::FromArgb(110, 210, 110)
    $cWarn = [System.Drawing.Color]::FromArgb(240, 175, 70)
    $cFail = [System.Drawing.Color]::FromArgb(255, 110, 105)
    $cInfo = [System.Drawing.Color]::FromArgb(110, 175, 255)
    $cDim = [System.Drawing.Color]::FromArgb(160, 160, 160)
    $cFg = [System.Drawing.Color]::FromArgb(232, 232, 232)
    $cBg = [System.Drawing.Color]::FromArgb(32, 32, 32)
    $cBgField = [System.Drawing.Color]::FromArgb(45, 45, 48)
    $cBgButton = [System.Drawing.Color]::FromArgb(58, 58, 62)
} else {
    $cOk = [System.Drawing.Color]::FromArgb(0, 128, 0)
    $cWarn = [System.Drawing.Color]::FromArgb(176, 96, 0)
    $cFail = [System.Drawing.Color]::FromArgb(200, 30, 30)
    $cInfo = [System.Drawing.Color]::FromArgb(0, 90, 180)
    $cDim = [System.Drawing.Color]::DimGray
    $cFg = [System.Drawing.SystemColors]::ControlText
    $cBg = [System.Drawing.SystemColors]::Control
    $cBgField = [System.Drawing.Color]::White
    $cBgButton = [System.Drawing.SystemColors]::Control
}

$scale = 1.0
try { $gx = [System.Drawing.Graphics]::FromHwnd([IntPtr]::Zero); $scale = [Math]::Max(1.0, $gx.DpiX / 96.0); $gx.Dispose() } catch {}
function S([double]$v) { return [int][Math]::Round($v * $scale) }
$script:wrapLabels = New-Object System.Collections.ArrayList

$form = New-Object System.Windows.Forms.Form
$form.Text = 'Tomodachi Life native - installer'
$form.AutoScaleMode = [System.Windows.Forms.AutoScaleMode]::None      # sizes below are in 96-dpi units times $scale; fonts are in points
$form.Font = $fontUi
$form.StartPosition = [System.Windows.Forms.FormStartPosition]::CenterScreen
$wa = [System.Windows.Forms.Screen]::PrimaryScreen.WorkingArea
$form.ClientSize = New-Object System.Drawing.Size([Math]::Min((S 1000), [int]($wa.Width * 0.92)), [Math]::Min((S 980), [int]($wa.Height * 0.95)))
$form.MinimumSize = New-Object System.Drawing.Size([Math]::Min((S 780), [int]($wa.Width * 0.92)), [Math]::Min((S 560), [int]($wa.Height * 0.92)))

function New-Label([string]$text, $color = $null, $font = $null) {
    $l = New-Object System.Windows.Forms.Label
    $l.Text = $text; $l.AutoSize = $true; $l.Margin = New-Object System.Windows.Forms.Padding(3, 3, 3, 3)
    if ($color) { $l.ForeColor = $color }
    if ($font) { $l.Font = $font }
    return $l
}
function New-Button([string]$text, [int]$w = 110) {
    $b = New-Object System.Windows.Forms.Button
    $b.Text = $text; $b.AutoSize = $true; $b.AutoSizeMode = 'GrowAndShrink'
    $b.MinimumSize = New-Object System.Drawing.Size((S $w), (S 28))
    $b.Margin = New-Object System.Windows.Forms.Padding(3, 3, 3, 3)
    return $b
}

# the page: one column, rows top to bottom
$page = New-Object System.Windows.Forms.TableLayoutPanel
$page.Dock = 'Fill'; $page.ColumnCount = 1; $page.RowCount = 8
$page.Padding = New-Object System.Windows.Forms.Padding(10, 8, 10, 8)
[void]$page.ColumnStyles.Add((New-Object System.Windows.Forms.ColumnStyle([System.Windows.Forms.SizeType]::Percent, 100)))
$rowStyles = @(
    @('AutoSize', 0), @('AutoSize', 0), @('AutoSize', 0), @('Percent', 34), @('AutoSize', 0), @('AutoSize', 0), @('Percent', 66), @('AutoSize', 0))
foreach ($rs in $rowStyles) {
    [void]$page.RowStyles.Add((New-Object System.Windows.Forms.RowStyle([System.Windows.Forms.SizeType]$rs[0], [single]$rs[1])))
}
$scroll = New-Object System.Windows.Forms.Panel
$scroll.Dock = 'Fill'; $scroll.AutoScroll = $true
$scroll.AutoScrollMinSize = New-Object System.Drawing.Size((S 800), (S 1100))
$scroll.Controls.Add($page)
$form.Controls.Add($scroll)
function Fit-Labels {
    $w = $scroll.ClientSize.Width
    foreach ($e in $script:wrapLabels) {
        $lw = $(if ($e[1] -eq 'field') { $w - (S 150) } else { $w - (S 40) })
        if ($lw -lt (S 200)) { $lw = S 200 }
        $e[0].MaximumSize = New-Object System.Drawing.Size($lw, 0)
    }
}
$form.Add_Resize({ Fit-Labels })

# row 0: title
$pTitle = New-Object System.Windows.Forms.FlowLayoutPanel
$pTitle.AutoSize = $true; $pTitle.Dock = 'Fill'; $pTitle.FlowDirection = 'TopDown'; $pTitle.WrapContents = $false
[void]$pTitle.Controls.Add((New-Label 'Build Tomodachi Life: Living the Dream for this PC from your own game files' $null $fontTitle))
[void]$pTitle.Controls.Add((New-Label 'Nothing of the game is in this download: everything is made here from the files below. Nothing is sent anywhere.' $cDim))
$page.Controls.Add($pTitle, 0, 0)

# row 1: the inputs
$gIn = New-Object System.Windows.Forms.GroupBox
$gIn.Text = '1. Your game files (found on this PC; change any of them) and where to install'
$gIn.Dock = 'Fill'; $gIn.AutoSize = $true; $gIn.AutoSizeMode = 'GrowAndShrink'
$tIn = New-Object System.Windows.Forms.TableLayoutPanel
$tIn.Dock = 'Top'; $tIn.AutoSize = $true; $tIn.ColumnCount = 4
$tIn.Padding = New-Object System.Windows.Forms.Padding(4, 2, 4, 4)
[void]$tIn.ColumnStyles.Add((New-Object System.Windows.Forms.ColumnStyle([System.Windows.Forms.SizeType]::AutoSize)))
[void]$tIn.ColumnStyles.Add((New-Object System.Windows.Forms.ColumnStyle([System.Windows.Forms.SizeType]::Percent, 100)))
[void]$tIn.ColumnStyles.Add((New-Object System.Windows.Forms.ColumnStyle([System.Windows.Forms.SizeType]::AutoSize)))
[void]$tIn.ColumnStyles.Add((New-Object System.Windows.Forms.ColumnStyle([System.Windows.Forms.SizeType]::AutoSize)))
$gIn.Controls.Add($tIn)
$page.Controls.Add($gIn, 0, 1)

$script:F = @{}
function Add-Field([string]$key, [string]$label, [string]$kind, [string]$hint) {
    $r = $tIn.RowCount
    $tIn.RowCount = $r + 2
    [void]$tIn.RowStyles.Add((New-Object System.Windows.Forms.RowStyle([System.Windows.Forms.SizeType]::AutoSize)))
    [void]$tIn.RowStyles.Add((New-Object System.Windows.Forms.RowStyle([System.Windows.Forms.SizeType]::AutoSize)))
    $lab = New-Label $label $null $fontBold
    $lab.Anchor = 'Left'
    $tIn.Controls.Add($lab, 0, $r)
    $box = New-Object System.Windows.Forms.TextBox
    $box.Dock = 'Fill'; $box.Margin = New-Object System.Windows.Forms.Padding(3, 4, 3, 3)
    $tIn.Controls.Add($box, 1, $r)
    $st = New-Label $hint $cDim
    $st.Margin = New-Object System.Windows.Forms.Padding(3, 0, 3, 6)
    [void]$script:wrapLabels.Add(@($st, 'field'))
    $tIn.Controls.Add($st, 1, ($r + 1))
    $tIn.SetColumnSpan($st, 3)
    $entry = @{ Box = $box; Status = $st; Kind = $kind; Hint = $hint; Buttons = @() }
    if ($kind -eq 'base') {
        $b1 = New-Button 'File...' 80; $b2 = New-Button 'Folder...' 80
        $tIn.Controls.Add($b1, 2, $r); $tIn.Controls.Add($b2, 3, $r)
        $entry.Buttons = @($b1, $b2)
        $b1.Add_Click({ Browse-File 'base' 'Base game (.xci, .nsp, .nsz, .nca)' 'Switch game files|*.xci;*.xcz;*.nsp;*.nsz;*.nca|All files|*.*' })
        $b2.Add_Click({ Browse-Folder 'base' 'Folder with exefs\ and romfs\ (a dump you already made)' })
    } elseif ($kind -eq 'update') {
        $b1 = New-Button 'Browse...' 80
        $tIn.Controls.Add($b1, 2, $r); $tIn.SetColumnSpan($b1, 2)
        $entry.Buttons = @($b1)
        $b1.Add_Click({ Browse-File 'update' 'Update 1.0.4 (.nsp, .nsz), optional' 'Switch update|*.nsp;*.nsz|All files|*.*' })
    } elseif ($kind -eq 'keys') {
        $b1 = New-Button 'Browse...' 80
        $tIn.Controls.Add($b1, 2, $r); $tIn.SetColumnSpan($b1, 2)
        $entry.Buttons = @($b1)
        $b1.Add_Click({ Browse-File 'prodkeys' 'prod.keys' 'Keys|*.keys|All files|*.*' })
    } elseif ($kind -eq 'fw') {
        $b1 = New-Button 'Browse...' 80
        $tIn.Controls.Add($b1, 2, $r); $tIn.SetColumnSpan($b1, 2)
        $entry.Buttons = @($b1)
        $b1.Add_Click({ Browse-Folder 'firmware' 'Firmware folder (optional: the .nca files of your firmware)' })
    } elseif ($kind -eq 'loc') {
        $b1 = New-Button 'Choose...' 80; $b2 = New-Button 'Default' 80
        $tIn.Controls.Add($b1, 2, $r); $tIn.Controls.Add($b2, 3, $r)
        $entry.Buttons = @($b1, $b2)
        $b1.Add_Click({ Browse-Location })
        $b2.Add_Click({ $script:F['buildloc'].Box.Text = $defaultBuild })
    }
    $script:F[$key] = $entry
    $box.Add_TextChanged({ if (-not $script:suppressStatus) { Update-FieldStatus } })
}
Add-Field 'base' 'Base game' 'base' 'Your game: .xci / .nsp / .nsz / .nca, or a folder you already extracted (with exefs\ and romfs\). Looking...'
Add-Field 'update' 'Update' 'update' 'Optional: 1.0.4 (.nsp / .nsz). Without it the game is built as 1.0.0. Looking...'
Add-Field 'prodkeys' 'prod.keys' 'keys' 'Your console keys: needed to read package files. Looking...'
Add-Field 'firmware' 'Firmware (optional)' 'fw' 'Only the fonts change. With your firmware: the console''s own fonts, Chinese / Japanese / Korean text included. Without: open-source fonts (same game, Latin text looks a little different, Asian characters missing). Looking...'

# DLC list (optional)
$dr = $tIn.RowCount
$tIn.RowCount = $dr + 2
[void]$tIn.RowStyles.Add((New-Object System.Windows.Forms.RowStyle([System.Windows.Forms.SizeType]::AutoSize)))
[void]$tIn.RowStyles.Add((New-Object System.Windows.Forms.RowStyle([System.Windows.Forms.SizeType]::AutoSize)))
$dlab = New-Label 'DLC (optional)' $null $fontBold; $dlab.Anchor = 'Top,Left'; $dlab.Margin = New-Object System.Windows.Forms.Padding(3, 6, 3, 3)
$tIn.Controls.Add($dlab, 0, $dr)
$script:dlcList = New-Object System.Windows.Forms.ListBox
$script:dlcList.Dock = 'Fill'; $script:dlcList.Height = (S 46); $script:dlcList.HorizontalScrollBar = $true
$script:dlcList.SelectionMode = 'MultiExtended'; $script:dlcList.IntegralHeight = $false
$script:dlcList.Margin = New-Object System.Windows.Forms.Padding(3, 4, 3, 3)
$tIn.Controls.Add($script:dlcList, 1, $dr)
$dbtn = New-Object System.Windows.Forms.FlowLayoutPanel
$dbtn.FlowDirection = 'TopDown'; $dbtn.AutoSize = $true; $dbtn.WrapContents = $false
$script:btnDlcAdd = New-Button 'Add...' 80; $script:btnDlcDel = New-Button 'Remove' 80; $script:btnDlcClr = New-Button 'Clear' 80
[void]$dbtn.Controls.Add($script:btnDlcAdd); [void]$dbtn.Controls.Add($script:btnDlcDel); [void]$dbtn.Controls.Add($script:btnDlcClr)
$tIn.Controls.Add($dbtn, 2, $dr); $tIn.SetColumnSpan($dbtn, 2)
$script:dlcStatus = New-Label '' $cDim
$script:dlcStatus.Margin = New-Object System.Windows.Forms.Padding(3, 0, 3, 6)
[void]$script:wrapLabels.Add(@($script:dlcStatus, 'field'))
$tIn.Controls.Add($script:dlcStatus, 1, ($dr + 1)); $tIn.SetColumnSpan($script:dlcStatus, 3)
# this game has no add-on content: the DLC row stays empty and hidden
$dlab.Visible = $false; $script:dlcList.Visible = $false; $dbtn.Visible = $false; $script:dlcStatus.Visible = $false

# install location: where the unpacked game (~7 GB), the build (~4 GB), the logs and the saves go
Add-Field 'buildloc' 'Install location' 'loc' 'Where the unpacked game and the build go (about 15 GB). Choose a folder on a drive with enough free space.'
$script:suppressStatus = $true; $script:F['buildloc'].Box.Text = $buildDir; $script:suppressStatus = $false
$script:F['buildloc'].Box.Add_TextChanged({ $script:locChangedAt = [DateTime]::Now })

# row 2: PC check + options
$p2 = New-Object System.Windows.Forms.TableLayoutPanel
$p2.Dock = 'Fill'; $p2.AutoSize = $true; $p2.ColumnCount = 2; $p2.RowCount = 1
[void]$p2.ColumnStyles.Add((New-Object System.Windows.Forms.ColumnStyle([System.Windows.Forms.SizeType]::Percent, 55)))
[void]$p2.ColumnStyles.Add((New-Object System.Windows.Forms.ColumnStyle([System.Windows.Forms.SizeType]::Percent, 45)))
$page.Controls.Add($p2, 0, 2)

$gPre = New-Object System.Windows.Forms.GroupBox
$gPre.Text = '2. This PC'; $gPre.Dock = 'Fill'
$script:preList = New-Object System.Windows.Forms.ListView
$script:preList.View = 'Details'; $script:preList.FullRowSelect = $true; $script:preList.HeaderStyle = 'None'
$script:preList.Dock = 'Fill'; $script:preList.Height = (S 132); $script:preList.BorderStyle = 'None'; $script:preList.Scrollable = $true
[void]$script:preList.Columns.Add('', (S 50)); [void]$script:preList.Columns.Add('', (S 140)); [void]$script:preList.Columns.Add('', (S 900))
$gPre.Controls.Add($script:preList)
$gPre.MinimumSize = New-Object System.Drawing.Size(0, (S 170))
$p2.Controls.Add($gPre, 0, 0)

$gOpt = New-Object System.Windows.Forms.GroupBox
$gOpt.Text = '3. Options'; $gOpt.Dock = 'Fill'
$fOpt = New-Object System.Windows.Forms.FlowLayoutPanel
$fOpt.Dock = 'Fill'; $fOpt.FlowDirection = 'TopDown'; $fOpt.WrapContents = $false; $fOpt.AutoScroll = $true
function New-Check([string]$text, [bool]$on) {
    $c = New-Object System.Windows.Forms.CheckBox
    $c.Text = $text; $c.Checked = $on; $c.AutoSize = $true; $c.Margin = New-Object System.Windows.Forms.Padding(6, 3, 3, 3)
    [void]$fOpt.Controls.Add($c)
    return $c
}
$script:cbShort = New-Check 'Create desktop and Start menu shortcuts' $true
$script:cbUpd = New-Check 'Check for updates first' $true
$script:cbRuntime = New-Check 'Use the prebuilt runtime (recommended)' $true
$script:tips = New-Object System.Windows.Forms.ToolTip
$script:tips.AutoPopDelay = 20000
$script:tips.SetToolTip($script:cbRuntime, 'No Visual Studio needed (about 3 GB less disk), some minutes shorter. The game you get is the same.')
$script:cbFast = New-Check 'Shorter build, slower game (no inlining)' $false
$script:tips.SetToolTip($script:cbFast, 'The game compiles in about half the time, but runs slower on the CPU (up to 25-30 % on the main thread). Leave it off unless the build takes too long on your PC.')
$script:cbDry = New-Check 'Dry run (show what would happen, change nothing)' $false
$fJobs = New-Object System.Windows.Forms.FlowLayoutPanel
$fJobs.AutoSize = $true; $fJobs.WrapContents = $false
$lblJ = New-Label 'Compiler jobs:'
[void]$fJobs.Controls.Add($lblJ)
$script:numJobs = New-Object System.Windows.Forms.NumericUpDown
$script:numJobs.Minimum = 1; $script:numJobs.Maximum = 64; $script:numJobs.Width = (S 56)
$script:numJobs.Value = [Math]::Max(1, [Math]::Min(24, [Environment]::ProcessorCount - 2))
[void]$fJobs.Controls.Add($script:numJobs)
$script:lblJobs = New-Label '(fewer = slower, PC stays usable)' $cDim
[void]$fJobs.Controls.Add($script:lblJobs)
$tJ = 'Compilers running at once. Each takes a CPU thread and about 1.5 GB of RAM: more than the safe number can run the PC out of memory. Fewer = slower, but the PC stays usable.'
foreach ($c in @($lblJ, $script:numJobs, $script:lblJobs)) { $script:tips.SetToolTip($c, $tJ) }
[void]$fOpt.Controls.Add($fJobs)
$gOpt.Controls.Add($fOpt)
$p2.Controls.Add($gOpt, 1, 0)

# row 3: steps
$gSteps = New-Object System.Windows.Forms.GroupBox
$gSteps.Text = '4. Steps'; $gSteps.Dock = 'Fill'; $gSteps.MinimumSize = New-Object System.Drawing.Size(0, (S 210))
$script:stepList = New-Object System.Windows.Forms.ListView
$script:stepList.View = 'Details'; $script:stepList.FullRowSelect = $true; $script:stepList.GridLines = $false
$script:stepList.Dock = 'Fill'; $script:stepList.HeaderStyle = 'Nonclickable'; $script:stepList.BorderStyle = 'None'
[void]$script:stepList.Columns.Add('Step', (S 300)); [void]$script:stepList.Columns.Add('State', (S 100))
[void]$script:stepList.Columns.Add('Time', (S 70)); [void]$script:stepList.Columns.Add('Note', (S 500))
$il = New-Object System.Windows.Forms.ImageList
$il.ImageSize = New-Object System.Drawing.Size((S 14), (S 14)); $il.ColorDepth = 'Depth32Bit'
$stateColors = [ordered]@{
    wait = [System.Drawing.Color]::Silver; run = [System.Drawing.Color]::FromArgb(0, 120, 215); done = $cOk
    skip = [System.Drawing.Color]::FromArgb(140, 140, 140); fail = $cFail; dry = [System.Drawing.Color]::FromArgb(230, 160, 0) }
foreach ($k in $stateColors.Keys) {
    $bmp = New-Object System.Drawing.Bitmap((S 14), (S 14))
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = 'AntiAlias'
    $g.FillEllipse((New-Object System.Drawing.SolidBrush($stateColors[$k])), (S 1), (S 1), (S 11), (S 11))
    $g.Dispose()
    $il.Images.Add($k, $bmp)
}
$script:stepList.SmallImageList = $il
$gSteps.Controls.Add($script:stepList)
$page.Controls.Add($gSteps, 0, 3)
$stateText = @{ wait = 'waiting'; run = 'running'; done = 'done'; skip = 'skipped'; fail = 'FAILED'; dry = 'would run' }

# row 4: progress
$p4 = New-Object System.Windows.Forms.TableLayoutPanel
$p4.Dock = 'Fill'; $p4.AutoSize = $true; $p4.ColumnCount = 3; $p4.RowCount = 2
[void]$p4.ColumnStyles.Add((New-Object System.Windows.Forms.ColumnStyle([System.Windows.Forms.SizeType]::Percent, 100)))
[void]$p4.ColumnStyles.Add((New-Object System.Windows.Forms.ColumnStyle([System.Windows.Forms.SizeType]::AutoSize)))
[void]$p4.ColumnStyles.Add((New-Object System.Windows.Forms.ColumnStyle([System.Windows.Forms.SizeType]::AutoSize)))
$script:bar = New-Object System.Windows.Forms.ProgressBar
$script:bar.Dock = 'Fill'; $script:bar.Minimum = 0; $script:bar.Maximum = 1000; $script:bar.Height = (S 22); $script:bar.Style = 'Continuous'
$p4.Controls.Add($script:bar, 0, 0)
$script:lblPct = New-Label '0 %' $null $fontBold; $script:lblPct.Anchor = 'Left'
$p4.Controls.Add($script:lblPct, 1, 0)
$script:lblEta = New-Label '' ; $script:lblEta.Anchor = 'Left'
$p4.Controls.Add($script:lblEta, 2, 0)
$script:lblStatus = New-Label 'Looking for your game files...' $cDim
[void]$script:wrapLabels.Add(@($script:lblStatus, 'wide'))
$p4.Controls.Add($script:lblStatus, 0, 1); $p4.SetColumnSpan($script:lblStatus, 3)
$page.Controls.Add($p4, 0, 4)

# row 5: error
$script:lblErr = New-Label '' $cFail $fontBold
[void]$script:wrapLabels.Add(@($script:lblErr, 'wide')); $script:lblErr.Visible = $false
$page.Controls.Add($script:lblErr, 0, 5)

# row 6: log
$script:logBox = New-Object System.Windows.Forms.TextBox
$script:logBox.Multiline = $true; $script:logBox.ReadOnly = $true; $script:logBox.ScrollBars = 'Both'; $script:logBox.WordWrap = $false
$script:logBox.MinimumSize = New-Object System.Drawing.Size(0, (S 130)); $script:logBox.Font = $fontMono; $script:logBox.Dock = 'Fill'; $script:logBox.BackColor = $cBgField; $script:logBox.ForeColor = $cFg
$script:logBox.MaxLength = 0
$page.Controls.Add($script:logBox, 0, 6)

# row 7: buttons
$pBtn = New-Object System.Windows.Forms.TableLayoutPanel
$pBtn.Dock = 'Fill'; $pBtn.AutoSize = $true; $pBtn.ColumnCount = 2; $pBtn.RowCount = 1
[void]$pBtn.ColumnStyles.Add((New-Object System.Windows.Forms.ColumnStyle([System.Windows.Forms.SizeType]::Percent, 100)))
[void]$pBtn.ColumnStyles.Add((New-Object System.Windows.Forms.ColumnStyle([System.Windows.Forms.SizeType]::AutoSize)))
$fL = New-Object System.Windows.Forms.FlowLayoutPanel; $fL.AutoSize = $true; $fL.Dock = 'Fill'; $fL.WrapContents = $false
$fR = New-Object System.Windows.Forms.FlowLayoutPanel; $fR.AutoSize = $true; $fR.Dock = 'Fill'; $fR.WrapContents = $false; $fR.FlowDirection = 'RightToLeft'
$script:btnClose = New-Button 'Close' 90
$script:btnPlay = New-Button 'Play' 90
$script:btnInstall = New-Button 'Install' 110
$script:btnInstall.Font = $fontBold
$script:btnDry = New-Button 'Dry run' 90
$script:btnUpdate = New-Button 'Update only' 100
$script:btnLogs = New-Button 'Open log folder' 120
$script:btnMsvc = New-Button 'Install Build Tools' 140      # shown only after a stop that needs the MSVC Build Tools
$script:btnMsvc.Visible = $false
[void]$fL.Controls.Add($script:btnLogs); [void]$fL.Controls.Add($script:btnUpdate); [void]$fL.Controls.Add($script:btnDry)
[void]$fL.Controls.Add($script:btnMsvc)
[void]$fR.Controls.Add($script:btnClose); [void]$fR.Controls.Add($script:btnPlay); [void]$fR.Controls.Add($script:btnInstall)
$pBtn.Controls.Add($fL, 0, 0); $pBtn.Controls.Add($fR, 1, 0)
$page.Controls.Add($pBtn, 0, 7)

# ---------------------------------------------------------------------------------------------------- helpers
function Log-Add([string]$line, [bool]$box = $true) {
    if ($box) { [void]$script:logPending.AppendLine($line) }
    [void]$script:filePending.AppendLine(('{0}  {1}' -f (Get-Date -Format 'HH:mm:ss'), $line))
}
function Flush-Log {
    if ($script:logPending.Length -gt 0) {
        $t = $script:logPending.ToString(); [void]$script:logPending.Clear()
        if ($script:logBox.TextLength -gt 400000) { $script:logBox.Text = $script:logBox.Text.Substring(200000) }
        $script:logBox.AppendText($t)
    }
    if ($script:filePending.Length -gt 0) {
        $t = $script:filePending.ToString(); [void]$script:filePending.Clear()
        if (-not $testMode) {
            try {
                [void][IO.Directory]::CreateDirectory($logDir)
                [IO.File]::AppendAllText($logFile, $t, (New-Object System.Text.UTF8Encoding($false)))
            } catch {}
        }
    }
}

function Set-Status([string]$text, $color = $null) {
    $script:lblStatus.Text = $text
    $script:lblStatus.ForeColor = $(if ($color) { $color } else { $cDim })
}

function Fmt-Dur([double]$s) {
    if ($s -lt 0) { $s = 0 }
    $t = [TimeSpan]::FromSeconds([Math]::Round($s))
    if ($t.TotalHours -ge 1) { return ('{0}:{1:00}:{2:00}' -f [int][Math]::Floor($t.TotalHours), $t.Minutes, $t.Seconds) }
    return ('{0}:{1:00}' -f [int]$t.TotalMinutes, $t.Seconds)
}
function Fmt-Left([double]$s) {
    if ($s -lt 60) { return 'less than a minute left' }
    if ($s -lt 5400) { return ('about {0} min left' -f [int][Math]::Round($s / 60)) }
    $h = [Math]::Floor($s / 3600); $m = [int][Math]::Round(($s - $h * 3600) / 60)
    if ($m -ge 60) { $h += 1; $m = 0 }
    return ('about {0} h {1} min left' -f [int]$h, $m)
}

function Strip-Desc($item) {      # find_inputs' sentence without the long path in it
    $d = [string]$item.desc; $p = [string]$item.path
    if ($p) {
        $what = $(if (Test-Path -LiteralPath $p -PathType Container -ErrorAction SilentlyContinue) { 'this folder' } else { 'this file' })
        $d = $d.Replace($p, $what)
    }
    $d = [regex]::Replace($d, '\s*\(for tools[^)]*\)', '')
    $d = [regex]::Replace($d, '^this (file|folder):\s*', '')
    return $d.Trim()
}
function Find-Item($list, [string]$path) {
    if (-not $list -or -not $path) { return $null }
    foreach ($it in @($list)) { if ([string]$it.path -ieq $path) { return $it } }
    return $null
}
function Is-Dir([string]$p) { return ($p -and (Test-Path -LiteralPath $p -PathType Container -ErrorAction SilentlyContinue)) }
function Is-File([string]$p) { return ($p -and (Test-Path -LiteralPath $p -PathType Leaf -ErrorAction SilentlyContinue)) }

function Set-FieldStatus([string]$key, [string]$text, $color) {
    $script:F[$key].Status.Text = $text
    $script:F[$key].Status.ForeColor = $color
}

function Update-FieldStatus {
    $fd = $script:found
    # base game or dump folder
    $v = $script:F['base'].Box.Text.Trim().Trim('"')
    if (-not $v) { Set-FieldStatus 'base' 'Required: choose the game file, or a folder you already extracted.' $cWarn }
    elseif (Is-Dir $v) {
        $it = if ($fd) { Find-Item $fd.dumps $v } else { $null }
        $ex = Test-Path -LiteralPath (Join-Path $v 'exefs'); $ro = Test-Path -LiteralPath (Join-Path $v 'romfs')
        $fnt = ''
        if (-not (Test-Path -LiteralPath (Join-Path $v 'sysdata\fonts')) -and -not $script:F['firmware'].Box.Text.Trim()) {
            $fnt = '  Note: no sysdata\fonts in this dump and no firmware chosen, so open-source fonts will be used.'
        }
        if ($it) { Set-FieldStatus 'base' ('Dump folder: ' + (Strip-Desc $it) + $fnt) $cOk }
        elseif ($ex -and $ro) { Set-FieldStatus 'base' ('Dump folder: exefs and romfs found.' + $fnt) $cOk }
        else { Set-FieldStatus 'base' 'This folder has no exefs\ and romfs\ inside: choose the game file instead, or the right folder.' $cFail }
    } elseif (Is-File $v) {
        $it = if ($fd) { Find-Item $fd.base $v } else { $null }
        if ($it) {
            $c = $(if ($it.supported -eq $false) { $cFail } else { $cOk })
            Set-FieldStatus 'base' (Strip-Desc $it) $c
        } else {
            $len = (Get-Item -LiteralPath $v).Length
            Set-FieldStatus 'base' ('{0}, {1:N2} GB (not checked yet; the installer reads it)' -f ([IO.Path]::GetExtension($v).ToLower()), ($len / 1GB)) $cInfo
        }
    } else { Set-FieldStatus 'base' 'This path does not exist.' $cFail }

    $v = $script:F['update'].Box.Text.Trim().Trim('"')
    if (-not $v) { Set-FieldStatus 'update' 'None: the game is built as 1.0.0 (the base game). Add the update 1.0.4 to play that version.' $cDim }
    elseif (Is-File $v) {
        $it = if ($fd) { Find-Item $fd.updates $v } else { $null }
        if ($it) { Set-FieldStatus 'update' (Strip-Desc $it) $(if ($it.supported -eq $false) { $cFail } else { $cOk }) }
        else { Set-FieldStatus 'update' 'File chosen (not checked yet; the installer reads it).' $cInfo }
    } else { Set-FieldStatus 'update' 'This file does not exist.' $cFail }

    $v = $script:F['prodkeys'].Box.Text.Trim().Trim('"')
    if (-not $v) { Set-FieldStatus 'prodkeys' 'Needed for package files (.nsp/.xci/.nsz); not for an extracted dump folder.' $cWarn }
    elseif (Is-File $v) {
        $it = if ($fd) { Find-Item $fd.keys $v } else { $null }
        if ($it) { Set-FieldStatus 'prodkeys' (Strip-Desc $it) $(if ($it.complete -eq $false) { $cWarn } else { $cOk }) }
        else { Set-FieldStatus 'prodkeys' 'File chosen (only the key names are looked at, never the values).' $cInfo }
    } else { Set-FieldStatus 'prodkeys' 'This file does not exist.' $cFail }

    $v = $script:F['firmware'].Box.Text.Trim().Trim('"')
    if (-not $v) { Set-FieldStatus 'firmware' 'Optional. Only the fonts change: without it open-source fonts are used (no Chinese / Japanese / Korean characters). The game is the same.' $cWarn }
    elseif (Is-Dir $v) {
        $it = if ($fd) { Find-Item $fd.firmware $v } else { $null }
        if ($it) { Set-FieldStatus 'firmware' (Strip-Desc $it) $cOk }
        else { Set-FieldStatus 'firmware' 'Folder chosen (not checked yet; the installer reads it).' $cInfo }
    } else { Set-FieldStatus 'firmware' 'This folder does not exist.' $cFail }

    $n = $script:dlcList.Items.Count
    if ($n -gt 0 -and (Is-Dir $script:F['base'].Box.Text.Trim().Trim('"'))) { $script:dlcStatus.Text = 'The base game is an extracted dump, so the DLC packages below are not used (choose a package file to use them).' }
    elseif ($n -eq 0) { $script:dlcStatus.Text = '' }
    else { $script:dlcStatus.Text = ('{0} DLC package(s) will be read. Optional: the game runs without them. Use Remove or Clear to leave some out.' -f $n) }
    $script:dlcStatus.ForeColor = $cDim
    Update-LocStatus
}

# ---- install location
function Full-Path([string]$p) { try { return [IO.Path]::GetFullPath($p).TrimEnd('\') } catch { return $p.TrimEnd('\') } }
function Same-Path([string]$a, [string]$b) { return ((Full-Path $a) -ieq (Full-Path $b)) }
function Get-ChosenBuild {
    $v = $script:F['buildloc'].Box.Text.Trim().Trim('"')
    if (-not $v) { return $defaultBuild }
    return (Full-Path ([Environment]::ExpandEnvironmentVariables($v)))
}
function Get-FreeGB([string]$p) {        # free space of the drive of $p (the folder need not exist yet); $null if unknown
    try {
        $root = [IO.Path]::GetPathRoot((Full-Path $p))
        if (-not $root -or $root.StartsWith('\\')) { return $null }
        $d = New-Object System.IO.DriveInfo($root)
        if (-not $d.IsReady) { return $null }
        return [double]$d.AvailableFreeSpace / 1GB
    } catch { return $null }
}
function Get-NeedGB {                     # -> @(GB, text): the same sum as precheck.py check_disk
    $loc = Get-ChosenBuild
    $b = $script:F['base'].Box.Text.Trim().Trim('"')
    $n = $script:buildGb; $parts = @(('build {0:N0} GB' -f $script:buildGb))
    $haveDump = (Is-Dir $b) -or (Test-Path -LiteralPath ([IO.Path]::Combine($loc, 'dump\exefs\main')))
    if (-not $haveDump) {
        $n += $script:dumpGb; $parts += ('unpacked game {0:N0} GB' -f $script:dumpGb)
        $t = 0.0
        foreach ($p in @($b, $script:F['update'].Box.Text.Trim().Trim('"'))) {
            if ($p -and $p.ToLower().EndsWith('.nsz') -and (Is-File $p)) { $t += (Get-Item -LiteralPath $p).Length }
        }
        if ($t -gt 0) { $tg = [Math]::Round($t * 1.25 / 1GB, 1); $n += $tg; $parts += ('{0:N0} GB for a while to decompress the .nsz' -f $tg) }
    }
    return @($n, ($parts -join ' + '))
}
function Update-LocStatus {
    if (-not $script:F.ContainsKey('buildloc')) { return }
    $v = $script:F['buildloc'].Box.Text.Trim().Trim('"')
    if ($v -and -not ($v -match '^[A-Za-z]:\\' -or $v.StartsWith('\\'))) {
        Set-FieldStatus 'buildloc' 'Give a full folder path, such as D:\Tomodachi (with the drive letter).' $cFail; return
    }
    $loc = Get-ChosenBuild
    if ((Test-Path -LiteralPath $loc -PathType Leaf)) { Set-FieldStatus 'buildloc' 'This is a file, not a folder.' $cFail; return }
    $need = Get-NeedGB
    $free = Get-FreeGB $loc
    $root = [IO.Path]::GetPathRoot($loc)
    if ($root -and -not $root.StartsWith('\\') -and -not [IO.Directory]::Exists($root)) {
        Set-FieldStatus 'buildloc' ('The drive {0} is not on this PC (or not connected): choose another folder.' -f $root) $cFail; return
    }
    $t = ''
    if ($null -eq $free) { $t = ('Free space of {0} unknown; about {1:N0} GB needed ({2}).' -f $root, $need[0], $need[1]); $c = $cWarn }
    else {
        $t = ('{0:N0} GB free on {1}, about {2:N0} GB needed ({3}).' -f $free, $root, $need[0], $need[1])
        if ($free -lt $need[0]) { $c = $cFail; $t += ' Not enough: choose a folder on another drive, or free some space.' }
        elseif ($free -lt $need[0] * 1.25) { $c = $cWarn; $t += ' It fits, but only just.' }
        else { $c = $cOk }
    }
    if (Same-Path $loc $defaultBuild) { $t += '  (The default: the build folder inside this program''s folder.)' }
    if ($storedBuild -and -not (Same-Path $loc $storedBuild) -and (Test-Path -LiteralPath ([IO.Path]::Combine($storedBuild, 'state.json')))) {
        $t += ('  Note: the last install is in {0}; a new location starts from the beginning there (the old folder is kept).' -f $storedBuild)
        if ($c -eq $cOk) { $c = $cInfo }
    } elseif ((Test-Path -LiteralPath ([IO.Path]::Combine($loc, 'state.json'))) -or (Test-Path -LiteralPath ([IO.Path]::Combine($loc, 'install_state.json')))) {
        $t += '  An install is already here: Resume goes on with it.'
    }
    Set-FieldStatus 'buildloc' $t $c
}
function Browse-Location {
    $d = New-Object System.Windows.Forms.FolderBrowserDialog
    $d.Description = 'Install location: a folder on a drive with about 15 GB free (the unpacked game and the build go there)'
    $d.ShowNewFolderButton = $true
    $cur = Get-ChosenBuild
    if (Is-Dir $cur) { $d.SelectedPath = $cur } elseif (Is-Dir (Split-Path -Parent $cur)) { $d.SelectedPath = (Split-Path -Parent $cur) }
    if ($d.ShowDialog($form) -eq 'OK') {
        $p = $d.SelectedPath
        # a drive root (D:\) gets its own folder: the build makes many folders and files there
        if ([IO.Path]::GetPathRoot($p) -ieq $p) { $p = Join-Path $p 'TomodachiLife-native' }
        $script:F['buildloc'].Box.Text = $p
    }
    $d.Dispose()
}

function Browse-File([string]$key, [string]$title, [string]$filter) {
    $d = New-Object System.Windows.Forms.OpenFileDialog
    $d.Title = $title; $d.Filter = $filter; $d.CheckFileExists = $true
    $cur = $script:F[$key].Box.Text.Trim().Trim('"')
    if ($cur -and (Is-File $cur)) { $d.InitialDirectory = Split-Path -Parent $cur }
    if ($d.ShowDialog($form) -eq 'OK') { $script:F[$key].Box.Text = $d.FileName }
    $d.Dispose()
}
function Browse-Folder([string]$key, [string]$title) {
    $d = New-Object System.Windows.Forms.FolderBrowserDialog
    $d.Description = $title; $d.ShowNewFolderButton = $false
    $cur = $script:F[$key].Box.Text.Trim().Trim('"')
    if ($cur -and (Is-Dir $cur)) { $d.SelectedPath = $cur }
    if ($d.ShowDialog($form) -eq 'OK') { $script:F[$key].Box.Text = $d.SelectedPath }
    $d.Dispose()
}
$script:btnDlcAdd.Add_Click({
    $d = New-Object System.Windows.Forms.OpenFileDialog
    $d.Title = 'DLC packages (.nsp, .nsz)'; $d.Filter = 'Switch DLC|*.nsp;*.nsz|All files|*.*'; $d.Multiselect = $true
    if ($d.ShowDialog($form) -eq 'OK') { foreach ($f in $d.FileNames) { if (-not $script:dlcList.Items.Contains($f)) { [void]$script:dlcList.Items.Add($f) } } }
    $d.Dispose(); Update-FieldStatus
})
$script:btnDlcDel.Add_Click({
    $sel = @($script:dlcList.SelectedItems | ForEach-Object { $_ })
    foreach ($s in $sel) { $script:dlcList.Items.Remove($s) }
    Update-FieldStatus
})
$script:btnDlcClr.Add_Click({ $script:dlcList.Items.Clear(); Update-FieldStatus })

# ---------------------------------------------------------------------------------------------------- steps
function Reset-Steps {
    $script:steps.Clear(); $script:stepIdx = @{}
    $script:stepList.Items.Clear()
    $script:jsonSeen = $false; $script:fallbackN = 0
    $script:errMsg = ''; $script:errHint = ''; $script:errAction = ''; $script:doneOk = $null; $script:doneMsg = ''
}
function Add-Step([string]$id, [string]$title, [double]$est) {
    if ($est -le 0) { $est = 10 }
    $s = @{ id = $id; title = $title; est = $est; state = 'wait'; secs = 0.0; done = 0.0; total = 0.0; estp = $false; t0 = $null; msg = ''; item = $null }
    $item = New-Object System.Windows.Forms.ListViewItem($title)
    [void]$item.SubItems.Add('waiting'); [void]$item.SubItems.Add(''); [void]$item.SubItems.Add('')
    $item.ImageKey = 'wait'
    $s.item = $item
    [void]$script:steps.Add($s)
    $script:stepIdx[$id] = $script:steps.Count - 1
    [void]$script:stepList.Items.Add($item)
    return $s
}
function Show-Step($s) {
    $it = $s.item
    $it.ImageKey = $s.state
    $it.SubItems[1].Text = $stateText[$s.state]
    $t = ''
    if ($s.state -eq 'run' -and $s.t0) { $t = Fmt-Dur (([DateTime]::Now - $s.t0).TotalSeconds) }
    elseif ($s.secs -gt 0) { $t = Fmt-Dur $s.secs }
    $it.SubItems[2].Text = $t
    $note = $s.msg
    if ($s.state -eq 'run' -and $s.total -gt 0 -and -not $s.estp) { $note = ('{0} / {1} files  {2}' -f [int]$s.done, [int]$s.total, $s.msg).Trim() }   # (estimated seconds: no numbers)
    $it.SubItems[3].Text = $note
    switch ($s.state) {
        'fail' { $it.ForeColor = $cFail }
        'skip' { $it.ForeColor = $cDim }
        'wait' { $it.ForeColor = $cDim }
        'run'  { $it.ForeColor = $cFg }
        default { $it.ForeColor = $cFg }
    }
}
function Get-StepById([string]$id) {
    if ($script:stepIdx.ContainsKey($id)) { return $script:steps[$script:stepIdx[$id]] }
    return $null
}
function Set-StepState($s, [string]$state, $secs, [string]$msg) {
    $s.state = $state
    if ($state -eq 'run') { if (-not $s.t0) { $s.t0 = [DateTime]::Now } }
    if ($null -ne $secs -and [double]$secs -gt 0) { $s.secs = [double]$secs }
    elseif (($state -eq 'done' -or $state -eq 'fail') -and $s.t0 -and $s.secs -le 0) { $s.secs = ([DateTime]::Now - $s.t0).TotalSeconds }
    if ($msg) { $s.msg = $msg }
    if ($state -eq 'done' -and $s.total -gt 0) { $s.done = $s.total }
    Show-Step $s
}

function Get-Frac($s) {
    if ($s.total -gt 0) { return [Math]::Min(1.0, [Math]::Max(0.0, $s.done / $s.total)) }
    if ($s.t0) { return [Math]::Min(0.95, ([DateTime]::Now - $s.t0).TotalSeconds / $s.est) }
    return 0.0
}

# overall fraction, time left, corrected by real/estimated of the steps already finished
function Get-Overall {
    $tot = 0.0; $done = 0.0; $rs = 0.0; $re = 0.0; $wait = 0.0; $cur = 0.0; $any = $false
    foreach ($s in $script:steps) {
        $tot += $s.est
        $st = $s.state
        if ($st -eq 'done' -or $st -eq 'skip' -or $st -eq 'dry') {
            $done += $s.est
            if ($st -eq 'done' -and $s.secs -gt 0) { $rs += $s.secs; $re += $s.est }
        } elseif ($st -eq 'run' -or $st -eq 'fail') {
            $f = Get-Frac $s; $done += $s.est * $f; $any = $true
        } else { $wait += $s.est }
    }
    $ratio = 1.0
    if ($re -gt 0) { $ratio = [Math]::Min(5.0, [Math]::Max(0.25, $rs / $re)) }
    $left = $wait * $ratio
    foreach ($s in $script:steps) {
        if ($s.state -ne 'run') { continue }
        $f = Get-Frac $s
        $el = $(if ($s.t0) { ([DateTime]::Now - $s.t0).TotalSeconds } else { 0 })
        if ($s.total -gt 0 -and $f -ge 0.03 -and $el -ge 15) { $left += $el * (1 - $f) / $f }
        else { $left += $s.est * (1 - $f) * $ratio }
    }
    $frac = $(if ($tot -gt 0) { [Math]::Min(1.0, $done / $tot) } else { 0.0 })
    return @{ Frac = $frac; Left = $left; Ratio = $ratio; Total = $tot }
}

function Update-Progress {
    $o = Get-Overall
    $v = [int][Math]::Round($o.Frac * 1000)
    if ($script:succeeded -and -not $script:jobDry -and $script:steps.Count -gt 0) { $v = 1000 }
    $script:bar.Value = [Math]::Max(0, [Math]::Min(1000, $v))
    $script:lblPct.Text = ('{0:N0} %' -f ($v / 10.0))
    if ($script:running) {
        $el = ([DateTime]::Now - $script:jobStart).TotalSeconds
        $eta = $(if ($script:steps.Count -gt 0) { Fmt-Left $o.Left } else { 'working...' })
        $script:lblEta.Text = ('elapsed {0}, {1}' -f (Fmt-Dur $el), $eta)
    } elseif ($script:lastExit -ne $null) {
        $script:lblEta.Text = ('took {0}' -f (Fmt-Dur (($script:jobEnd - $script:jobStart).TotalSeconds)))
    }
    foreach ($s in $script:steps) { if ($s.state -eq 'run') { Show-Step $s } }
}

# ---------------------------------------------------------------------------------------------------- events
function Set-Plan($list) {
    Reset-StepsOnly
    foreach ($p in @($list)) {
        $id = [string]$p.id; if (-not $id) { $id = 's' + $script:steps.Count }
        $title = [string]$p.title; if (-not $title) { $title = $id }
        $est = 0.0; if ($null -ne $p.est_s) { $est = [double]$p.est_s }
        [void](Add-Step $id $title $est)
    }
}
function Reset-StepsOnly {
    $script:steps.Clear(); $script:stepIdx = @{}; $script:stepList.Items.Clear()
}

function Handle-Event($e) {
    $script:jsonSeen = $true
    switch ([string]$e.ev) {
        'plan' { Set-Plan $e.steps }
        'step' {
            $s = Get-StepById ([string]$e.id)
            if (-not $s) { $t = [string]$e.id; $s = Add-Step $t $t 10 }
            $m = $(if ($e.msg) { [string]$e.msg } else { '' })
            $secs = $e.secs
            switch ([string]$e.state) {
                'start' { Set-StepState $s 'run' $null $m }
                'done'  { Set-StepState $s 'done' $secs $m }
                'skip'  { Set-StepState $s 'skip' $secs $m }
                'fail'  { Set-StepState $s 'fail' $secs $m; if ($m) { Log-Add ('[{0}] FAILED: {1}' -f $s.id, $m) } }
                'dry'   { Set-StepState $s 'dry' $null $m }
            }
        }
        'progress' {
            $s = Get-StepById ([string]$e.id)
            if ($s) {
                if ($null -ne $e.done) { $s.done = [double]$e.done }
                if ($null -ne $e.total) { $s.total = [double]$e.total }
                $s.estp = [bool]$e.est
                if ($s.state -eq 'wait') { Set-StepState $s 'run' $null '' }
                if ($e.msg) { $s.msg = [string]$e.msg }
            }
        }
        'done' {
            $script:doneOk = [bool]$e.ok
            $script:doneMsg = $(if ($e.msg) { [string]$e.msg } else { '' })
            if ($script:doneMsg) { Log-Add $script:doneMsg }
        }
        'error' {
            $script:errMsg = $(if ($e.msg) { [string]$e.msg } else { 'The installation stopped.' })
            $script:errHint = $(if ($e.hint) { [string]$e.hint } else { '' })
            $script:errAction = $(if ($e.action) { [string]$e.action } else { '' })
            Log-Add ('ERROR: ' + $script:errMsg)
            if ($script:errHint) { Log-Add ('  hint: ' + $script:errHint) }
        }
    }
}

# setup.py's lines when no plan came: "[6/11] lift ...", "[6/11] lift: ok  19.3s", "... would run", "... done earlier (x), skipped"
function Parse-SetupLine([string]$line) {
    $m = [regex]::Match($line, '^\[(\d+)/(\d+)\]\s+(\w+)\s*(.*)$')
    if (-not $m.Success) { return $false }
    $i = [int]$m.Groups[1].Value; $n = [int]$m.Groups[2].Value; $name = $m.Groups[3].Value; $rest = $m.Groups[4].Value.Trim()
    if ($script:steps.Count -eq 0 -or $script:fallbackN -ne $n) {
        Reset-StepsOnly; $script:fallbackN = $n
        for ($k = 1; $k -le $n; $k++) { [void](Add-Step ('s' + $k) ('step ' + $k) 30) }
    }
    $s = $script:steps[$i - 1]
    $s.title = $name; $s.item.Text = $name
    if ($script:fallbackWeights.ContainsKey($name)) { $s.est = 30.0 * $script:fallbackWeights[$name] }
    $secs = $null
    $ms = [regex]::Match($rest, '(\d+)m(\d+)s|([\d.]+)s')
    if ($ms.Success) { if ($ms.Groups[1].Success) { $secs = [double]$ms.Groups[1].Value * 60 + [double]$ms.Groups[2].Value } else { $secs = [double]::Parse($ms.Groups[3].Value, [Globalization.CultureInfo]::InvariantCulture) } }
    if ($rest -match '^:\s*ok') { Set-StepState $s 'done' $secs '' }
    elseif ($rest -match 'would run') { Set-StepState $s 'dry' $null '' }
    elseif ($rest -match 'skipped') { Set-StepState $s 'skip' $null 'done earlier' }
    elseif ($rest -match '^\.\.\.') { Set-StepState $s 'run' $null '' }
    else { return $false }
    return $true
}

function Handle-Line([string]$line) {
    if ($line.StartsWith('##ssbu ')) {
        $json = $line.Substring(7)
        Log-Add ('event ' + $json) $false
        try { $e = $json | ConvertFrom-Json } catch { Log-Add ('(unreadable event) ' + $json); return }
        Handle-Event $e
        return
    }
    Log-Add $line
    if (-not $script:jsonSeen) {
        if (Parse-SetupLine $line) { }
        elseif ($line -match '^FAILED at step (\w+)') {
            foreach ($s in $script:steps) { if ($s.state -eq 'run') { Set-StepState $s 'fail' $null '' } }
            if (-not $script:errMsg) { $script:errMsg = $line }
        }
    }
    if ($line.Length -gt 0 -and -not $line.StartsWith('##')) { Set-Status $line $null }
}

# ---------------------------------------------------------------------------------------------------- job control
function Set-Buttons {
    $run = $script:running
    $ok = [bool]$script:py
    $script:btnInstall.Enabled = (-not $run) -and $ok
    $script:btnDry.Enabled = (-not $run) -and $ok
    $script:btnUpdate.Enabled = (-not $run) -and $ok
    foreach ($k in $script:F.Keys) { $script:F[$k].Box.Enabled = -not $run; foreach ($b in $script:F[$k].Buttons) { $b.Enabled = -not $run } }
    $script:dlcList.Enabled = -not $run; $script:btnDlcAdd.Enabled = -not $run; $script:btnDlcDel.Enabled = -not $run; $script:btnDlcClr.Enabled = -not $run
    foreach ($c in @($script:cbShort, $script:cbUpd, $script:cbRuntime, $script:cbFast, $script:cbDry, $script:numJobs)) { $c.Enabled = -not $run }
    $script:btnMsvc.Enabled = -not $run
    $loc = Get-ChosenBuild                # Play and Resume follow the install location shown in the window
    $script:btnPlay.Enabled = (-not $run) -and (Test-Path -LiteralPath ([IO.Path]::Combine($loc, 'work\tomodachi_native.exe'))) -and (Same-Path $loc $buildDir)
    $hasState = (Test-Path -LiteralPath ([IO.Path]::Combine($loc, 'state.json'))) -or (Test-Path -LiteralPath ([IO.Path]::Combine($loc, 'install_state.json')))
    if ($run) { $script:btnInstall.Text = 'Working...' }
    elseif ($script:failedBefore -or $hasState) { $script:btnInstall.Text = 'Resume' }
    elseif ($script:succeeded) { $script:btnInstall.Text = 'Install again' }
    else { $script:btnInstall.Text = 'Install' }
}

function Build-InstallArgs([bool]$forceDry) {
    $a = New-Object System.Collections.ArrayList
    [void]$a.Add('-u'); [void]$a.Add((Q (Join-Path $repo 'scripts\install.py'))); [void]$a.Add('--no-gui'); [void]$a.Add('--progress-json')
    $b = $script:F['base'].Box.Text.Trim().Trim('"')
    $isDump = Is-Dir $b
    if ($b) { if ($isDump) { [void]$a.Add('--dump'); [void]$a.Add((Q $b.TrimEnd('\'))) } else { [void]$a.Add('--base'); [void]$a.Add((Q $b)) } }
    $u = $script:F['update'].Box.Text.Trim().Trim('"')
    if ($u -and -not $isDump) { [void]$a.Add('--update'); [void]$a.Add((Q $u)) }
    if (-not $isDump) { foreach ($d in $script:dlcList.Items) { [void]$a.Add('--dlc'); [void]$a.Add((Q ([string]$d))) } }
    $k = $script:F['prodkeys'].Box.Text.Trim().Trim('"')
    if ($k -and -not $isDump) { [void]$a.Add('--keys'); [void]$a.Add((Q $k)) }
    $f = $script:F['firmware'].Box.Text.Trim().Trim('"')
    if ($f) { [void]$a.Add('--firmware'); [void]$a.Add((Q $f.TrimEnd('\'))) }
    # the install location: passed when it is not the default, or to go back to the default (install.py then forgets the
    # old one); with nothing chosen the command line is the same as before
    $loc = Get-ChosenBuild
    if (-not (Same-Path $loc $defaultBuild) -or $storedBuild) { [void]$a.Add('--build-dir'); [void]$a.Add((Q $loc)) }
    [void]$a.Add('--jobs'); [void]$a.Add([string][int]$script:numJobs.Value)
    if ($forceDry -or $script:cbDry.Checked) { [void]$a.Add('--dry-run') }
    if (-not $script:cbRuntime.Checked) { [void]$a.Add('--no-runtime') }
    if (-not $script:cbShort.Checked) { [void]$a.Add('--no-shortcuts') }
    if ($script:cbFast.Checked) { [void]$a.Add('--no-inline') }
    if (-not $script:cbUpd.Checked) { [void]$a.Add('--no-self-update') }
    return ($a -join ' ')
}

function Begin-Job([string]$kind, [bool]$dry) {
    $script:jobKind = $kind; $script:jobDry = $dry
    $script:running = $true; $script:succeeded = $false; $script:lastExit = $null
    $script:jobStart = [DateTime]::Now
    Reset-Steps
    $script:lblErr.Visible = $false; $script:lblErr.Text = ''
    $script:btnMsvc.Visible = $false
    $script:bar.Value = 0; $script:lblPct.Text = '0 %'
    Set-Buttons
}

function Start-Job([string]$kind, [bool]$dry) {
    if (-not $script:py) { return }
    if ($script:running) { return }
    $cmdArgs = ''
    if ($kind -eq 'install') {
        $b = $script:F['base'].Box.Text.Trim().Trim('"')
        $problems = @()
        if (-not $b) { $problems += 'the base game' } elseif (-not ((Is-File $b) -or (Is-Dir $b))) { $problems += 'a base game that exists' }
        if ($problems.Count -gt 0 -and -not $dry) {
            [void][System.Windows.Forms.MessageBox]::Show("Please choose $($problems -join ', ') first.", 'Tomodachi Life native - installer', 'OK', 'Information'); return
        }
        $isDump = Is-Dir $b
        $miss = @()
        if (-not $isDump) {
            if (-not $script:F['prodkeys'].Box.Text.Trim()) { $miss += 'prod.keys' }
        }
        if ($miss.Count -gt 0 -and -not $dry) {
            $r = [System.Windows.Forms.MessageBox]::Show("Not chosen: $($miss -join ', ').`n`nThe installer may stop at the step that needs it (you can add it and press Resume). Start anyway?", 'Tomodachi Life native - installer', 'YesNo', 'Question')
            if ($r -ne 'Yes') { return }
        }
        $lv = $script:F['buildloc'].Box.Text.Trim().Trim('"')
        if ($lv -and -not ($lv -match '^[A-Za-z]:\\' -or $lv.StartsWith('\\'))) {
            [void][System.Windows.Forms.MessageBox]::Show('The install location must be a full folder path, such as D:\Tomodachi.', 'Tomodachi Life native - installer', 'OK', 'Information'); return
        }
        $loc = Get-ChosenBuild
        $need = Get-NeedGB; $free = Get-FreeGB $loc
        if ($null -ne $free -and $free -lt $need[0] -and -not $dry) {
            $r = [System.Windows.Forms.MessageBox]::Show(("The install location {0} has {1:N0} GB free; about {2:N0} GB are needed ({3}).`n`nChoose a folder on another drive (Install location, Choose...), or free some space. Start anyway?" -f $loc, $free, $need[0], $need[1]), 'Tomodachi Life native - installer', 'YesNo', 'Warning')
            if ($r -ne 'Yes') { return }
        }
        if ($storedBuild -and -not (Same-Path $loc $storedBuild) -and (Test-Path -LiteralPath ([IO.Path]::Combine($storedBuild, 'state.json'))) -and -not $dry) {
            $r = [System.Windows.Forms.MessageBox]::Show(("The last install is in {0}.`n`nWith the new location {1} the installation starts from the beginning there (the game files are unpacked and built again); the old folder is not deleted, you can delete it yourself afterwards.`n`nUse the new location?" -f $storedBuild, $loc), 'Tomodachi Life native - installer', 'YesNo', 'Question')
            if ($r -ne 'Yes') { return }
        }
        $fail = @($script:preResults | Where-Object { $_.level -eq 'fail' -and -not ($_.what -eq 'Disk space' -and $null -ne $free) })   # (the disk was asked about just above)
        if ($fail.Count -gt 0 -and -not $dry) {
            $r = [System.Windows.Forms.MessageBox]::Show("The PC check found a problem:`n`n" + (($fail | ForEach-Object { $_.what + ': ' + $_.msg }) -join "`n") + "`n`nStart anyway?", 'Tomodachi Life native - installer', 'YesNo', 'Warning')
            if ($r -ne 'Yes') { return }
        }
        $cmdArgs = Build-InstallArgs $dry
        if ($cmdArgs.Length -gt 30000) {
            [void][System.Windows.Forms.MessageBox]::Show('The list of DLC files is too long for one command line (Windows allows about 32000 characters). Remove some DLC packages (Remove / Clear) and try again.', 'Tomodachi Life native - installer', 'OK', 'Warning'); return
        }
        $exeArgs = $cmdArgs
    } else {
        $exeArgs = '-u ' + (Q (Join-Path $repo 'scripts\update.py')) + $(if ($dry -or $script:cbDry.Checked) { ' --dry-run' } else { '' })
        $dry = $dry -or $script:cbDry.Checked
    }
    Begin-Job $kind $dry
    if ($kind -eq 'update') { [void](Add-Step 'update' 'Update from GitHub' 15); [void]$null }
    Log-Add ('=== {0} {1}: {2} {3}' -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'), $kind, $script:py, $exeArgs) $false
    Log-Add ('Starting ' + $kind + $(if ($dry) { ' (dry run)' } else { '' }) + ' ...')
    $script:job = New-Object SsbuProc($script:q, $false)
    if (-not $script:job.Start($script:py, $exeArgs, $repo)) {
        Log-Add ('Could not start Python: ' + $script:job.StartError)
        $script:errMsg = 'Could not start Python: ' + $script:job.StartError
    } elseif ($kind -eq 'update') {
        Set-StepState $script:steps[0] 'run' $null ''
    }
    Set-Status 'Working... this can take from 30 minutes to a few hours. If you close the window, the next start goes on from where it stopped.' $null
}

function Finish-Job([int]$code) {
    $script:running = $false
    $script:jobEnd = [DateTime]::Now
    $script:lastExit = $code
    $ok = ($code -eq 0) -and ($script:doneOk -ne $false) -and (-not $script:errMsg)
    if ($ok) {
        foreach ($s in $script:steps) {
            if ($s.state -eq 'run') { Set-StepState $s 'done' $null '' }
            elseif ($s.state -eq 'wait') { Set-StepState $s $(if ($script:jobDry) { 'dry' } else { 'skip' }) $null '' }
        }
        $script:succeeded = $true; $script:failedBefore = $false
        $msg = $(if ($script:doneMsg) { $script:doneMsg } elseif ($script:jobDry) { 'Dry run finished: nothing was changed.' }
                 elseif ($script:jobKind -eq 'update') { 'Update finished.' } else { 'Done. Press Play (or start "Tomodachi Life.exe").' })
        Set-Status $msg $cOk
        $script:lblErr.Visible = $false
    } else {
        foreach ($s in $script:steps) { if ($s.state -eq 'run') { Set-StepState $s 'fail' $null '' } }
        $script:failedBefore = ($script:jobKind -eq 'install')
        $m = $(if ($script:errMsg) { $script:errMsg } else { "The installation stopped (code $code). Look at the log below." })
        $h = $(if ($script:errHint) { $script:errHint } else { 'Fix the problem, then press ' + $(if ($script:jobKind -eq 'install') { 'Resume' } else { 'the button again' }) + ': finished steps are not repeated.' })
        $script:lblErr.Text = $m + "`r`n" + $h
        $script:lblErr.Visible = $true
        $script:btnMsvc.Visible = ($script:errAction -eq 'msvc')
        Set-Status 'Stopped.' $cFail
    }
    if ($script:jobKind -eq 'install' -and -not $script:jobDry) {      # install.py remembered the install location
        $script:storedBuild = Get-StoredBuild
        $script:buildDir = $(if ($script:storedBuild) { $script:storedBuild } else { $defaultBuild })
        Update-LocStatus
    }
    Update-Progress
    Set-Buttons
    Log-Add ('=== finished, exit code {0}, {1}' -f $code, $(if ($ok) { 'ok' } else { 'not ok' })) $false
}

# ---------------------------------------------------------------------------------------------------- scans
$script:preResults = @()
function Start-Scans {
    if (-not $script:py) {
        Set-Status 'Python was not found (deps\python\python.exe or build\python_path.txt). Run scripts\bootstrap.ps1 again.' $cFail
        foreach ($k in $script:F.Keys) { $script:F[$k].Status.Text = 'No Python: nothing could be searched.' }
        Set-Buttons; return
    }
    $script:scanFind = New-Object SsbuProc($script:q, $true)
    [void]$script:scanFind.Start($script:py, ('-u ' + (Q (Join-Path $repo 'scripts\find_inputs.py')) + ' --json'), $repo)
    Start-Precheck
}
function Start-Precheck {
    if (-not $script:py) { return }
    $script:preDone = $false
    $script:scanPre = New-Object SsbuProc($script:q, $true)
    $a = '-u ' + (Q (Join-Path $repo 'scripts\precheck.py')) + ' --json' + $(if (-not $script:cbRuntime.Checked) { ' --need-msvc' } else { '' })
    $a += ' --build ' + (Q (Get-ChosenBuild))          # the disk check looks at the drive of the install location
    $b = $script:F['base'].Box.Text.Trim().Trim('"')
    if (Is-Dir $b) { $a += ' --dump ' + (Q $b.TrimEnd('\')) }
    else { foreach ($g in @($b, $script:F['update'].Box.Text.Trim().Trim('"'))) { if (Is-File $g) { $a += ' --game ' + (Q $g) } } }
    [void]$script:scanPre.Start($script:py, $a, $repo)
}
$script:cbRuntime.Add_CheckedChanged({ if (-not $script:running -and $script:preDone) { Start-Precheck } })

# the MSVC Build Tools in a console the user can see (winget's own progress and Microsoft's installer window)
$script:btnMsvc.Add_Click({
    $cmd = 'winget install --id Microsoft.VisualStudio.2022.BuildTools -e --accept-package-agreements --accept-source-agreements --override "--passive --wait --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"'
    if (-not (Get-Command winget -ErrorAction SilentlyContinue)) {
        [void][System.Windows.Forms.MessageBox]::Show('winget is not on this PC. Install "Build Tools for Visual Studio" (workload "Desktop development with C++") from https://aka.ms/vs/17/release/vs_BuildTools.exe , then press Resume.', 'Tomodachi Life native - installer', 'OK', 'Information')
        return
    }
    $r = [System.Windows.Forms.MessageBox]::Show("Install the Microsoft C++ Build Tools now?`n`n- free, about 3 GB on C:, 5-15 minutes`n- Yes accepts the Microsoft licence terms of the Build Tools and of the winget source`n- Windows asks for administrator rights; Microsoft's installer shows its progress`n`nA console window opens and shows what happens; when it says it is finished, close it and press Resume here.`n`nCommand: $cmd", 'Tomodachi Life native - installer', 'YesNo', 'Question')
    if ($r -ne 'Yes') { return }
    try {
        Start-Process -FilePath 'cmd.exe' -ArgumentList ('/k echo Installing the Microsoft C++ Build Tools (5-15 minutes; a Microsoft window shows the progress)... & ' + $cmd + ' & echo. & echo Finished: read the lines above. Close this window and press Resume in the installer.')
        Log-Add ('started in a console: ' + $cmd)
    } catch { [void][System.Windows.Forms.MessageBox]::Show('Could not start the console: ' + $_.Exception.Message, 'Tomodachi Life native - installer', 'OK', 'Warning') }
})

function Apply-Found {
    $raw = $script:scanFind.GetOutput()
    $r = $null
    try { $i = $raw.IndexOf('{'); if ($i -ge 0) { $r = $raw.Substring($i) | ConvertFrom-Json } } catch { $r = $null }
    if (-not $r) {
        $err = $script:scanFind.GetErrorText()
        Set-Status ('Searching for your game files did not work (code {0}). You can still choose the files yourself. {1}' -f $script:scanFind.ExitCode, ($err -split "`n" | Select-Object -Last 1)) $cWarn
        Log-Add ('find_inputs failed: ' + $err)
        Update-FieldStatus; return
    }
    $script:found = $r
    $b = $r.best
    $script:suppressStatus = $true
    try {
        $note = ''
        if (-not $script:F['base'].Box.Text) {
            if ($b.dump) { $script:F['base'].Box.Text = [string]$b.dump; if ($b.base) { $note = $b.base } }
            elseif ($b.base) { $script:F['base'].Box.Text = [string]$b.base }
        }
        if (-not $script:F['update'].Box.Text -and $b.update) { $script:F['update'].Box.Text = [string]$b.update }
        if (-not $script:F['prodkeys'].Box.Text -and $b.prod_keys) { $script:F['prodkeys'].Box.Text = [string]$b.prod_keys }
        if (-not $script:F['firmware'].Box.Text -and $b.firmware) { $script:F['firmware'].Box.Text = [string]$b.firmware }
        if ($script:dlcList.Items.Count -eq 0 -and $b.dlc) { foreach ($d in @($b.dlc)) { [void]$script:dlcList.Items.Add([string]$d) } }
    } finally { $script:suppressStatus = $false }
    Update-FieldStatus
    if ($note -and (Is-Dir $script:F['base'].Box.Text)) {
        $script:F['base'].Status.Text += ('  Your package ' + (Split-Path -Leaf $note) + ' was found too: press File... to use it instead.')
    }
    $n = 0; foreach ($k in 'base', 'update', 'prodkeys', 'firmware') { if ($script:F[$k].Box.Text) { $n++ } }
    Set-Status ('Search finished ({0:N1} s): {1} of 4 file fields filled in. Check them, then press Install.' -f [double]$r.seconds, $n) $null
    foreach ($nt in @($r.notes)) { if ($nt) { Log-Add ('note: ' + $nt) } }
    # the PC check again with the game files found (a dump folder or .nsz packages change the disk space needed)
    if ($script:preDone) { Start-Precheck } else { $script:preAgain = $true }
}

# (Windows PowerShell 5.1 hands a top-level JSON array over as ONE object, hence the extra variable below.)
function Apply-Precheck {
    $raw = $script:scanPre.GetOutput()
    $res = $null
    try { $i = $raw.IndexOf('['); if ($i -ge 0) { $one = $raw.Substring($i) | ConvertFrom-Json; $res = @($one) } } catch { $res = $null }
    $script:preList.Items.Clear()
    if (-not $res) {
        $it = New-Object System.Windows.Forms.ListViewItem('WARN'); [void]$it.SubItems.Add('Checks'); [void]$it.SubItems.Add('the PC check did not run (code ' + $script:scanPre.ExitCode + ')')
        $it.ForeColor = $cWarn; [void]$script:preList.Items.Add($it); return
    }
    $script:preResults = $res
    foreach ($c in $res) {
        $lv = [string]$c.level
        $it = New-Object System.Windows.Forms.ListViewItem($lv.ToUpper())
        [void]$it.SubItems.Add([string]$c.what); [void]$it.SubItems.Add([string]$c.msg)
        if ($lv -eq 'ok') { $it.ForeColor = $cOk } elseif ($lv -eq 'warn') { $it.ForeColor = $cWarn } else { $it.ForeColor = $cFail }
        [void]$script:preList.Items.Add($it)
        if ($c.what -eq 'Disk space' -and $null -ne $c.build_gb) {      # the sizes the "Install location" status adds up
            $script:buildGb = [double]$c.build_gb; $script:dumpGb = [double]$c.dump_gb; Update-LocStatus
        }
        if ($c.what -eq 'Memory' -and ([string]$c.msg) -match '--jobs (\d+)') {
            $j = [int]$Matches[1]; if ($j -ge 1 -and $j -le 64) { $script:numJobs.Value = $j
                $script:lblJobs.Text = ('(safe here: 1 to {0})' -f $j) }
        }
    }
}

# ---------------------------------------------------------------------------------------------------- self test
function Fake-Feed([string]$kind) {
    $ev = { param($o) '##ssbu ' + ($o | ConvertTo-Json -Compress -Depth 5) }
    $plan = @{ ev = 'plan'; steps = @(
        @{ id = 'update'; title = 'Check for updates'; est_s = 5 },
        @{ id = 'deps'; title = 'Python packages'; est_s = 10 },
        @{ id = 'extract'; title = 'Extract the game files'; est_s = 600 },
        @{ id = 'compile'; title = 'Compile the game'; est_s = 1800 },
        @{ id = 'link'; title = 'Link and check'; est_s = 30 }) }
    $f = New-Object System.Collections.ArrayList
    function A([int]$t, [string]$l) { [void]$f.Add(@{ t = $t; line = $l }) }
    if ($kind -eq 'fallback') {
        A 100 'Python 3.13 | requirements ok'
        A 200 '[1/11] deps ...'
        A 300 '[1/11] deps: ok  1.2s'
        A 400 '[2/11] stage ...'
        A 500 '[2/11] stage: ok  0.5s'
        A 600 '[3/11] convert: done earlier (3m02s), skipped'
        A 700 '[4/9] tables ...'
        A 800 '[4/9] tables: ok  4.0s'
        A 900 '[5/11] tables ...'
        A 1000 '[5/11] tables: ok  12.4s'
        A 1100 '[6/11] lift ...'
        A 1300 '[6/11] lift: ok  19.3s'
        A 1400 '[7/11] sdk ...'
        A 1500 'some plain compiler line'
        A 1600 '@SNAP'
        A 1700 '@END 0'
        return $f
    }
    A 100 'Tomodachi Life native installer (self-test, no python is started)'
    A 200 (& $ev $plan)
    A 300 (& $ev @{ ev = 'step'; id = 'update'; state = 'start' })
    A 400 'Checking https://github.com/... (fake line)'
    A 500 (& $ev @{ ev = 'step'; id = 'update'; state = 'done'; secs = 4.0; msg = 'up to date' })
    A 600 (& $ev @{ ev = 'step'; id = 'deps'; state = 'skip'; msg = 'done earlier' })
    A 700 (& $ev @{ ev = 'step'; id = 'extract'; state = 'start' })
    A 800 (& $ev @{ ev = 'progress'; id = 'extract'; done = 5; total = 20 })
    A 900 (& $ev @{ ev = 'step'; id = 'extract'; state = 'done'; secs = 310.0 })
    A 1000 (& $ev @{ ev = 'step'; id = 'compile'; state = 'start' })
    A 1100 'plain compiler line 1'
    A 1200 '##ssbu {broken json'
    A 1300 (& $ev @{ ev = 'progress'; id = 'compile'; done = 120; total = 1162 })
    A 1400 '@SNAP'
    if ($kind -eq 'error') {
        A 1600 (& $ev @{ ev = 'step'; id = 'compile'; state = 'fail'; msg = 'clang exited with 1' })
        A 1700 (& $ev @{ ev = 'error'; msg = 'The compiler failed on lift_0123.c'; hint = 'Free some disk space, then press Resume.'; action = 'msvc' })
        A 1800 '@END 1'
    } else {
        A 1600 (& $ev @{ ev = 'progress'; id = 'compile'; done = 1162; total = 1162 })
        A 1700 (& $ev @{ ev = 'step'; id = 'compile'; state = 'done'; secs = 900.0 })
        A 1800 (& $ev @{ ev = 'step'; id = 'link'; state = 'start' })
        A 1900 (& $ev @{ ev = 'step'; id = 'link'; state = 'done'; secs = 15.0 })
        A 2000 (& $ev @{ ev = 'done'; ok = $true; msg = 'All done. You can play now.' })
        A 2100 '@END 0'
    }
    return $f
}

function Take-Shot {
    if (-not $Shot) { return }
    try {
        $bmp = New-Object System.Drawing.Bitmap($form.Width, $form.Height)
        $form.DrawToBitmap($bmp, (New-Object System.Drawing.Rectangle(0, 0, $form.Width, $form.Height)))
        $bmp.Save($Shot, [System.Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose()
    } catch { [void]$script:testDump.Add('screenshot failed: ' + $_.Exception.Message) }
}

function Snapshot([string]$tag) {
    [void]$script:testDump.Add("--- state: $tag")
    foreach ($s in $script:steps) {
        [void]$script:testDump.Add(('  step {0,-10} {1,-10} est={2,6} secs={3,6:N1} {4}' -f $s.id, $s.state, $s.est, $s.secs, $s.msg))
    }
    $o = Get-Overall
    [void]$script:testDump.Add(('  bar={0}/1000 pct="{1}" eta="{2}" ratio={3:N2}' -f $script:bar.Value, $script:lblPct.Text, $script:lblEta.Text, $o.Ratio))
    [void]$script:testDump.Add(('  status="{0}"' -f $script:lblStatus.Text))
    [void]$script:testDump.Add(('  error visible={0} text="{1}"' -f $script:lblErr.Visible, ($script:lblErr.Text -replace "`r`n", ' | ')))
    [void]$script:testDump.Add(('  buttons: install="{0}" enabled={1}; close enabled={2}; build-tools button visible={3}' -f $script:btnInstall.Text, $script:btnInstall.Enabled, $script:btnClose.Enabled, $script:btnMsvc.Visible))
}

# ---------------------------------------------------------------------------------------------------- timer
$script:fakeList = $null; $script:fakeNext = 0; $script:fakeStart = $null
$script:tickN = 0
$timer = New-Object System.Windows.Forms.Timer
$timer.Interval = 100
$timer.Add_Tick({
    $script:tickN++
    # test feeder: scripted lines go through the same queue and parser as the real process output
    if ($script:fakeList -ne $null -and $script:fakeNext -lt $script:fakeList.Count) {
        $ms = ([DateTime]::Now - $script:fakeStart).TotalMilliseconds
        while ($script:fakeNext -lt $script:fakeList.Count -and $script:fakeList[$script:fakeNext].t -le $ms) {
            $l = [string]$script:fakeList[$script:fakeNext].line; $script:fakeNext++
            if ($l -eq '@SNAP') { Flush-Queue; Update-Progress; Snapshot 'mid-run' }
            elseif ($l.StartsWith('@END ')) { $script:fakeDone = $true; $script:fakeCode = [int]$l.Substring(5) }
            else { $script:q.Enqueue($l) }
        }
    }
    Flush-Queue
    if ($script:running) {
        $isDone = $(if ($script:job) { $script:job.Done } else { $script:fakeDone })
        if ($isDone -and $script:q.IsEmpty) {
            $code = $(if ($script:job) { $script:job.ExitCode } else { $script:fakeCode })
            if ($script:job -and $script:job.StartError) { $code = 1 }
            Finish-Job $code
        }
    }
    if ($script:scanFind -and $script:scanFind.Done -and -not $script:foundDone) { $script:foundDone = $true; Apply-Found }
    if ($script:scanPre -and $script:scanPre.Done -and -not $script:preDone) {
        $script:preDone = $true; Apply-Precheck
        if ($script:preAgain) { $script:preAgain = $false; Start-Precheck }
    }
    if ($script:tickN % 2 -eq 0) { Update-Progress }
    # install location: the free space shown live; the PC check again 1.5 s after the folder was changed
    if ($script:tickN % 50 -eq 0 -and -not $script:running) { Update-LocStatus }
    if ($script:locChangedAt -and -not $script:running -and (([DateTime]::Now - $script:locChangedAt).TotalSeconds -ge 1.5)) {
        $script:locChangedAt = $null; Update-LocStatus; Set-Buttons
        if ($script:preDone -and -not $testMode) { Start-Precheck }
    }
    if ($script:tickN % 20 -eq 0 -and -not $script:running) { Set-Buttons }
    Flush-Log
    if ($NoRun -and $script:foundDone -and $script:preDone) {
        $script:noRunReady++
        if ($script:noRunReady -ge 3) { Dump-NoRun; Take-Shot; $form.Close() }
    }
})
$script:noRunReady = 0
function Flush-Queue {
    $n = 0; $line = $null
    while ($n -lt 400 -and $script:q.TryDequeue([ref]$line)) { $n++; Handle-Line $line }
}

# ---------------------------------------------------------------------------------------------------- buttons
$script:btnInstall.Add_Click({ Start-Job 'install' $false })
$script:btnDry.Add_Click({ Start-Job 'install' $true })
$script:btnUpdate.Add_Click({ Start-Job 'update' $false })
$script:btnLogs.Add_Click({
    $d = [IO.Path]::Combine((Get-ChosenBuild), 'logs')
    if (-not (Test-Path -LiteralPath $d)) { $d = $logDir }
    try { [void][IO.Directory]::CreateDirectory($d); Start-Process -FilePath 'explorer.exe' -ArgumentList (Q $d) } catch {}
})
$script:btnPlay.Add_Click({
    $run = Join-Path $repo 'scripts\run.py'      # the launcher of the install location (what Play Tomodachi Life.bat did)
    if ($script:py) { Start-Process -FilePath $script:py -ArgumentList (Q $run) -WorkingDirectory $repo -WindowStyle Minimized } else { [void][System.Windows.Forms.MessageBox]::Show('Python was not found: run scripts\bootstrap.ps1 again.', 'Tomodachi Life native - installer', 'OK', 'Warning') }
})
$script:btnClose.Add_Click({ $form.Close() })
$script:allowClose = $false
$form.Add_FormClosing({
    param($sender, $e)
    if ($script:running -and $script:job -and -not $script:job.Done -and -not $testMode) {
        $r = [System.Windows.Forms.MessageBox]::Show('The installation is running. Stop it?' + "`n`n" + '(The next start goes on from where it stopped.)', 'Tomodachi Life native - installer', 'YesNo', 'Question')
        if ($r -ne 'Yes') { $e.Cancel = $true; return }
        $script:job.Kill()            # only the process tree this window started, by its PID
    }
    if ($script:scanFind -and -not $script:scanFind.Done) { $script:scanFind.Kill() }
    if ($script:scanPre -and -not $script:scanPre.Done) { $script:scanPre.Kill() }
    $timer.Stop(); Flush-Log
})

function Dump-NoRun {
    [void]$script:testDump.Add("python: $($script:py)   (dpi scale $scale)")
    [void]$script:testDump.Add(('find_inputs: exit={0}  precheck: exit={1}' -f $script:scanFind.ExitCode, $script:scanPre.ExitCode))
    foreach ($k in 'base', 'update', 'prodkeys', 'firmware', 'buildloc') {
        [void]$script:testDump.Add(('{0,-9} = {1}' -f $k, $script:F[$k].Box.Text))
        [void]$script:testDump.Add(('{0,-9}   status: {1}' -f '', $script:F[$k].Status.Text))
    }
    [void]$script:testDump.Add(('dlc       = {0} item(s); {1}' -f $script:dlcList.Items.Count, $script:dlcStatus.Text))
    for ($i = 0; $i -lt $script:preList.Items.Count; $i++) { $it = $script:preList.Items[$i]; [void]$script:testDump.Add(('precheck  {0,-5} {1,-20} {2}' -f $it.Text, $it.SubItems[1].Text, $it.SubItems[2].Text)) }
    [void]$script:testDump.Add('jobs spinner = ' + $script:numJobs.Value)
    [void]$script:testDump.Add('status: ' + $script:lblStatus.Text)
    [void]$script:testDump.Add('install command line would be: python ' + (Build-InstallArgs $false).Substring(0, [Math]::Min(700, (Build-InstallArgs $false).Length)) + ' ...')
    [void]$script:testDump.Add('buttons: install="' + $script:btnInstall.Text + '" play enabled=' + $script:btnPlay.Enabled)
}

# ---------------------------------------------------------------------------------------------------- start
$form.Add_Shown({
    Fit-Labels
    Set-Buttons
    if ($SelfTest -le 0) { Start-Scans }     # the self test starts no process at all
    $timer.Start()
    if ($SelfTest -gt 0) {
        Begin-Job 'install' $false
        $script:job = $null; $script:fakeDone = $false
        $script:fakeList = Fake-Feed $SelfTestKind; $script:fakeNext = 0; $script:fakeStart = [DateTime]::Now
        $script:closer = New-Object System.Windows.Forms.Timer
        $script:closer.Interval = [Math]::Max(1000, $SelfTest * 1000)
        $script:closer.Add_Tick({
            $script:closer.Stop(); Flush-Queue; Update-Progress; Snapshot 'final (just before closing)'; Take-Shot
            $form.Close()
        })
        $script:closer.Start()
    }
})
if (-not $testMode) {
    try { [void][IO.Directory]::CreateDirectory($logDir) } catch {}
}
$script:btnPlay.Enabled = $false
Update-FieldStatus
[System.Windows.Forms.Application]::add_ThreadException({ param($s, $e) Log-Add ('window error: ' + $e.Exception.Message) })
function Set-DarkTheme($c) {
    # every control once, before the window shows: fields and lists a shade lighter than the background, flat buttons;
    # a label keeps a colour of its own (status lines: green / orange / red / dim)
    $t = $c.GetType().Name
    if ($t -in 'TextBox', 'ListView', 'ListBox', 'RichTextBox', 'NumericUpDown', 'ComboBox') {
        $c.BackColor = $cBgField; $c.ForeColor = $cFg
        if ($t -eq 'ListView') {    # the column headers are drawn by Windows in light colours: drawn here instead
            $c.OwnerDraw = $true
            $c.Add_DrawColumnHeader({ param($s, $e)
                $e.Graphics.FillRectangle((New-Object System.Drawing.SolidBrush($cBgButton)), $e.Bounds)
                [System.Windows.Forms.TextRenderer]::DrawText($e.Graphics, $e.Header.Text, $s.Font, $e.Bounds, $cFg, [System.Windows.Forms.TextFormatFlags]::VerticalCenter -bor [System.Windows.Forms.TextFormatFlags]::Left) })
            $c.Add_DrawItem({ param($s, $e) $e.DrawDefault = $true })
            $c.Add_DrawSubItem({ param($s, $e) $e.DrawDefault = $true })
        }
    }
    elseif ($t -eq 'GroupBox') { $c.BackColor = $cBg; $c.ForeColor = $cFg }
    elseif ($t -eq 'Button') { $c.FlatStyle = 'Flat'; $c.BackColor = $cBgButton; $c.ForeColor = $cFg; $c.FlatAppearance.BorderColor = [System.Drawing.Color]::FromArgb(90, 90, 96) }
    else {
        $c.BackColor = $cBg
        if ($c.ForeColor.ToArgb() -eq [System.Drawing.SystemColors]::ControlText.ToArgb() -or $c.ForeColor.ToArgb() -eq [System.Drawing.SystemColors]::WindowText.ToArgb()) { $c.ForeColor = $cFg }
    }
    foreach ($k in $c.Controls) { Set-DarkTheme $k }
}
if ($script:dark) {
    Set-DarkTheme $form
    try {    # the title bar too (Windows 10 2004+ / 11: DWMWA_USE_IMMERSIVE_DARK_MODE)
        Add-Type -Namespace SsbuInst -Name Dwm -MemberDefinition '[DllImport("dwmapi.dll")] public static extern int DwmSetWindowAttribute(IntPtr h, int a, ref int v, int s);' -ErrorAction Stop
        $form.Add_HandleCreated({ $one = 1; [void][SsbuInst.Dwm]::DwmSetWindowAttribute($form.Handle, 20, [ref]$one, 4) })
    } catch {}
}
if ($env:SSBU_INSTALLER_SNAPSHOT) {      # test only: a picture of the window 3 s after it shows (theme checks)
    $snapT = New-Object System.Windows.Forms.Timer; $snapT.Interval = 3000
    $snapT.Add_Tick({ $snapT.Stop(); try { $bm = New-Object System.Drawing.Bitmap($form.Width, $form.Height); $form.DrawToBitmap($bm, (New-Object System.Drawing.Rectangle(0, 0, $form.Width, $form.Height))); $bm.Save($env:SSBU_INSTALLER_SNAPSHOT) } catch {} })
    $form.Add_Shown({ $snapT.Start() })
}
[System.Windows.Forms.Application]::Run($form)
Flush-Log

if ($testMode) {
    # -SelfTest / -NoRun: print what the window ended up showing
    foreach ($l in $script:testDump) { [Console]::WriteLine($l) }
    if ($SelfTest -gt 0) { [Console]::WriteLine('self test finished') }
}
