<#
.SYNOPSIS
Fail-closed Microsoft Defender scan gate for a Windows build directory.
.DESCRIPTION
Run after building and before executing tests, launching Sifa, or publishing any
artifacts. Requires Windows with active Microsoft Defender and signatures updated
within the last 24 hours. This script does not enable Defender, change policy,
elevate privileges, add exclusions, restore quarantined files or update signatures.

MpCmdRun exit code 0 is insufficient: it can also mean a threat was successfully
remediated. This gate additionally checks Defender detection history and active
threats, and requires the executable inventory and hashes to remain unchanged.

A successful report describes only this scan, engine/signature version and exact
executable hashes. It is not a guarantee of safety or a false-positive verdict.
An existing requested JSON report is removed before validation so a failed run
cannot leave a stale success receipt at that output path.
.EXAMPLE
./tools/scan-windows.ps1 -Path ./build -ReportPath ./build/defender-scan.json
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateNotNullOrEmpty()]
    [string]$Path,

    [Parameter(Mandatory = $false)]
    [ValidateNotNullOrEmpty()]
    [string]$ReportPath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Get-RequiredProperty {
    param([object]$Object, [string]$Name)
    $property = $Object.PSObject.Properties[$Name]
    if ($null -eq $property -or $null -eq $property.Value) {
        throw "Defender did not report required property '$Name'; scan readiness is unknown."
    }
    return $property.Value
}

function Get-VerifiedDefenderStatus {
    $status = Get-MpComputerStatus -ErrorAction Stop
    if ($null -eq $status) { throw 'Defender health information is unavailable.' }
    foreach ($name in @('AMServiceEnabled', 'AntivirusEnabled', 'RealTimeProtectionEnabled')) {
        if ((Get-RequiredProperty $status $name) -ne $true) {
            throw "Defender '$name' is not enabled. This gate will not alter protection settings."
        }
    }
    if ((Get-RequiredProperty $status 'AMRunningMode') -ne 'Normal') {
        throw 'Defender is not running in Normal antivirus mode; scan gate is blocked.'
    }
    foreach ($name in @('AMEngineVersion', 'AntivirusSignatureVersion')) {
        $version = [string](Get-RequiredProperty $status $name)
        if ([string]::IsNullOrWhiteSpace($version) -or $version -eq '0.0.0.0') {
            throw "Defender '$name' is unavailable."
        }
    }
    $updated = ([datetime](Get-RequiredProperty $status 'AntivirusSignatureLastUpdated')).ToUniversalTime()
    $age = [datetime]::UtcNow - $updated
    if ($age.TotalHours -gt 24 -or $age.TotalMinutes -lt -5) {
        throw 'Defender signature timestamp is older than 24 hours or inconsistent with the system clock. Update protection through Windows Security and retry.'
    }
    $outdated = $status.PSObject.Properties['DefenderSignaturesOutOfDate']
    if ($null -ne $outdated -and $outdated.Value -eq $true) {
        throw 'Defender reports outdated signatures. Update protection through Windows Security and retry.'
    }
    return $status
}

function Assert-NoActiveThreats {
    foreach ($threat in @(Get-MpThreat -ErrorAction Stop)) {
        if ((Get-RequiredProperty $threat 'IsActive') -eq $true) {
            throw 'Defender reports an active threat. Resolve it through Windows Security; this gate will not restore or allow detected files.'
        }
    }
}

function Assert-NoConfiguredScanExclusions {
    $preferences = Get-MpPreference -ErrorAction Stop
    if ($null -eq $preferences) { throw 'Defender scan-exclusion settings could not be read.' }
    foreach ($name in @('ExclusionPath', 'ExclusionExtension', 'ExclusionProcess')) {
        $property = $preferences.PSObject.Properties[$name]
        if ($null -eq $property) { throw "Defender did not report '$name'; scan coverage is unknown." }
        $configured = @($property.Value | Where-Object { -not [string]::IsNullOrWhiteSpace([string]$_) })
        if ($configured.Count -gt 0) {
            throw "Defender has configured '$name' exclusions. This conservative gate cannot establish full scan coverage and will not change exclusions."
        }
    }
}

function Get-DetectionSnapshot {
    $records = @{}
    foreach ($detection in @(Get-MpThreatDetection -ErrorAction Stop)) {
        $id = [string](Get-RequiredProperty $detection 'DetectionID')
        if ([string]::IsNullOrWhiteSpace($id) -or $records.ContainsKey($id)) {
            throw 'Defender detection history contains an invalid or duplicate identity.'
        }
        $initial = ([datetime](Get-RequiredProperty $detection 'InitialDetectionTime')).ToUniversalTime()
        $changed = ([datetime](Get-RequiredProperty $detection 'LastThreatStatusChangeTime')).ToUniversalTime()
        $fingerprint = $detection | Select-Object DetectionID, ThreatID, InitialDetectionTime,
            LastThreatStatusChangeTime, RemediationTime, ActionSuccess, ThreatStatusID,
            ThreatStatusErrorCode, CurrentThreatExecutionStatusID, Resources |
            ConvertTo-Json -Depth 6 -Compress
        $records[$id] = [pscustomobject]@{ initial = $initial; changed = $changed; fingerprint = $fingerprint }
    }
    return $records
}

