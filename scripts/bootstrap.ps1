# Install Tomodachi Life.bat runs this. It puts a private Python 3.13 into <repo>\deps\python (the python.org "embeddable" package,
# checked against its SHA-256), installs pip and requirements.txt there, then opens the installer window
# (scripts\installer_window.ps1). Nothing is installed system-wide, PATH and the registry are not touched; deleting the
# deps folder removes everything. Only if that private install fails it falls back to a Python already on the PC
# (or the per-user python.org installer) and the older tkinter window (scripts\installer_gui.py).
# Structure of the download/verify/install steps adapted from WindowSmash's tools\install_windowsmash.ps1 (SNO2207).
param([switch]$NoLaunch)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Windows.Forms
$repo = Split-Path -Parent $PSScriptRoot
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
$ProgressPreference = 'SilentlyContinue'

# --- pinned downloads (hashes taken from files fetched on 2026-10-19) ---
$pyVersion = '3.13.16'
$embedUrl = "https://www.python.org/ftp/python/$pyVersion/python-$pyVersion-embed-amd64.zip"
$embedSha = '97dae5274cc54867065e8d5a3226e48c35017ed332a0fdb0e27d5b5821961297'
$pipName = 'pip-26.2.1-py3-none-any.whl'
$pipUrl = 'https://files.pythonhosted.org/packages/f3/6e/1736e5b4ae2b778ef2f81c47d797de9f891d4d8acb047a24ca37a60294dd/pip-26.2.1-py3-none-any.whl'
$pipSha = '71138adf1f4ca900cdb7d289c21b7494329f2332b6d85f0e1c42108c0384ed3e'
# the older fallback: the per-user python.org installer
$fullVersion = '3.13.16'
$fullUrl = "https://www.python.org/ftp/python/$fullVersion/python-$fullVersion-amd64.exe"
$fullSha = 'fb4f9f5d438b2396da0086dc70b935c530cb578e37adc6d354f7ad2037fee83b'

$dl = Join-Path $repo 'build\downloads'
$depsPy = Join-Path $repo 'deps\python'
$pyExe = Join-Path $depsPy 'python.exe'

function Say([string]$text, [string]$buttons = 'OK', [string]$icon = 'Information') {
    return [System.Windows.Forms.MessageBox]::Show($text, 'Tomodachi Life installer', $buttons, $icon)
}

function Get-Verified([string]$url, [string]$file, [string]$sha) {
    if (Test-Path $file) { if ((Get-FileHash $file -Algorithm SHA256).Hash -eq $sha.ToUpper()) { return } ; Remove-Item $file -Force }
    Write-Host "Downloading $url"
    Invoke-WebRequest -Uri $url -OutFile $file -UseBasicParsing
    $h = (Get-FileHash $file -Algorithm SHA256).Hash
    if ($h -ne $sha.ToUpper()) { Remove-Item $file -Force; throw "SHA-256 mismatch for $(Split-Path -Leaf $file): got $h" }
}

function Test-Private {
    if (-not (Test-Path $pyExe)) { return $false }
    try {
        & $pyExe -c "import zstandard, PIL, Crypto, lz4" 2>$null | Out-Null
        return $LASTEXITCODE -eq 0
    } catch { return $false }
}

