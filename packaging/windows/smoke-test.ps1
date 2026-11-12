# Opens dist/theorder1886-windows.zip as a player gets it, on a Windows without MSYS2 and without
# the game (.github/workflows/windows-package.yml): the expected files are there, no game or player
# file ships, every DLL the package's executables import is in the package or is a Windows system
# DLL, theorder-probe.exe starts (--cpu-features), and the launcher runs a command line that exits by
# itself (--recomplauncher-prepare on an empty folder: exit code 1, "Incomplete" message, no window).
# Adapted from the sister project's packaging/windows/smoke-test.ps1 (same lineage, GPL-2.0).
# The smoke with a real game stays local: tools/smoke_package.py.
# Usage: powershell -NoProfile -File smoke-test.ps1 path\to\theorder1886-windows.zip
param([Parameter(Mandatory)][string]$Package)
$ErrorActionPreference = 'Stop'
# A player's PATH: a DLL found only in MSYS2 or another toolchain must not hide a missing one.
$env:PATH = "$env:SystemRoot\System32;$env:SystemRoot;$env:SystemRoot\System32\WindowsPowerShell\v1.0"
$work = Join-Path ([IO.Path]::GetTempPath()) ("theorder-smoke-" + [guid]::NewGuid())

$failed = @()
try {
    # Entry by entry: zips written by Windows PowerShell 5.1 use backslashes in entry names.
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $zip = [IO.Compression.ZipFile]::OpenRead((Resolve-Path -LiteralPath $Package).Path)
    try {
        foreach ($entry in $zip.Entries) {
            $rel = $entry.FullName.Replace('\', '/')
            if ($rel.Split('/') -contains '..') { throw "unsafe path in the zip: $rel" }
            $target = Join-Path $work $rel
            if ($rel.EndsWith('/')) { $null = New-Item -ItemType Directory -Force -Path $target; continue }
            $null = New-Item -ItemType Directory -Force -Path (Split-Path $target)
            [IO.Compression.ZipFileExtensions]::ExtractToFile($entry, $target, $true)
        }
    } finally { $zip.Dispose() }
    $tree = Join-Path $work 'theorder1886-windows'
    if (-not (Test-Path -LiteralPath $tree)) { throw "the zip has no theorder1886-windows folder" }

    # Files a player needs.
    foreach ($rel in 'The Order 1886.exe', 'Play The Order 1886.exe', 'bin\theorder-probe.exe',
                     'bin\theorder-gpu-capabilities.exe', 'run.py', 'recomplauncher.json',
                     'README-Windows.txt', 'LICENSE', 'scripts', 'patches') {
        if (-not (Test-Path -LiteralPath (Join-Path $tree $rel))) { $failed += "missing from the package: $rel" }
    }

    # The same names package.sh refuses to ship: game files and anything a player makes.
    $forbidden = 'eboot.bin', 'sce_module', 'sce_sys', 'savedata', 'user', 'cache', '*.log', 'theorder.ini',
        'settings.json', 'mods.json', 'patches.json', '*.spv', '*.pkg', '*.sprx', '*.prx', 'nvngx_dlss*.dll'
    # fsr4_shaders holds the open FSR 4 model's own shaders, not the game's.
    $models = Join-Path $tree 'fsr4_shaders'
    $found = Get-ChildItem -LiteralPath $tree -Recurse -Force |
        Where-Object { -not $_.FullName.StartsWith($models) } |
        Where-Object { $name = $_.Name; $forbidden | Where-Object { $name -like $_ } }
    if ($found) { $failed += "files that must not ship: $($found.FullName -join ', ')" }

    # Imports: parse each PE file's import table (delay-load imports are not listed) and look the
    # names up in the package or in System32 (the API-set stubs api-ms-win-*/ext-ms-win-* are virtual).
    function Get-PeImports([string]$Path) {
        $b = [IO.File]::ReadAllBytes($Path)
        if ($b.Length -lt 64 -or $b[0] -ne 0x4D -or $b[1] -ne 0x5A) { return @() }
        $pe = [BitConverter]::ToInt32($b, 0x3C)
        if ($pe + 24 -gt $b.Length -or [BitConverter]::ToUInt32($b, $pe) -ne 0x4550) { return @() }
        $sections = [BitConverter]::ToUInt16($b, $pe + 6)
        $optSize = [BitConverter]::ToUInt16($b, $pe + 20)
        $opt = $pe + 24
        $magic = [BitConverter]::ToUInt16($b, $opt)
        $dir = if ($magic -eq 0x20B) { $opt + 120 } elseif ($magic -eq 0x10B) { $opt + 104 } else { return @() }
        $importRva = [BitConverter]::ToUInt32($b, $dir)
        if ($importRva -eq 0) { return @() }
        $table = $opt + $optSize
        function ToOffset([uint32]$rva) {
            for ($i = 0; $i -lt $sections; $i++) {
                $s = $table + 40 * $i
                $va = [BitConverter]::ToUInt32($b, $s + 12)
                $size = [Math]::Max([BitConverter]::ToUInt32($b, $s + 8), [BitConverter]::ToUInt32($b, $s + 16))
                if ($rva -ge $va -and $rva -lt $va + $size) { return [int]($rva - $va + [BitConverter]::ToUInt32($b, $s + 20)) }
            }
            return -1
        }
        $names = @()
        $d = ToOffset $importRva
        while ($d -ge 0 -and $d + 20 -le $b.Length) {
            $nameRva = [BitConverter]::ToUInt32($b, $d + 12)
            if ($nameRva -eq 0) { break }
            $n = ToOffset $nameRva
            if ($n -lt 0) { break }
            $e = $n
            while ($e -lt $b.Length -and $b[$e] -ne 0) { $e++ }
            $names += [Text.Encoding]::ASCII.GetString($b, $n, $e - $n)
            $d += 20
        }
        return $names
    }
    $inPackage = @{}
    Get-ChildItem -LiteralPath $tree -Recurse -File | Where-Object { $_.Extension -in '.dll', '.pyd' } |
        ForEach-Object { $inPackage[$_.Name.ToLowerInvariant()] = $true }
    $checked = 0
    $missing = @{}
    Get-ChildItem -LiteralPath $tree -Recurse -File |
        Where-Object { $_.Extension -in '.exe', '.dll', '.pyd' } | ForEach-Object {
            $checked++
            foreach ($dll in (Get-PeImports $_.FullName)) {
                $key = $dll.ToLowerInvariant()
                if ($inPackage.ContainsKey($key) -or $key -like 'api-ms-win-*' -or $key -like 'ext-ms-win-*') { continue }
                if (Test-Path -LiteralPath (Join-Path $env:SystemRoot "System32\$dll")) { continue }
                if (-not $missing.ContainsKey($key)) { $missing[$key] = @() }
                $missing[$key] += $_.Name
            }
        }
    Write-Host "Import check: $checked PE files"
    foreach ($key in ($missing.Keys | Sort-Object)) {
        $failed += "$key is imported by $((@($missing[$key]) | Select-Object -Unique) -join ', ') and is neither in the package nor in System32"
    }

    # Runs a program with a timeout; returns its exit code (or $null when it had to be stopped).
    function Invoke-Timed([string]$Exe, [string[]]$Arguments, [string]$Log, [int]$Seconds) {
        $p = Start-Process -FilePath $Exe -ArgumentList $Arguments -NoNewWindow -PassThru `
            -RedirectStandardOutput $Log -RedirectStandardError "$Log.err"
        $null = $p.Handle  # without it ExitCode stays empty after WaitForExit(timeout)
        if (-not $p.WaitForExit($Seconds * 1000)) {
            Stop-Process -Id $p.Id -Force
            $p.WaitForExit()
            return $null
        }
        return $p.ExitCode
    }

    # A missing DLL stops the probe before main (0xC0000135).
    $code = Invoke-Timed (Join-Path $tree 'bin\theorder-probe.exe') @('--cpu-features') (Join-Path $work 'probe.log') 60
    if ($null -eq $code) { $failed += 'theorder-probe.exe --cpu-features did not finish in 60 s' }
    elseif ($code -ne 0) { $failed += ('theorder-probe.exe --cpu-features exited 0x{0:X}' -f $code) }
    Get-Content -ErrorAction SilentlyContinue (Join-Path $work 'probe.log')

    # The launcher without a window: an empty folder is "not a game folder", exit code 1 and a message.
    $empty = Join-Path $work 'empty-game-folder'
    $null = New-Item -ItemType Directory -Path $empty
    $log = Join-Path $work 'prepare.log'
    $code = Invoke-Timed (Join-Path $tree 'The Order 1886.exe') @('--recomplauncher-prepare', "`"$empty`"") $log 120
    $text = "$(Get-Content -Raw -ErrorAction SilentlyContinue $log)$(Get-Content -Raw -ErrorAction SilentlyContinue "$log.err")"
    Write-Host $text
    if ($null -eq $code) { $failed += 'The Order 1886.exe --recomplauncher-prepare did not finish in 120 s' }
    elseif ($code -ne 1) { $failed += ('The Order 1886.exe --recomplauncher-prepare exited 0x{0:X}, expected 1' -f $code) }
    elseif ($text -notmatch 'Incomplete or unreadable game folder') {
        $failed += 'The Order 1886.exe --recomplauncher-prepare did not report the incomplete folder'
    }
}
finally {
    Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
}
if ($failed) {
    $failed | ForEach-Object { Write-Host "FAIL: $_" }
    exit 1
}
Write-Host 'OK: the package opens without MSYS2'
