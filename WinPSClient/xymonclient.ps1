﻿# -----------------------------------------------------------------------------------
# User configurable settings
# -----------------------------------------------------------------------------------

$xymonservers = @( "xymonhost" )	# List your Xymon servers here
# $clientname  = "winxptest"	# Define this to override the default client hostname

# Params for default clientname
$clientfqdn = 1   		# 0 = unqualified, 1 = fully-qualified
$clientlower = 1  		# 0 = case unmodified, 1 = lowercase converted

$xymonclientconfig = "C:\TEMP\xymonconfig.ps1"
# -----------------------------------------------------------------------------------


function XymonInit
{
	$script:wanteddisks = @( 3 )	# 3=Local disks, 4=Network shares, 2=USB, 5=CD
	$script:wantedlogs = "Application",  "System", "Security"
	$script:maxlogage = 60

	$script:loopinterval = 300
	$script:slowscanrate = 12

	if ($cpuinfo -ne $null) 	{ Remove-Variable cpuinfo }
	if ($totalload -ne $null)	{ Remove-Variable totalload }
	if ($numcpus -ne $null)		{ Remove-Variable numcpus }
	if ($numcores -ne $null)	{ Remove-Variable numcores }
	if ($numvcpus -ne $null)	{ Remove-Variable numvcpus }
	
	if ($osinfo -ne $null)		{ Remove-Variable osinfo }
	if ($svcs -ne $null)		{ Remove-Variable svcs }
	if ($procs -ne $null)		{ Remove-Variable procs }
	if ($disks -ne $null)		{ Remove-Variable disks }
	if ($netifs -ne $null)		{ Remove-Variable netifs }
	if ($svcprocs -ne $null)	{ Remove-Variable svcprocs }

	if ($localdatetime -ne $null)	{ Remove-Variable localdatetime }
	if ($uptime -ne $null)			{ Remove-Variable uptime }
	if ($usercount -ne $null)		{ Remove-Variable usercount }
	
	if ($XymonProcsCpu -ne $null) 			{ Remove-Variable XymonProcsCpu }
	if ($XymonProcsCpuTStart -ne $null) 	{ Remove-Variable XymonProcsTStart }
	if ($XymonProcsCpuElapsed -ne $null) 	{ Remove-Variable XymonProcsElapsed }
	
	if ($clientname -eq $null -or $clientname -eq "") {
		$script:clientname  = $env:computername
		if ($clientfqdn -and ($env:userdnsdomain -ne $null)) { 
			$script:clientname += "." + $env:userdnsdomain
		}
		if ($clientlower) { $script:clientname = $clientname.ToLower() }
	}
}

function XymonProcsCPUUtilisation
{
	# XymonProcsCpu is a table with 4 elements:
	# 	0 = process handle
	# 	1 = last tick value
	# 	2 = ticks used since last poll
	# 	3 = activeflag

	if ($XymonProcsCpu -eq $null) {
		$script:XymonProcsCpu = @{ 0 = ( $null, 0, 0, $false) }
		$script:XymonProcsCpuTStart = (Get-Date).ticks
		$script:XymonProcsCpuElapsed = 0
	}
	else {
		$script:XymonProcsCpuElapsed = (Get-Date).ticks - $XymonProcsCpuTStart
		$script:XymonProcsCpuTStart = (Get-Date).Ticks
	}
	
	Get-Process | foreach {
		$thisp = $XymonProcsCpu[$_.Id]
		
		if ($thisp -eq $null -and $_.Id -ne 0) {
			# New process - create an entry in the curprocs table
			# We use static values here, because some get-process entries have null values
			# for the tick-count (The "SYSTEM" and "Idle" processes).
			$script:XymonProcsCpu += @{ $_.Id = @($null, 0, 0, $false) }
			$thisp = $XymonProcsCpu[$_.Id]
		}

		$thisp[3] = $true
		$thisp[2] = $_.TotalProcessorTime.Ticks - $thisp[1]
		$thisp[1] = $_.TotalProcessorTime.Ticks
		$thisp[0] = $_
	}
}


