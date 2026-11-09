# Resource guard for the retest batches (not versioned). Every 2 s: system commit and the game's
# private bytes. If the game passes the private-bytes limit, its state is logged and it is stopped,
# so a runaway allocation cannot exhaust the commit limit and take the runner down with it.
param([string]$Log = "$PSScriptRoot\memwatch.log", [double]$LimitGB = 30, [string]$StopFlag = "$PSScriptRoot\memwatch.stop")
Remove-Item $StopFlag -ErrorAction SilentlyContinue
while (-not (Test-Path $StopFlag)) {
    $os = Get-CimInstance Win32_OperatingSystem
    $limit = $os.TotalVirtualMemorySize / 1MB
    $used = ($os.TotalVirtualMemorySize - $os.FreeVirtualMemory) / 1MB
    $p = Get-Process theorder-probe -ErrorAction SilentlyContinue | Select-Object -First 1
    $priv = if ($p) { $p.PrivateMemorySize64 / 1GB } else { 0 }
    $line = "{0:o} commit {1:N1}/{2:N1} GB game pid {3} private {4:N2} GB" -f (Get-Date), $used, $limit, $p.Id, $priv
    if ($p -and $priv -gt $LimitGB) {
        Add-Content $Log "$line KILL (private above $LimitGB GB; working set $([math]::Round($p.WorkingSet64/1GB,2)) GB, threads $($p.Threads.Count))"
        Stop-Process -Id $p.Id -Force
    } elseif ($p) {
        Add-Content $Log $line
    }
    Start-Sleep -Seconds 2
}