function Install-Private {
    New-Item -ItemType Directory -Force $dl | Out-Null
    if (-not (Test-Path $pyExe)) {
        $zip = Join-Path $dl "python-$pyVersion-embed-amd64.zip"
        Get-Verified $embedUrl $zip $embedSha
        if (Test-Path $depsPy) { Remove-Item $depsPy -Recurse -Force }
        New-Item -ItemType Directory -Force $depsPy | Out-Null
        Expand-Archive -LiteralPath $zip -DestinationPath $depsPy -Force
    }
    # enable site-packages: pythonXY._pth has "#import site"
    $pth = Get-ChildItem $depsPy -Filter 'python*._pth' | Select-Object -First 1
    if (-not $pth) { throw 'python._pth not found in the embeddable package' }
    $txt = (Get-Content $pth.FullName) -replace '^\s*#\s*import site', 'import site'
    Set-Content -Path $pth.FullName -Value $txt -Encoding ASCII
    # pip, from its pinned wheel: a wheel is a zip Python can run directly
    $whl = Join-Path $dl $pipName
    Get-Verified $pipUrl $whl $pipSha
    Write-Host 'Installing pip and the Python packages (about 30 MB) ...'
    $ErrorActionPreference = 'Continue'      # pip writes notices to stderr; only the exit codes count here
    $site = Join-Path $depsPy 'Lib\site-packages'
    New-Item -ItemType Directory -Force $site | Out-Null
    $zipCopy = Join-Path $dl 'pip-wheel.zip'
    Copy-Item $whl $zipCopy -Force
    Expand-Archive -LiteralPath $zipCopy -DestinationPath $site -Force     # a wheel is a zip: unpacking it is installing pip
    Remove-Item $zipCopy -Force
    # The embeddable Python ignores the script's folder in sys.path (its ._pth file makes it isolated), but our scripts
    # import their neighbours. A .pth line puts the running script's folder first, as a normal Python does.
    Set-Content -Path (Join-Path $site 'ssbu_scriptdir.pth') -Encoding ASCII -Value `
        "import sys, os; a = sys.argv[0] if sys.argv else ''; (a and os.path.isfile(a) and sys.path.insert(0, os.path.dirname(os.path.abspath(a))))"
    & $pyExe -m pip --version
    if ($LASTEXITCODE -ne 0) { throw "pip could not be installed (code $LASTEXITCODE)" }
    & $pyExe -m pip install --only-binary=:all: --no-warn-script-location --disable-pip-version-check -r (Join-Path $repo 'requirements.txt')
    if ($LASTEXITCODE -ne 0) { throw "pip install -r requirements.txt failed (code $LASTEXITCODE)" }
    if (-not (Test-Private)) { throw 'the private Python does not import its packages' }
}

# --- the older way, kept as a fallback ---
function Test-Python([string]$exe) {
    if (-not $exe -or -not (Test-Path $exe)) { return $false }
    if ($exe -like '*\WindowsApps\*') { return $false }          # the Microsoft Store alias, not a Python
    try {
        & $exe -c "import sys, tkinter; sys.exit(0 if sys.version_info >= (3, 10) else 1)" 2>$null | Out-Null
        return $LASTEXITCODE -eq 0
    } catch { return $false }
}

function Find-Python {
    $c = @()
    try { $c += (& py -3 -c "import sys; print(sys.executable)" 2>$null) } catch {}
    try { $c += (Get-Command python.exe -All -ErrorAction SilentlyContinue | ForEach-Object { $_.Source }) } catch {}
    foreach ($root in @("$env:LOCALAPPDATA\Programs\Python", $env:ProgramFiles, 'C:\')) {
        if (Test-Path $root) {
            $c += Get-ChildItem $root -Directory -Filter 'Python3*' -ErrorAction SilentlyContinue |
                  Sort-Object Name -Descending | ForEach-Object { Join-Path $_.FullName 'python.exe' }
        }
    }
    foreach ($exe in $c) { if (Test-Python $exe) { return $exe } }
    return $null
}

function Start-Fallback {
    $py = Find-Python
    if (-not $py) {
        $ans = Say ("The private Python could not be set up, so the installer needs Python (free, about 30 MB), which is not on this PC.`n`n" +
                    "Download Python $fullVersion from python.org and install it for your user only?`n" +
                    "(It can be removed later from Settings > Apps.)") 'YesNo' 'Question'
        if ($ans -ne 'Yes') { exit 1 }
        $file = Join-Path $dl "python-$fullVersion-amd64.exe"
        Get-Verified $fullUrl $file $fullSha
        # /passive: a progress bar and no questions; for this user only, no PATH or file-type changes
        $p = Start-Process -FilePath $file -Wait -PassThru -ArgumentList '/passive', 'InstallAllUsers=0', 'Include_launcher=0',
             'Include_test=0', 'Include_doc=0', 'Shortcuts=0', 'AssociateFiles=0', 'PrependPath=0'
        $py = Find-Python
        if ($p.ExitCode -ne 0 -or -not $py) {
            Say "Python could not be installed (code $($p.ExitCode)). Install it from https://www.python.org/downloads/ and run Install Tomodachi Life.bat again." 'OK' 'Error' | Out-Null
            exit 1
        }
    }
    Set-Content -Path (Join-Path $repo 'build\python_path.txt') -Value $py -Encoding ASCII
    $pyw = Join-Path (Split-Path -Parent $py) 'pythonw.exe'
    if (-not (Test-Path $pyw)) { $pyw = $py }
    if (-not $NoLaunch) { Start-Process -FilePath $pyw -ArgumentList "`"$repo\scripts\installer_gui.py`"" -WorkingDirectory $repo }
}

try {
    New-Item -ItemType Directory -Force (Join-Path $repo 'build') | Out-Null
    $ok = Test-Private
    if (-not $ok) {
        try { Install-Private; $ok = $true }
        catch { Write-Host "Private Python install failed: $($_.Exception.Message)"; Write-Host 'Trying a Python already on this PC instead ...' }
    }
    if ($ok) {
        Set-Content -Path (Join-Path $repo 'build\python_path.txt') -Value $pyExe -Encoding ASCII
        if (-not $NoLaunch) {
            # The installer window runs right here, in this PowerShell (Windows PowerShell is STA, as WinForms needs). It used
            # to start a hidden second PowerShell with -ExecutionPolicy Bypass, and with the Mark-of-the-Web cleared over the
            # whole folder that made Windows Defender's machine learning call this script a trojan (Trojan:Script/Wacatac.H!ml,
            # 2026-10-30) and remove it.
            & (Join-Path $repo 'scripts\installer_window.ps1')
        }
    } else {
        Start-Fallback
    }
} catch {
    Say "The installer could not start: $($_.Exception.Message)" 'OK' 'Error' | Out-Null
    exit 1
}
