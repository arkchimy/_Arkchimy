param([string]$Csv = (Join-Path $PSScriptRoot 'remeasure_out\main_runs.csv'), [string]$Stage = '')
function Med($v) { $a = @($v | ForEach-Object { [double]$_ } | Sort-Object); if ($a.Count -eq 0) { return 0 }; return $a[[math]::Floor($a.Count / 2)] }
$r = Import-Csv $Csv | Where-Object { $_.Valid -eq 'True' }
if ($Stage) { $r = $r | Where-Object { $_.Stage -in ($Stage -split ',') } }
$out = foreach ($g in ($r | Group-Object Stage, Build, Workers, Label)) {
	$x = @($g.Group | Sort-Object { [int]$_.Round })
	$isChat = [int]$x[0].Room -gt 0
	$rateCol = if ($isChat) { 'DeliverPerSec' } else { 'PktPerSec' }
	[pscustomobject]@{
		Stage = $x[0].Stage; Build = $x[0].Build; W = $x[0].Workers; Label = $x[0].Label; N = $x.Count
		Rate = [int](Med ($x.$rateCol))
		Rates = (($x | ForEach-Object { [int]([double]$_.$rateCol / 10000) }) -join '/')
		PerSend = (($x | ForEach-Object { [math]::Round([double]$_.PktPerSend, 1) }) -join '/')
		Gqcs = (($x | ForEach-Object { [math]::Round([double]$_.CompletePerGqcs, 1) }) -join '/')
		Srv = Med ($x.ServerCores)
		Cli = Med ($x.C0Cores)
		CpuUs = Med ($x.CpuUsPerUnit)
		WaitUs = Med ($x.SendWaitUs)
		Lock = Med ($x.LockHoldUs)
		P50 = [int](Med ($x.C0LatP50Us)); P99 = [int](Med ($x.C0LatP99Us))
		Max = (($x | ForEach-Object { [int]([double]$_.C0LatMaxUs / 1000) }) -join '/')
		Ovf = (($x.Overflow) -join '/')
		Close = (($x.C0Close) -join '/')
	}
}
$out | Sort-Object Stage, Build, { [int]$_.W }, Label | Format-Table -Property * -AutoSize | Out-String -Width 260
