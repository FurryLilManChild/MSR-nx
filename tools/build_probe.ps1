# MSR-nx: generates the game's C code and builds the PC probe (msr_probe.exe).
# Run from C:\MSR-nx in PowerShell:  powershell -ExecutionPolicy Bypass -File tools\build_probe.ps1
# Uses MSYS2's MinGW-w64 GCC (C:\msys64\mingw64) and Python 3 with capstone.

param(
    [string]$Gcc = "C:\msys64\mingw64\bin\gcc.exe",
    [int]$Jobs = [Environment]::ProcessorCount,
    [switch]$SkipGenerate
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
$env:PATH = (Split-Path -Parent $Gcc) + ";" + $env:PATH

if (-not (Test-Path $Gcc)) { throw "GCC not found at $Gcc. Install it in MSYS2: pacman -S mingw-w64-x86_64-gcc" }
if (-not (Test-Path "extract\code.bin")) { throw "extract\code.bin is missing" }
if (-not (Test-Path "analysis\msr2ds\functions.csv")) { throw "analysis\msr2ds\functions.csv is missing" }

$gen = "build\gen"
$obj = "build\obj"
$out = "out\probe"
New-Item -ItemType Directory -Force $gen, $obj, $out | Out-Null

if (-not $SkipGenerate) {
    Write-Host "[1/3] Generating C from code.bin (about a minute)..."
    Copy-Item "extract\msr2ds\layout.json" "extract\layout.json" -Force
    py -3 tools\gen\generate.py extract analysis\msr2ds\functions.csv $gen
    if ($LASTEXITCODE -ne 0) { throw "generate.py failed" }
}

$flags = @("-O2", "-w", "-fno-strict-aliasing", "-ffp-contract=off", "-DMSR_TRACE",
           "-Iruntime\cpu", "-Iruntime\os")
$sources = @(Get-ChildItem "$gen\unit_*.c" | Sort-Object Name | ForEach-Object { $_.FullName })
$sources += (Resolve-Path "$gen\table.c").Path
$sources += @("runtime\cpu\dispatch.c", "runtime\os\log.c", "runtime\os\mem.c", "runtime\os\svc.c",
              "runtime\probe\main_probe.c") | ForEach-Object { (Resolve-Path $_).Path }

$headerTime = (Get-ChildItem "runtime\cpu\*.h", "runtime\os\*.h" | Sort-Object LastWriteTime | Select-Object -Last 1).LastWriteTime
Write-Host "[2/3] Compiling $($sources.Count) files with $Jobs jobs..."
$running = @()
$done = 0
$objects = @()
foreach ($src in $sources) {
    $o = Join-Path $obj ([IO.Path]::GetFileNameWithoutExtension($src) + ".o")
    $objects += $o
    # Skip files that are older than their object and the runtime headers.
    if ((Test-Path $o) -and ((Get-Item $o).LastWriteTime -gt (Get-Item $src).LastWriteTime) -and
        ((Get-Item $o).LastWriteTime -gt $headerTime)) {
        $done++
        continue
    }
    while (($running | Where-Object { -not $_.HasExited }).Count -ge $Jobs) { Start-Sleep -Milliseconds 300 }
    $finished = @($running | Where-Object { $_.HasExited })
    foreach ($p in $finished) {
        if ($p.ExitCode -ne 0) { throw "a compile failed; see build\obj\*.err" }
        $done++
        Write-Host "  $done of $($sources.Count)"
    }
    $running = @($running | Where-Object { -not $_.HasExited })
    $argList = $flags + @("-c", "`"$src`"", "-o", "`"$o`"")
    $p = Start-Process -FilePath $Gcc -ArgumentList $argList -NoNewWindow -PassThru `
        -RedirectStandardError ($o + ".err")
    $null = $p.Handle   # keeps the exit code readable after the process ends
    $running += $p
}
foreach ($p in $running) {
    $p.WaitForExit()
    if ($p.ExitCode -ne 0) { throw "a compile failed; see build\obj\*.err" }
    $done++
    Write-Host "  $done of $($sources.Count)"
}

Write-Host "[3/3] Linking..."
& $Gcc -o "$out\msr_probe.exe" $objects
if ($LASTEXITCODE -ne 0) { throw "link failed" }
Write-Host "Built $out\msr_probe.exe"
