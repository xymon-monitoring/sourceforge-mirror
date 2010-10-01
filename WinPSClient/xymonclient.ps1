﻿# ###################################################################################
# 
# Xymon client for Windows
#
# This is a client implementation for Windows systems that support the
# Powershell scripting language.
#
# Copyright (C) 2010 Henrik Storner <henrik@hswn.dk>
# Copyright (C) 2010 David Baldwin
#
# This program is released under the GNU General Public License (GPL),
# version 2. See the file "COPYING" for details.
#
# ###################################################################################

# -----------------------------------------------------------------------------------
# User configurable settings
# -----------------------------------------------------------------------------------

$xymonservers = @( "xymonhost" )	# List your Xymon servers here
# $clientname  = "winxptest"	# Define this to override the default client hostname

# Params for default clientname
$clientfqdn = 1   		# 0 = unqualified, 1 = fully-qualified
$clientlower = 1  		# 0 = case unmodified, 1 = lowercase converted
$clientbbwinmembug = 1  # 0 = report correctly, 1 = page and virtual switched
$clientremotecfg = 0  	# 0 = don't run remote config, 1 = run remote config

$xymonclientconfig = "C:\TEMP\xymonconfig.ps1"
$xymonclientlog = "C:\TEMP\xymonclient.log"
# -----------------------------------------------------------------------------------

$XymonClientVersion = "$Id$"