function XymonCollectInfo
{
	$script:cpuinfo = @(Get-WmiObject -Class Win32_Processor)
	$script:totalload = 0
	$script:numcpus  = $cpuinfo.Count
	$script:numcores = 0
	$script:numvcpus = 0
	foreach ($cpu in $cpuinfo) { 
		$script:totalload += $cpu.LoadPercentage
		$script:numcpus  += 1
		$script:numcores += $cpu.NumberOfCores
		$script:numvcpus += $cpu.NumberOfLogicalProcessors
	}
	$script:totalload /= $numcpus

	$script:osinfo = Get-WmiObject -Class Win32_OperatingSystem
	$script:svcs = Get-WmiObject -Class Win32_Service | Sort-Object -Property Name
	$script:procs = Get-Process | Sort-Object -Property Id
	foreach ($disktype in $wanteddisks) { 
		$mydisks += @( (Get-WmiObject -Class Win32_LogicalDisk | where { $_.DriveType -eq $disktype } ))
	}
	$script:disks = $mydisks | Sort-Object DeviceID
	$script:netifs = Get-WmiObject -Class Win32_NetworkAdapterConfiguration | where { $_.IPEnabled -eq $true }

	$script:svcprocs = @{([int]-1) = ""}
	foreach ($s in $svcs) {
		if ($s.State -eq "Running") {
			if ($svcprocs[([int]$s.ProcessId)] -eq $null) {
				$script:svcprocs += @{ ([int]$s.ProcessId) = $s.Name }
			}
			else {
				$script:svcprocs[([int]$s.ProcessId)] += ("/" + $s.Name)
			}
		}
	}
	
	$script:localdatetime = $osinfo.ConvertToDateTime($osinfo.LocalDateTime)
	$script:uptime = $localdatetime - $osinfo.ConvertToDateTime($osinfo.LastBootUpTime)

	$script:usercount = 0	# FIXME

	XymonProcsCPUUtilisation
}

function WMIProp($class)
{
	$wmidata = gwmi -class $class
	$props = ($wmidata | gm -MemberType Property | Sort-Object -Property Name | ? { $_.Name -notlike "__*" })
	foreach ($p in $props) {
		$p.Name + " : " + $wmidata.($p.Name)
	}
}

function UnixDate([System.DateTime] $t)
{
	$DayNames = "","Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"
	$MonthNames = "", "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
	
	$res = ""
	$res += $DayNames[$t.DayOfWeek.value__] + " "
	$res += $MonthNames[$t.Month] + " "
	$res += [string]$t.Day + " "
	if ($t.Hour -lt 10) { $res += "0" + [string]$t.Hour } else { $res += [string]$t.Hour }
	$res += ":"
	if ($t.Minute -lt 10) { $res += "0" + [string]$t.Minute } else { $res += [string]$t.Minute }
	$res += ":"
	if ($t.Second -lt 10) { $res += "0" + [string]$t.Second } else { $res += [string]$t.Second }
	$res += " "
	$res += [string]$t.Year

	$res
}

function pad($s, $maxlen)
{
	if ($s.Length -gt $maxlen) {
		$s.Substring(0, $maxlen)
	}
	else {
		$s.Padright($maxlen)
	}
}

function XymonPrintProcess($pobj, $name, $pct)
{
	$pcpu = (("{0:F1}" -f $pct) + "`%").PadRight(8)
	$ppid = ([string]($pobj[0]).Id).PadRight(6)
	
	if ($name.length -gt 30) { $name = $name.substring(0, 30) }
	$pname = $name.PadRight(32)

	$pprio = ([string]$pobj[0].BasePriority).PadRight(5)
	$ptime = (([string]$pobj[0].TotalProcessorTime).Split(".")[0]).PadRight(9)
	$pmem = ([string]($pobj[0].WorkingSet64 / 1KB)) + "k"

	$pcpu + $ppid + $pname + $pprio + $ptime + $pmem
}

function XymonDate
{
	"[date]"
	UnixDate $localdatetime
}