function Get-ExecutableInventory {
    param([string]$Directory)
    # Reject links/junctions rather than silently scanning or hashing outside the
    # requested build directory. Walk one level at a time so links are not followed.
    $pending = New-Object 'System.Collections.Generic.Queue[string]'
    $pending.Enqueue($Directory)
    $files = New-Object 'System.Collections.Generic.List[object]'
    while ($pending.Count -gt 0) {
        $current = $pending.Dequeue()
        foreach ($entry in @(Get-ChildItem -LiteralPath $current -Force -ErrorAction Stop)) {
            if (($entry.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
                throw "Build directory contains a link or junction: $($entry.FullName)"
            }
            if ($entry.PSIsContainer) {
                $pending.Enqueue($entry.FullName)
            } elseif ($entry.Extension -ieq '.exe') {
                $hash = Get-FileHash -LiteralPath $entry.FullName -Algorithm SHA256 -ErrorAction Stop
                $files.Add([pscustomobject]@{
                    path = $entry.FullName
                    sha256 = $hash.Hash.ToLowerInvariant()
                    size = [long]$entry.Length
                })
            }
        }
    }
    return @($files | Sort-Object path)
}

function Find-DefenderScanner {
    $platform = Join-Path $env:ProgramData 'Microsoft\Windows Defender\Platform'
    $candidate = $null
    if (Test-Path -LiteralPath $platform -PathType Container) {
        $versions = @(Get-ChildItem -LiteralPath $platform -Directory -ErrorAction Stop | Sort-Object {
            try { [version]($_.Name.Split('-')[0]) } catch { [version]'0.0' }
        } -Descending)
        foreach ($directory in $versions) {
            $file = Join-Path $directory.FullName 'MpCmdRun.exe'
            if (Test-Path -LiteralPath $file -PathType Leaf) { $candidate = $file; break }
        }
    }
    if ($null -eq $candidate) {
        $fallback = Join-Path $env:ProgramFiles 'Windows Defender\MpCmdRun.exe'
        if (Test-Path -LiteralPath $fallback -PathType Leaf) { $candidate = $fallback }
    }
    if ($null -eq $candidate) { throw 'The installed Microsoft Defender MpCmdRun.exe could not be located.' }
    $signature = Get-AuthenticodeSignature -LiteralPath $candidate -ErrorAction Stop
    if ($signature.Status -ne 'Valid' -or $null -eq $signature.SignerCertificate -or
        $signature.SignerCertificate.Subject -notmatch '(^|,\s*)O=Microsoft Corporation(,|$)') {
        throw 'The installed Defender scanner does not have a validated Microsoft signature.'
    }
    return $candidate
}

$reportFullPath = $null
try {
    if ($ReportPath) {
        $reportFullPath = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($ReportPath)
        if ([IO.Path]::GetExtension($reportFullPath) -ine '.json') {
            throw 'ReportPath must name a JSON file.'
        }
        if (Test-Path -LiteralPath $reportFullPath) {
            $previous = Get-Item -LiteralPath $reportFullPath -Force -ErrorAction Stop
            if ($previous.PSIsContainer -or ($previous.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
                throw 'ReportPath cannot be a directory, link or junction.'
            }
            Remove-Item -LiteralPath $reportFullPath -Force -ErrorAction Stop
        }
    }
    if ([Environment]::OSVersion.Platform -ne [PlatformID]::Win32NT) {
        throw 'This gate requires Windows and its real Microsoft Defender installation.'
    }
    foreach ($command in @('Get-MpComputerStatus', 'Get-MpThreatDetection', 'Get-MpThreat',
                            'Get-MpPreference', 'Get-AuthenticodeSignature', 'Get-FileHash')) {
        $null = Get-Command $command -ErrorAction Stop
    }
    $resolved = Resolve-Path -LiteralPath $Path -ErrorAction Stop
    if ($resolved.Provider.Name -ne 'FileSystem') { throw 'Path must be a filesystem directory.' }
    $target = Get-Item -LiteralPath $resolved.ProviderPath -Force -ErrorAction Stop
    if (-not $target.PSIsContainer -or ($target.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw 'Path must be a real build directory, not a file, link or junction.'
    }
    $scanPath = $target.FullName.TrimEnd([IO.Path]::DirectorySeparatorChar)
    if ($scanPath -eq [IO.Path]::GetPathRoot($target.FullName).TrimEnd([IO.Path]::DirectorySeparatorChar)) {
        throw 'Choose the build directory, not an entire drive.'
    }
    $statusBefore = Get-VerifiedDefenderStatus
    Assert-NoActiveThreats
    Assert-NoConfiguredScanExclusions
    $scanner = Find-DefenderScanner
    $before = @(Get-ExecutableInventory $scanPath)
    if ($before.Count -eq 0) { throw 'No executable files were found; zero-file scans cannot approve a build.' }
    $detectionsBefore = Get-DetectionSnapshot
    $startedAt = [datetime]::UtcNow
    Write-Host "Scanning $($before.Count) executable(s) and the entire build directory with Microsoft Defender."
    Write-Host "Engine: $($statusBefore.AMEngineVersion); signatures: $($statusBefore.AntivirusSignatureVersion); updated: $($statusBefore.AntivirusSignatureLastUpdated)"

    # Start-Process uses no command shell, does not request elevation, and waits
    # for the real custom scan. Never use -DisableRemediation or ignore its result.
    $process = Start-Process -FilePath $scanner -ArgumentList @('-Scan', '-ScanType', '3', '-File', ('"' + $scanPath + '"')) -NoNewWindow -PassThru -Wait -ErrorAction Stop
    $scanExitCode = $process.ExitCode
    if ($null -eq $scanExitCode) { throw 'The Defender scan did not provide a process exit code.' }
    $detectionsAfter = Get-DetectionSnapshot
    $statusAfter = Get-VerifiedDefenderStatus
    Assert-NoActiveThreats
    Assert-NoConfiguredScanExclusions
    # Detection timestamps can have second precision. Include the boundary and
    # compare records so updates to an older detection cannot escape the gate.
    $cutoff = $startedAt.AddSeconds(-2)
    foreach ($id in $detectionsAfter.Keys) {
        $record = $detectionsAfter[$id]
        if (-not $detectionsBefore.ContainsKey($id) -or
            $record.fingerprint -ne $detectionsBefore[$id].fingerprint -or
            $record.initial -ge $cutoff -or $record.changed -ge $cutoff) {
            throw 'Defender recorded a new or changed threat detection around this scan. Build execution and publication are blocked, including successfully remediated detections.'
        }
    }
    foreach ($id in $detectionsBefore.Keys) {
        if (-not $detectionsAfter.ContainsKey($id)) {
            throw 'Defender detection history changed during the scan; its outcome cannot be established.'
        }
    }
    if ($scanExitCode -ne 0) { throw "Microsoft Defender scan failed or detected a threat (exit code $scanExitCode)." }
    if ($statusBefore.AMEngineVersion -ne $statusAfter.AMEngineVersion -or
        $statusBefore.AntivirusSignatureVersion -ne $statusAfter.AntivirusSignatureVersion) {
        throw 'Defender engine or signatures changed during the scan. Retry to produce a report for one known version.'
    }
    $after = @(Get-ExecutableInventory $scanPath)
    if ($before.Count -ne $after.Count) { throw 'Executable inventory changed during the scan; publication is blocked.' }
    for ($i = 0; $i -lt $before.Count; $i++) {
        if ($before[$i].path -cne $after[$i].path -or $before[$i].sha256 -ne $after[$i].sha256 -or $before[$i].size -ne $after[$i].size) {
            throw 'An executable changed or was removed during the scan; publication is blocked.'
        }
    }
    $finishedAt = [datetime]::UtcNow
    if ($reportFullPath) {
        $report = [ordered]@{
            schema_version = 1
            scanner = 'Microsoft Defender'
            result = 'no_threats_detected'
            engine_version = [string]$statusAfter.AMEngineVersion
            signature_version = [string]$statusAfter.AntivirusSignatureVersion
            signature_updated_at = ([datetime]$statusAfter.AntivirusSignatureLastUpdated).ToUniversalTime().ToString('o')
            started_at = $startedAt.ToString('o')
            finished_at = $finishedAt.ToString('o')
            files = @($after)
        }
        $parent = [IO.Path]::GetDirectoryName($reportFullPath)
        $null = [IO.Directory]::CreateDirectory($parent)
        [IO.File]::WriteAllText($reportFullPath, ($report | ConvertTo-Json -Depth 6), (New-Object Text.UTF8Encoding($false)))
        Write-Host "Scan report: $reportFullPath"
    }
    Write-Host 'This scan completed without reported detections and the executable hashes are unchanged. This is not a safety guarantee.'
    exit 0
} catch {
    Write-Error ("Defender scan gate blocked: " + $_.Exception.Message) -ErrorAction Continue
    exit 1
}