function XymonInit
{
	$script:wanteddisks = @( 3 )	# 3=Local disks, 4=Network shares, 2=USB, 5=CD
	$script:wantedlogs = "Application",  "System", "Security"
	$script:maxlogage = 60

	$script:loopinterval = 300 # seconds
	$script:slowscanrate = 12
	$script:havequerycmd = (get-command -ErrorAction:SilentlyContinue query) -ne $null

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
		$ipProperties = [System.Net.NetworkInformation.IPGlobalProperties]::GetIPGlobalProperties()
		$script:clientname  = $ipProperties.HostName
		if ($clientfqdn -and ($ipProperties.DomainName -ne $null)) { 
			$script:clientname += "." + $ipProperties.DomainName
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
	
	$allprocs = Get-Process
	foreach ($p in $allprocs) {
		$thisp = $XymonProcsCpu[$p.Id]
		if ($thisp -eq $null -and $p.Id -ne 0) {
			# New process - create an entry in the curprocs table
			# We use static values here, because some get-process entries have null values
			# for the tick-count (The "SYSTEM" and "Idle" processes).
			$script:XymonProcsCpu += @{ $p.Id = @($null, 0, 0, $false) }
			$thisp = $XymonProcsCpu[$p.Id]
		}

		$thisp[3] = $true
		$thisp[2] = $p.TotalProcessorTime.Ticks - $thisp[1]
		$thisp[1] = $p.TotalProcessorTime.Ticks
		$thisp[0] = $p
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
	$wmidata = Get-WmiObject -Class $class
	$props = ($wmidata | Get-Member -MemberType Property | Sort-Object -Property Name | where { $_.Name -notlike "__*" })
	foreach ($p in $props) {
		$p.Name + " : " + $wmidata.($p.Name)
	}
}

function UnixDate([System.DateTime] $t)
{
	$t.ToString("ddd dd MMM HH:mm:ss yyyy")
}

function filesize($file,$clsize=4KB)
{
    return [math]::floor((($_.Length -1)/$clsize + 1) * $clsize/1KB)
}

function du([string]$dir,[int]$clsize=0)
{
    if($clsize -eq 0) {
        $drive = "{0}:" -f [string](get-item $dir | %{ $_.psdrive })
        $clsize = [int](Get-WmiObject win32_Volume | ? { $_.DriveLetter -eq $drive }).BlockSize
        if($clsize -eq 0 -or $clsize -eq $null) { $clsize = 4096 } # default in case not found
    }
    $sum = 0
    $dulist = ""
    get-childitem $dir -Force | % {
        if( $_.Attributes -like "*Directory*" ) {
           $dulist += du ("{0}\{1}" -f [string]$dir,$_.Name) $clsize | out-string
           $sum += $dulist.Split("`n")[-2].Split("`t")[0] # get size for subdir
        } else { 
           $sum += filesize $_ $clsize
        }
    }
    "$dulist$sum`t$dir"
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
	$epoch = [int](($localdatetime.Ticks - ([DateTime] "1/1/1970 00:00:00").Ticks) / 10000000) - $osinfo.CurrentTimeZone*60

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
	"{0,-15} {1,9} {2,9} {3,9} {4,9} {5,10} {6}" -f "Filesystem", "1K-blocks", "Used", "Avail", "Capacity", "Mounted", "Summary(Total\Avail GB)"
	foreach ($d in $disks) {
		$diskletter = ($d.DeviceId).Trim(":")
		[uint32]$diskusedKB = ([uint32]($d.Size/1KB)) - ([uint32]($d.FreeSpace/1KB))	# PS ver 1 doesnt support subtraction uint64's
		[uint32]$disksizeKB = [uint32]($d.size/1KB)

		$dsKB = "{0:F0}" -f ($d.Size / 1KB); $dsGB = "{0:F2}" -f ($d.Size / 1GB)
		$duKB = "{0:F0}" -f ($diskusedKB); $duGB = "{0:F2}" -f ($diskusedKB / 1KB);
		$dfKB = "{0:F0}" -f ($d.FreeSpace / 1KB); $dfGB = "{0:F2}" -f ($d.FreeSpace / 1GB)
		
		if ($d.Size -gt 0) {
			$duPCT = $diskusedKB/$disksizeKB
		}
		else {
			$duPCT = 0
		}

		"{0,-15} {1,9} {2,9} {3,9} {4,9:0%} {5,10} {6}" -f $diskletter, $dsKB, $duKB, $dfKB, $duPCT, "/FIXED/$diskletter", $dsGB + "\" + $dfGB
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
	if($clientbbwinmembug -eq 0) {  	# 0 = report correctly, 1 = page and virtual switched
		"virtual: $virttotal $virtused"
		"page: $pagetotal $pageused"
	} else {
		"virtual: $pagetotal $pageused"
		"page: $virttotal $virtused"
	}
}

function XymonMsgs
{
	$since = (Get-Date).AddMinutes(-($maxlogage))
	if ($wantedlogs -eq $null) {
		$wantedlogs = "Application", "System", "Security"
	}

	foreach ($l in $wantedlogs) {
		$log = Get-EventLog -List | where { $_.Log -eq $l }

		$logentries = Get-EventLog -ErrorAction:SilentlyContinue -LogName $log.Log -asBaseObject -After $since | where {$_.EntryType -match "Error|Warning"}
	
		"[msgs:eventlog_$l]"
		if ($logentries -ne $null) {
			foreach ($entry in $logentries) {
				[string]$entry.EntryType + " - " + [string]$entry.TimeGenerated + " - " + [string]$entry.Source + " - " + [string]$entry.Message
			}
		}
	}
}

function XymonDir
{
	$script:clientlocalcfg | ? { $_ -match "^dir:(.*)" } | % {
        "[dir:$($matches[1])]"
	    du $matches[1]
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
	$pref=""
	netstat -s | ?{$_ -match "=|(\w+) Statistics for"} | %{ if($_.contains("=")) {("$pref$_").REPLACE(" ","")}else{$pref=$matches[1].ToLower();""}}
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
	"{0,8} {1,-35} {2,-17} {3,-17} {4,-17} {5,8} {6,-7} {7,5} {8}" -f "PID", "User", "WorkingSet/Peak", "VirtualMem/Peak", "PagedMem/Peak", "NPS", "Handles", "%CPU", "Name"
	foreach ($p in $procs) {
		if ($svcprocs[($p.Id)] -ne $null) {
			$procname = "Service:" + $svcprocs[($p.Id)]
		}
		else {
			$procname = $p.Name
		}

		$thiswmip = Get-WmiObject -Class Win32_Process | where { $_.ProcessId -eq $p.Id }
		if(	$thiswmip -ne $null ) { # short-lived process could possibly be gone
			$saveerractpref = $ErrorActionPreference
            $ErrorActionPreference = "SilentlyContinue"
            $owner = ($thiswmip.getowner()).Domain + "\" + ($thiswmip.GetOwner().user)
            $ErrorActionPreference = $saveerractpref
		} else { $owner = "NA" }
		if ($owner -eq "\") { $owner = "SYSTEM" }
		if ($owner.length -gt 32) { $owner = $owner.substring(0, 32) }

		$pws     = "{0,8:F0}/{1,-8:F0}" -f ($p.WorkingSet64 / 1KB), ($p.PeakWorkingSet64 / 1KB)
		$pvmem   = "{0,8:F0}/{1,-8:F0}" -f ($p.VirtualMemorySize64 / 1KB), ($p.PeakVirtualMemorySize64 / 1KB)
		$ppgmem  = "{0,8:F0}/{1,-8:F0}" -f ($p.PagedMemorySize64 / 1KB), ($p.PeakPagedMemorySize64 / 1KB)
		$pnpgmem = "{0,8:F0}" -f ($p.NonPagedSystemMemorySize64 / 1KB)
			
		$thisp = $XymonProcsCpu[$p.Id]
		if ($XymonProcsCpuElapsed -gt 0 -and $thisp -ne $null) {
			$pcpu = "{0,5:F0}" -f (([int](10000*($thisp[2] / $XymonProcsCpuElapsed))) / 100)
		} else {
			$pcpu = "{0,5}" -f "-"
		}

		"{0,8} {1,-35} {2} {3} {4} {5} {6,7:F0} {7} {8}" -f $p.Id, $owner, $pws, $pvmem, $ppgmem, $pnpgmem, $p.HandleCount, $pcpu, $procname
	}
}

function XymonWho
{
	if( $havequerycmd) {
		"[who]"
		query session
	}
}

function XymonUsers
{
	if( $havequerycmd) {
		"[users]"
		query user
	}
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
	$fmt = "{0,-70} {1,-15} {2}"
	$fmt -f "Name", "Version", "Vendor"
    $fmt -f "----", "-------", "------"
    Get-WmiObject -Class Win32_Product | Sort-Object Name | 
		foreach {
			$fmt -f $_.Name, $_.Version, $_.Vendor
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
	$script:clientlocalcfg = $cfglines.Split("`n")
	$clientlocalcfg >$xymonclientconfig

	# Source the new config
	if ($clientremotecfg -ne 0) { . $xymonclientconfig }
}

function XymonReportConfig
{
	"[XymonConfig]"
	foreach($v in @("wanteddisks", "wantedlogs", "maxlogage", "loopinterval", "slowscanrate", "Version", "clientname", "clientbbwinmembug", "clientremotecfg", "clientlocalcfg" )) {
		""; "$v"
		Invoke-Expression "`$$v"
	}
}

function XymonClientSections {
	XymonUname
	XymonCpu
	XymonDisk
	XymonMemory
	XymonEventLogs
	XymonMsgs
	XymonProcs
	XymonNetstat
	XymonPorts
	XymonIPConfig
	XymonRoute
# BBWIn uses "GetIfTable" which grabs the MIB-2 interfaces.
# This is an IPHLPAPI function that does not exist in Powershell
# Dont know if it is accessible via .NET somehow.
#	XymonIfstat
	XymonSvcs
	XymonDir
	XymonUptime
	XymonWho
	XymonUsers

	XymonWMIOperatingSystem
	XymonWMIComputerSystem
	XymonWMIBIOS
	XymonWMIProcessor
	XymonWMIMemory
	XymonWMILogicalDisk

	$XymonWMIQuickFixEngineeringCache
	$XymonWMIProductCache

	XymonReportConfig
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
	$clsecs = XymonClientSections | Out-String
	$localdatetime = Get-Date
	$clout += XymonDate | Out-String
	$clout += XymonClock | Out-String
	$clout +=  $clsecs
	
	Get-Date >> $xymonclientlog
	XymonReportConfig >> $xymonclientlog
	$newconfig = XymonSend $clout $xymonservers
	XymonClientConfig $newconfig
	$delay = ($loopinterval - (Get-Date).Subtract($starttime).TotalSeconds)
	if ($delay -gt 0) { sleep $delay }
}