function XymonClock
{
	$epoch = (($localdatetime.Ticks - ([DateTime] "1/1/1970 00:00:00").Ticks) / 10000000) - $osinfo.CurrentTimeZone*60

	"[clock]"
	"epoch: " + $epoch
	"local: " + (UnixDate $localdatetime)
	"UTC: " + (UnixDate $localdatetime.AddMinutes(-$osinfo.CurrentTimeZone))
	"NTP server: " + (Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\W32Time\Parameters').NtpServer
	"Time Synchronisation type:" + (Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\W32Time\Parameters').Type
}

function XymonUptime
{
	"[uptime]"
	"sec: " + [string] ([int]($uptime.Ticks / 10000000))
	([string]$uptime.Days) + " days " + ([string]$uptime.Hours) + " hours " + ([string]$uptime.Minutes) + " minutes " + ([string]$uptime.Seconds) + " seconds"
	"Bootup: " + $osinfo.LastBootUpTime
}

function XymonUname
{
	"[uname]"
	$osinfo.Caption + " " + $osinfo.CSDVersion + " (build " + $osinfo.BuildNumber + ")"
}

function XymonCpu
{
	"[cpu]"
	"up: " + ([string]$uptime.Days) + " days, " + $usercount + " users, " + $procs.count + " procs, load=" + ([string]$totalload) + "`%"
	""
	"CPU states:"
	"`ttotal`t" + ([string]$totalload) + "`%"
	foreach ($cpu in $cpuinfo) { 
		"`t" + $cpu.DeviceID + "`t" + $cpu.LoadPercentage + "`%"
	}

	if ($XymonProcsCpuElapsed -gt 0) {
		""
		"CPU".PadRight(8) + "PID".PadRight(6) + "Image Name".PadRight(32) + "Pri".PadRight(5) + "Time".PadRight(9) + "MemUsage"

		foreach ($p in $XymonProcsCpu.Keys) {
			$thisp = $XymonProcsCpu[$p]
			if ($thisp[3] -eq $true) {
				# Process found in the latest Get-Procs call, so it is active.
				if ($svcprocs[$p] -eq $null) {
					$pname = ($thisp[0]).Name
				}
				else {
					$pname = "SVC:" + $svcprocs[$p]
				}

				$usedpct = ([int](10000*($thisp[2] / $XymonProcsCpuElapsed))) / 100
				XymonPrintProcess $thisp $pname $usedpct

				$thisp[3] = $false	# Set flag to catch a dead process on the next run
			}
			else {
				# Process has died, clear it.
				$thisp[2] = $thisp[1] = 0
				$thisp[0] = $null
			}
		}
	}
}

function XymonDisk
{
	"[disk]"
	"Filesystem".PadRight(15) + "1K-blocks".PadLeft(9) + " " + "Used".PadLeft(9) + " " + "Avail".PadLeft(9) + " " + "Capacity".PadLeft(9) + " " + "Mounted".PadRight(10) + "Summary(Total\Avail)"
	foreach ($d in $disks) {
		$diskletter = ($d.DeviceId).Trim(":")
		$diskused = ($d.Size - $d.FreeSpace)
		$dsKB = "{0:F0}" -f ($d.Size / 1KB); $dsGB = "{0:F2}" -f ($d.Size / 1GB)
		$duKB = "{0:F0}" -f ($diskused / 1KB); $duGB = "{0:F2}" -f ($diskused / 1GB); $duPCT = "{0:N0}" -f (100*($diskused/$d.Size))
		$dfKB = "{0:F0}" -f ($d.Freesize / 1KB); $dfGB = "{0:F2}" -f ($d.FreeSpace / 1GB)
		
		$diskletter.PadRight(15) + $dsKB.PadLeft(9) + " " + $duKB.PadLeft(9) + " " + $dsKB.PadLeft(9) + " " + ($duPCT + "`%").PadLeft(9) + " " + ("/FIXED/" + $diskletter).PadRight(10) + $dsGB + "`\" + $dfGB
	}
}

function XymonMemory
{
	$physused  = [int](($osinfo.TotalVisibleMemorySize - $osinfo.FreePhysicalMemory)/1KB)
	$phystotal = [int]($osinfo.TotalVisibleMemorySize / 1KB)
	$pageused  = [int](($osinfo.SizeStoredInPagingFiles - $osinfo.FreeSpaceInPagingFiles) / 1KB)
	$pagetotal = [int]($osinfo.SizeStoredInPagingFiles / 1KB)
	$virtused  = [int](($osinfo.TotalVirtualMemorySize - $osinfo.FreeVirtualMemory) / 1KB)
	$virttotal = [int]($osinfo.TotalVirtualMemorySize / 1KB)

	"[memory]"
	"memory    Total    Used"
	"physical: $phystotal $physused"
	"virtual: $virttotal $virtused"
	"page: $pagetotal $pageused"
}

function XymonMsgs
{
	$since = (Get-Date).AddMinutes(-($logmaxage))
	if ($wantedlogs -eq $null) {
		$wantedlogs = "Application", "System", "Security"
	}

	foreach ($l in $wantedlogs) {
		$log = Get-EventLog -List | where { $_.Log -eq $l }

		$oldpref = $ErrorActionPreference
		$ErrorActionPreference = "silentlycontinue"
		$logentries = Get-EventLog -LogName $log.Log -asBaseObject -After $since -newest 100 -EntryType Error,Warning
		$ErrorActionPreference = $oldpref
	
		"[msgs:eventlog_$l]"
		if ($logentries -ne $null) {
			foreach ($entry in $logentries) {
				[string]$entry.EntryType + " - " + [string]$entry.TimeGenerated + " - " + [string]$entry.Source + " - " + [string]$entry.Message
			}
		}
	}
}

function XymonPorts
{
	"[ports]"
	netstat -an
}

function XymonIpconfig
{
	"[ipconfig]"
	ipconfig /all
}

function XymonRoute
{
	"[route]"
	netstat -rn
}

function XymonNetstat
{
	"[netstat]"
	netstat -s
}

function XymonSvcs
{
	"[svcs]"
	"Name".PadRight(39) + " " + "StartupType".PadRight(12) + " " + "Status".PadRight(14) + " " + "DisplayName"
	foreach ($s in $svcs) {
		if ($s.StartMode -eq "Auto") { $stm = "automatic" } else { $stm = $s.StartMode.ToLower() }
		if ($s.State -eq "Running")  { $state = "started" } else { $state = $s.State.ToLower() }
		$s.Name.Replace(" ","_").PadRight(39) + " " + $stm.PadRight(12) + " " + $state.PadRight(14) + " " + $s.DisplayName
	}
}

function XymonProcs
{
	"[procs]"
	"PID".PadRight(8)+"Name"
	foreach ($p in $procs) {
		if ($svcprocs[($p.Id)] -ne $null) {
			$procname = "Service:" + $svcprocs[($p.Id)]
		}
		else {
			$procname = $p.Name
		}
		([string]$p.Id).PadRight(8) + $procname
	}
}

function XymonWho
{
	"[who]"
	query session
}


function XymonWMIOperatingSystem
{
	"[WMI:Win32_OperatingSystem]"
	WMIProp Win32_OperatingSystem
}

function XymonWMIQuickFixEngineering
{
	"[WMI:Win32_QuickFixEngineering]"
	Get-WmiObject -Class Win32_QuickFixEngineering | where { $_.Description -ne "" } | Sort-Object HotFixID | Format-Wide -Property HotFixID -AutoSize
}

function XymonWMIProduct
{
	"[WMI:Win32_Product]"
	"Name".PadRight(45) + "   " + "Version".PadRight(15) + "   " + "Vendor".PadRight(30)
	"----".PadRight(45) + "   " + "-------".PadRight(15) + "   " + "------".PadRight(30)
	Get-WmiObject -Class Win32_Product | Sort-Object Name | 
		foreach {
			(pad $_.Name 45) + "   " + (pad $_.Version 15) + "   " + (pad $_.Vendor 30)
		}
}

function XymonWMIComputerSystem
{
	"[WMI:Win32_ComputerSystem]"
	WMIProp Win32_ComputerSystem
}

function XymonWMIBIOS
{
	"[WMI:Win32_BIOS]"
	WMIProp Win32_BIOS
}

function XymonWMIProcessor
{
	"[WMI:Win32_Processor]"
	$cpuinfo | Format-List DeviceId,Name,CurrentClockSpeed,NumberOfCores,NumberOfLogicalProcessors,CpuStatus,Status,LoadPercentage
}

function XymonWMIMemory
{
	"[WMI:Win32_PhysicalMemory]"
	Get-WmiObject -Class Win32_PhysicalMemory | Format-Table -AutoSize BankLabel,Capacity,DataWidth,DeviceLocator
}

function XymonWMILogicalDisk
{
	"[WMI:Win32_LogicalDisk]"
	Get-WmiObject -Class Win32_LogicalDisk | Format-Table -AutoSize
}

function XymonEventLogs
{
	"[EventlogSummary]"
	Get-EventLog -List | Format-Table -AutoSize
}


function XymonSend($msg, $servers)
{
	$saveresponse = 1	# Only on the first server

	$ASCIIEncoder = New-Object System.Text.ASCIIEncoding

	foreach ($srv in $servers) {
		$srvparams = $srv.Split(":")
		$srvip = $srvparams[0]
		if ($srvparams.Count -gt 1) {
			$srvport = $srvparams[1]
		}
		else {
			$srvport = 1984
		}

		try {
			$socket = new-object System.Net.Sockets.TcpClient($srvip, $srvport)
		}
		catch {
			$errmsg = $Error[0].Exception
			Write-Error "Cannot connect to host $srv : $errmsg"
			continue;
		}

		$stream = $socket.GetStream() 
		
		foreach ($line in $msg)
		{
			# Convert data to ASCII instead of UTF, and to Unix line breaks
			$sent += $socket.Client.Send($ASCIIEncoder.GetBytes($line.Replace("`r","") + "`n"))
		}

		if ($saveresponse) {
			$socket.Client.Shutdown(1)	# Signal to Xymon we're done writing.

		    $buffer = new-object System.Byte[] 4096
			$encoding = new-object System.Text.AsciiEncoding
			$outputBuffer = ""

    		do {
        		## Allow data to buffer for a bit
        		start-sleep -m 200

        		## Read what data is available
        		$foundmore = $false
        		$stream.ReadTimeout = 1000

        		do {
            		try {
                		$read = $stream.Read($buffer, 0, 1024)

                		if ($read -gt 0) {
                    		$foundmore = $true
                    		$outputBuffer += ($encoding.GetString($buffer, 0, $read))
                		}
					}
					catch { 
						$foundMore = $false; $read = 0
					}
        		} while ($read -gt 0)
    		} while ($foundmore)
		}

		$socket.Close()
	}

	$outputbuffer
}

function XymonClientConfig($cfglines)
{
	if ($cfglines -eq $null -or $cfglines -eq "") { exit }

	# Convert to Windows-style linebreaks
	$cfgwinformat = $cfglines.Split("`n")
	$cfgwinformat >$xymonclientconfig

	# Source the new config
	. $xymonclientconfig
}

function XymonReportConfig
{
	"[XymonConfig]"
	""; "wanteddisks"
	$script:wanteddisks
	""; "wantedlogs"
	$script:wantedlogs
	""; "maxlogage"
	$script:maxlogage
	""; "loopinterval"
	$script:loopinterval
	""; "slowscanrate"
	$script:slowscanrate
}

##### Main code #####
XymonInit

$running = $true
$loopcount = ($slowscanrate - 1)

while ($running -eq $true) {
	$starttime = Get-Date
	
	$loopcount++; 
	if ($loopcount -eq $slowscanrate) { 
		$loopcount = 0
		$XymonWMIQuickFixEngineeringCache = XymonWMIQuickFixEngineering
		$XymonWMIProductCache = XymonWMIProduct
	}

	XymonCollectInfo

	$clout = "client " + $clientname + ".bbwin win32" | Out-String

	$clout += XymonDate | Out-String
	$clout += XymonClock | Out-String
	$clout += XymonUname | Out-String
	$clout += XymonCpu | Out-String
	$clout += XymonDisk | Out-String
	$clout += XymonMemory | Out-String
	$clout += XymonEventLogs | Out-String
	$clout += XymonMsgs | Out-String
	$clout += XymonProcs | Out-String
	$clout += XymonNetstat | Out-String
	$clout += XymonPorts | Out-String
	$clout += XymonIPConfig | Out-String
	$clout += XymonRoute | Out-String
#	$clout += XymonIfstat | Out-String
	$clout += XymonSvcs | Out-String
	$clout += XymonUptime | Out-String
	$clout += XymonWho | Out-String

	$clout += XymonWMIOperatingSystem | Out-String
	$clout += XymonWMIComputerSystem | Out-String
	$clout += XymonWMIBIOS | Out-String
	$clout += XymonWMIProcessor | Out-String
	$clout += XymonWMIMemory | Out-String
	$clout += XymonWMILogicalDisk | Out-String

	$clout += $XymonWMIQuickFixEngineeringCache | Out-String
	$clout += $XymonWMIProductCache | Out-String

	$clout += XymonReportConfig | Out-String
	
	$newconfig = XymonSend $clout $xymonservers
	XymonClientConfig $newconfig

	$delay = ($loopinterval - (Get-Date).Subtract($starttime).TotalSeconds)
	if ($delay -gt 0) { sleep $delay }
}
