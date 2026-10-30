<#
Run on Windows only, after the executable has passed the distribution scan.
This exercises the actual desktop executable with no Android SDK installed in
its isolated configuration. It does not boot Android or validate game support.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Exe,
    [string]$OutputDirectory = (Join-Path $PSScriptRoot '..\build-msvc\desktop-smoke')
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
if ($env:OS -ne 'Windows_NT') { throw 'This test requires Windows and an interactive desktop.' }
$executable = (Resolve-Path -LiteralPath $Exe).Path
if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) { throw 'Executable not found.' }
$outputPath = [IO.Path]::GetFullPath($OutputDirectory)
[IO.Directory]::CreateDirectory($outputPath) | Out-Null
$temporary = Join-Path ([IO.Path]::GetTempPath()) ('sifa-desktop-smoke-' + [Guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($temporary) | Out-Null
$localData = Join-Path $temporary 'local-data'
$missingSdk = Join-Path $temporary 'sdk-not-installed'
$emptyAvds = Join-Path $temporary 'empty-avds'
foreach ($path in @($localData, $missingSdk, $emptyAvds)) {
    [IO.Directory]::CreateDirectory($path) | Out-Null
}

Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
namespace SifaDesktopSmoke {
    public static class Native {
        private delegate bool EnumWindowsProc(IntPtr window, IntPtr data);
        [DllImport("user32.dll")] private static extern bool EnumWindows(EnumWindowsProc callback, IntPtr data);
        [DllImport("user32.dll")] private static extern uint GetWindowThreadProcessId(IntPtr window, out uint process);
        [DllImport("user32.dll", CharSet = CharSet.Unicode)]
        private static extern int GetClassName(IntPtr window, StringBuilder text, int count);
        [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr window, int id);
        [DllImport("user32.dll")] public static extern bool IsWindowEnabled(IntPtr window);
        [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr window);
        [DllImport("user32.dll", SetLastError = true)]
        public static extern bool PostMessage(IntPtr window, uint message, UIntPtr wparam, IntPtr lparam);
        [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern IntPtr SendMessageTimeout(IntPtr window, uint message, UIntPtr wparam,
            IntPtr lparam, uint flags, uint timeout, out UIntPtr result);
        [DllImport("user32.dll", EntryPoint = "SendMessageTimeoutW", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern IntPtr SendTextMessageTimeout(IntPtr window, uint message, UIntPtr wparam,
            StringBuilder lparam, uint flags, uint timeout, out UIntPtr result);
        [StructLayout(LayoutKind.Sequential)]
        public struct Rect { public int Left, Top, Right, Bottom; }
        [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr window, out Rect rect);
        [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr window, IntPtr dc, uint flags);

        public static string ClassName(IntPtr window) {
            var text = new StringBuilder(256);
            GetClassName(window, text, text.Capacity);
            return text.ToString();
        }
        public static IntPtr FindWindow(uint process, string expectedClass) {
            IntPtr found = IntPtr.Zero;
            EnumWindows(delegate(IntPtr window, IntPtr data) {
                uint owner;
                GetWindowThreadProcessId(window, out owner);
                if (owner == process && ClassName(window) == expectedClass) {
                    found = window;
                    return false;
                }
                return true;
            }, IntPtr.Zero);
            return found;
        }
        public static long Message(IntPtr window, uint message) {
            UIntPtr value;
            if (SendMessageTimeout(window, message, UIntPtr.Zero, IntPtr.Zero, 2, 2000, out value) == IntPtr.Zero)
                throw new InvalidOperationException("Window did not respond to message " + message);
            return unchecked((long)value.ToUInt64());
        }
        public static string Text(IntPtr window) {
            long length = Message(window, 0x000E);
            if (length < 0 || length > 65536) throw new InvalidOperationException("Unexpected window text size");
            var text = new StringBuilder((int)length + 1);
            UIntPtr copied;
            if (SendTextMessageTimeout(window, 0x000D, new UIntPtr((uint)text.Capacity), text, 2, 2000, out copied) == IntPtr.Zero)
                throw new InvalidOperationException("Window text read timed out");
            return text.ToString();
        }
    }
}
'@

function Assert-True([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}

function New-TestProcess([string]$Arguments) {
    $info = New-Object Diagnostics.ProcessStartInfo
    $info.FileName = $executable
    $info.Arguments = $Arguments
    $info.WorkingDirectory = $temporary
    $info.UseShellExecute = $false
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    # Apply only to this child. The runner's SDK, devices and user data are intact.
    $info.EnvironmentVariables['LOCALAPPDATA'] = $localData
    $info.EnvironmentVariables['ANDROID_HOME'] = $missingSdk
    $info.EnvironmentVariables['ANDROID_SDK_ROOT'] = $missingSdk
    $info.EnvironmentVariables['ANDROID_AVD_HOME'] = $emptyAvds
    $info.EnvironmentVariables['ANDROID_USER_HOME'] = (Join-Path $temporary 'android-user')
    $info.EnvironmentVariables['ANDROID_EMULATOR_HOME'] = (Join-Path $temporary 'android-emulator')
    $process = New-Object Diagnostics.Process
    $process.StartInfo = $info
    if (-not $process.Start()) { throw 'Could not start the desktop executable.' }
    return $process
}

function Save-Screenshot([IntPtr]$Window) {
    if ($Window -eq [IntPtr]::Zero) { return $false }
    $bitmap = $null
    $graphics = $null
    $dc = [IntPtr]::Zero
    try {
        Add-Type -AssemblyName System.Drawing
        $rect = New-Object SifaDesktopSmoke.Native+Rect
        if (-not [SifaDesktopSmoke.Native]::GetWindowRect($Window, [ref]$rect)) { return $false }
        $width = $rect.Right - $rect.Left
        $height = $rect.Bottom - $rect.Top
        if ($width -le 0 -or $height -le 0 -or $width -gt 6000 -or $height -gt 6000) { return $false }
        $bitmap = New-Object Drawing.Bitmap($width, $height)
        $graphics = [Drawing.Graphics]::FromImage($bitmap)
        $dc = $graphics.GetHdc()
        $captured = [SifaDesktopSmoke.Native]::PrintWindow($Window, $dc, 2)
        $graphics.ReleaseHdc($dc)
        $dc = [IntPtr]::Zero
        if (-not $captured) { return $false }
        $bitmap.Save((Join-Path $outputPath 'desktop.png'), [Drawing.Imaging.ImageFormat]::Png)
        return $true
    } catch {
        Write-Warning ('Optional desktop screenshot unavailable: ' + $_.Exception.Message)
        return $false
    } finally {
        if ($dc -ne [IntPtr]::Zero -and $null -ne $graphics) { $graphics.ReleaseHdc($dc) }
        if ($null -ne $graphics) { $graphics.Dispose() }
        if ($null -ne $bitmap) { $bitmap.Dispose() }
    }
}

$guiProcess = $null
$cliProcess = $null
$guiStdout = $null
$guiStderr = $null
$window = [IntPtr]::Zero
$checks = New-Object 'Collections.Generic.List[string]'
$result = [ordered]@{
    result = 'failed'
    executable_sha256 = (Get-FileHash -LiteralPath $executable -Algorithm SHA256).Hash.ToLowerInvariant()
    checks = @()
    screenshot_saved = $false
    android_boot_tested = $false
    game_tested = $false
}
$failure = $null
try {
    $cliProcess = New-TestProcess '--version'
    $cliStdout = $cliProcess.StandardOutput.ReadToEndAsync()
    $cliStderr = $cliProcess.StandardError.ReadToEndAsync()
    Assert-True ($cliProcess.WaitForExit(10000)) 'GUI-subsystem CLI --version did not exit.'
    Assert-True ($cliProcess.ExitCode -eq 0) ('CLI --version failed: ' + $cliStderr.Result)
    $version = $cliStdout.Result.Trim()
    Assert-True ($version -match '^Sifa \d+\.\d+\.\d+$') ('Redirected CLI output is missing or invalid: ' + $version)
    $result.version = $version
    $checks.Add('GUI-subsystem CLI preserves redirected stdout and exit status')

    $guiProcess = New-TestProcess ''
    $guiStdout = $guiProcess.StandardOutput.ReadToEndAsync()
    $guiStderr = $guiProcess.StandardError.ReadToEndAsync()
    $deadline = [DateTime]::UtcNow.AddSeconds(20)
    do {
        $guiProcess.Refresh()
        if ($guiProcess.HasExited) { throw ('Desktop exited before creating its window: ' + $guiStderr.Result) }
        $window = [SifaDesktopSmoke.Native]::FindWindow([uint32]$guiProcess.Id, 'SifaAndroidDesktop')
        # EnumWindows can observe the HWND while CreateWindowEx/WM_CREATE are
        # still running, before the application reaches ShowWindow. Visibility
        # is part of readiness, not an immediate post-creation invariant.
        if ($window -ne [IntPtr]::Zero -and [SifaDesktopSmoke.Native]::IsWindowVisible($window)) { break }
        Start-Sleep -Milliseconds 100
    } while ([DateTime]::UtcNow -lt $deadline)
    Assert-True ($window -ne [IntPtr]::Zero) 'Native Sifa window was not created within 20 seconds.'
    Assert-True ([SifaDesktopSmoke.Native]::IsWindowVisible($window)) 'Native Sifa window did not become visible within 20 seconds.'
    [SifaDesktopSmoke.Native]::Message($window, 0) | Out-Null
    $checks.Add('Visible native Windows window responds to messages')

    $controls = @{}
    foreach ($definition in @(
        @{ id = 101; class = 'Edit'; name = 'SDK path' },
        @{ id = 103; class = 'Button'; name = 'Refresh devices' },
        @{ id = 104; class = 'ComboBox'; name = 'Android device' },
        @{ id = 108; class = 'Button'; name = 'Start Android' },
        @{ id = 109; class = 'Button'; name = 'Stop Android' },
        @{ id = 110; class = 'Button'; name = 'Google Play' },
        @{ id = 111; class = 'Button'; name = 'Game store page' },
        @{ id = 112; class = 'Button'; name = 'Start game' },
        @{ id = 113; class = 'Button'; name = 'SDK setup help' },
        @{ id = 114; class = 'Edit'; name = 'Status log' },
        @{ id = 116; class = 'ComboBox'; name = 'Android resolution' }
    )) {
        $handle = [SifaDesktopSmoke.Native]::GetDlgItem($window, $definition.id)
        Assert-True ($handle -ne [IntPtr]::Zero) ('Missing native control: ' + $definition.name)
        Assert-True ([SifaDesktopSmoke.Native]::ClassName($handle) -ieq $definition.class) ('Wrong control class: ' + $definition.name)
        $controls[$definition.id] = $handle
    }
    $deadline = [DateTime]::UtcNow.AddSeconds(20)
    do {
        $logText = [SifaDesktopSmoke.Native]::Text($controls[114])
        if ([SifaDesktopSmoke.Native]::IsWindowEnabled($controls[101]) -and $logText.Length -gt 0) { break }
        Start-Sleep -Milliseconds 100
    } while ([DateTime]::UtcNow -lt $deadline)
    Assert-True ([SifaDesktopSmoke.Native]::IsWindowEnabled($controls[101])) 'SDK inspection did not finish.'
    Assert-True ($logText -match 'Android SDK') 'Missing-SDK status was not reported in the native log.'
    Assert-True ([SifaDesktopSmoke.Native]::Message($controls[104], 0x0146) -eq 0) 'The isolated empty AVD list was not empty.'
    foreach ($id in @(108, 109, 110, 111, 112)) {
        Assert-True (-not [SifaDesktopSmoke.Native]::IsWindowEnabled($controls[$id])) ('Unavailable Android action was enabled: ' + $id)
    }
    Assert-True ([SifaDesktopSmoke.Native]::IsWindowEnabled($controls[113])) 'Setup help must remain usable when the SDK is missing.'
    $checks.Add('Missing SDK is reported; Android, Play and game actions remain unavailable')
    Assert-True ([SifaDesktopSmoke.Native]::IsWindowEnabled($controls[116])) 'Android resolution cannot be configured before startup.'
    Assert-True ([SifaDesktopSmoke.Native]::Message($controls[116], 0x0147) -eq 0) 'Fresh desktop preferences did not select the 1280x720 resolution option.'
    $checks.Add('Fresh desktop preferences select the minimum 1280x720 resolution')

    $previousLength = $logText.Length
    [SifaDesktopSmoke.Native]::Message($controls[103], 0x00F5) | Out-Null
    $deadline = [DateTime]::UtcNow.AddSeconds(20)
    do {
        $logText = [SifaDesktopSmoke.Native]::Text($controls[114])
        if ([SifaDesktopSmoke.Native]::IsWindowEnabled($controls[103]) -and $logText.Length -gt $previousLength) { break }
        Start-Sleep -Milliseconds 100
    } while ([DateTime]::UtcNow -lt $deadline)
    Assert-True ($logText.Length -gt $previousLength) 'Refresh did not complete another SDK/AVD inspection.'
    Assert-True (-not [SifaDesktopSmoke.Native]::IsWindowEnabled($controls[108])) 'Refresh incorrectly enabled Android without an SDK.'
    [SifaDesktopSmoke.Native]::Message($window, 0) | Out-Null
    $checks.Add('Refresh executes again and the desktop remains responsive')

    $children = @(Get-CimInstance Win32_Process -Filter ('ParentProcessId = ' + $guiProcess.Id))
    Assert-True ($children.Count -eq 0) 'The missing-SDK desktop unexpectedly launched a child process.'
    $checks.Add('Opening the native launcher without an SDK starts no browser or runtime child')
    $result.screenshot_saved = Save-Screenshot $window
    $logText | Set-Content -LiteralPath (Join-Path $outputPath 'desktop-status.txt') -Encoding UTF8

    Assert-True ([SifaDesktopSmoke.Native]::PostMessage($window, 0x0010, [UIntPtr]::Zero, [IntPtr]::Zero)) 'Could not request normal window close.'
    Assert-True ($guiProcess.WaitForExit(15000)) 'Native desktop did not exit after WM_CLOSE.'
    Assert-True ($guiProcess.ExitCode -eq 0) ('Native desktop exited with code ' + $guiProcess.ExitCode)
    $window = [IntPtr]::Zero
    $preferencesPath = Join-Path $localData 'Sifa\android-desktop.json'
    Assert-True (Test-Path -LiteralPath $preferencesPath -PathType Leaf) 'Desktop preferences were not saved in isolated LOCALAPPDATA.'
    $preferences = Get-Content -LiteralPath $preferencesPath -Raw | ConvertFrom-Json
    Assert-True ($null -ne $preferences.cores -and $null -ne $preferences.memory_mb) 'Saved desktop preferences are invalid.'
    Assert-True ($preferences.width -eq 1280 -and $preferences.height -eq 720) 'The default desktop resolution was not saved as 1280x720.'
    $checks.Add('WM_CLOSE exits successfully and saves preferences only in the isolated user directory')
    $result.result = 'passed'
} catch {
    $failure = $_.Exception.Message
    $result.error = $failure
    if (-not $result.screenshot_saved) { $result.screenshot_saved = Save-Screenshot $window }
    $annotation = $failure.Replace('%', '%25').Replace("`r", '%0D').Replace("`n", '%0A')
    Write-Host "::error title=Native desktop smoke failed::$annotation"
} finally {
    foreach ($process in @($guiProcess, $cliProcess)) {
        if ($null -eq $process) { continue }
        if (-not $process.HasExited) {
            if ($process -eq $guiProcess -and $window -ne [IntPtr]::Zero) {
                [SifaDesktopSmoke.Native]::PostMessage($window, 0x0010, [UIntPtr]::Zero, [IntPtr]::Zero) | Out-Null
                $process.WaitForExit(3000) | Out-Null
            }
            if (-not $process.HasExited) { $process.Kill(); $process.WaitForExit(5000) | Out-Null }
        }
    }
    if ($null -ne $guiStdout -and $guiStdout.IsCompleted) {
        $guiStdout.Result | Set-Content -LiteralPath (Join-Path $outputPath 'desktop-stdout.log') -Encoding UTF8
    }
    if ($null -ne $guiStderr -and $guiStderr.IsCompleted) {
        $guiStderr.Result | Set-Content -LiteralPath (Join-Path $outputPath 'desktop-stderr.log') -Encoding UTF8
    }
    $result.checks = @($checks.ToArray())
    $result.finished_at = [DateTime]::UtcNow.ToString('o')
    $result | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $outputPath 'result.json') -Encoding UTF8
    if ($null -ne $guiProcess) { $guiProcess.Dispose() }
    if ($null -ne $cliProcess) { $cliProcess.Dispose() }
    Remove-Item -LiteralPath $temporary -Recurse -Force
}
if ($null -ne $failure) { throw $failure }
Write-Host ('Native desktop smoke passed: ' + $checks.Count + ' checks. Android and game execution were not tested.')
