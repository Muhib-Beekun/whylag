# GPU / NVIDIA driver diagnosis for whylag cases
# Usage: powershell -ExecutionPolicy Bypass -File scripts/gpu-driver-check.ps1

$ErrorActionPreference = 'SilentlyContinue'

Write-Host "=== whylag GPU driver diagnosis ===" -ForegroundColor Cyan
Write-Host "Time: $(Get-Date -Format o)"
Write-Host ""

Write-Host "--- Display adapters (Win32_VideoController) ---" -ForegroundColor Yellow
Get-CimInstance Win32_VideoController | ForEach-Object {
    [PSCustomObject]@{
        Name           = $_.Name
        DriverVersion  = $_.DriverVersion
        DriverDate     = $_.DriverDate
        AdapterRAM_GB  = if ($_.AdapterRAM) { [math]::Round($_.AdapterRAM / 1GB, 2) } else { $null }
        VideoProcessor = $_.VideoProcessor
        PNPDeviceID    = $_.PNPDeviceID
        Status         = $_.Status
        CurrentBitsPerPixel = $_.CurrentBitsPerPixel
        CurrentRefreshRate  = $_.CurrentRefreshRate
    }
} | Format-List

Write-Host "--- NVIDIA-SMI ---" -ForegroundColor Yellow
$smi = Get-Command nvidia-smi -ErrorAction SilentlyContinue
if ($smi) {
    nvidia-smi --query-gpu=index,name,driver_version,pstate,pcie.link.gen.current,pcie.link.width.current,temperature.gpu,power.draw,clocks.gr,clocks.mem,utilization.gpu,utilization.memory --format=csv
    Write-Host ""
    nvidia-smi -L
} else {
    Write-Host "nvidia-smi not on PATH"
}

Write-Host ""
Write-Host "--- DXGI / primary adapter (EnumDisplayDevices) ---" -ForegroundColor Yellow
Add-Type @"
using System;
using System.Runtime.InteropServices;
using System.Text;
public class DisplayHelper {
    [StructLayout(LayoutKind.Sequential, CharSet=CharSet.Ansi)]
    public struct DISPLAY_DEVICE {
        public int cb; [MarshalAs(UnmanagedType.ByValTStr, SizeConst=32)] public string DeviceName;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst=128)] public string DeviceString;
        public int StateFlags; [MarshalAs(UnmanagedType.ByValTStr, SizeConst=128)] public string DeviceID;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst=128)] public string DeviceKey;
    }
    [DllImport("user32.dll", CharSet=CharSet.Ansi)] public static extern bool EnumDisplayDevices(string lpDevice, uint iDevNum, ref DISPLAY_DEVICE lpDisplayDevice, uint dwFlags);
}
"@
$devNum = 0
while ($true) {
    $dd = New-Object DisplayHelper+DISPLAY_DEVICE
    $dd.cb = [System.Runtime.InteropServices.Marshal]::SizeOf($dd)
    if (-not [DisplayHelper]::EnumDisplayDevices($null, $devNum, [ref]$dd, 0)) { break }
    $primary = ($dd.StateFlags -band 0x4) -ne 0
    $active  = ($dd.StateFlags -band 0x1) -ne 0
    if ($active) {
        Write-Host ("[{0}] {1}" -f $(if ($primary) { "PRIMARY" } else { "       " }), $dd.DeviceString)
        Write-Host "      DeviceName: $($dd.DeviceName)"
        Write-Host "      DeviceID:   $($dd.DeviceID)"
    }
    $devNum++
}

Write-Host ""
Write-Host "--- NVIDIA driver registry (ControlSet001) ---" -ForegroundColor Yellow
$nvKeys = Get-ChildItem "HKLM:\SYSTEM\CurrentControlSet\Services\nvlddmkm" -ErrorAction SilentlyContinue
if ($nvKeys) {
    Get-ItemProperty "HKLM:\SYSTEM\CurrentControlSet\Services\nvlddmkm" -ErrorAction SilentlyContinue |
        Select-Object PSPath, DisplayName, ImagePath, Start | Format-List
}
Get-ChildItem "HKLM:\SYSTEM\CurrentControlSet\Control\Class\{4d36e968-e325-11ce-bfc1-08002be10318}" -ErrorAction SilentlyContinue |
    ForEach-Object {
        $p = Get-ItemProperty $_.PSPath -ErrorAction SilentlyContinue
        if ($p.DriverDesc -match 'NVIDIA') {
            [PSCustomObject]@{
                GPU         = $p.DriverDesc
                DriverVersion = $p.DriverVersion
                DriverDate  = $p.DriverDate
                Provider    = $p.ProviderName
            }
        }
    } | Format-Table -AutoSize

Write-Host ""
Write-Host "--- Recent NVIDIA / display related System events (7 days) ---" -ForegroundColor Yellow
Get-WinEvent -FilterHashtable @{
    LogName = 'System'
    StartTime = (Get-Date).AddDays(-7)
} -MaxEvents 200 -ErrorAction SilentlyContinue |
    Where-Object { $_.ProviderName -match 'nvlddmkm|Display|nvidia' -or $_.Message -match 'NVIDIA|display driver|nvlddmkm' } |
    Select-Object TimeCreated, Id, ProviderName, LevelDisplayName, Message -First 15 |
    Format-List

Write-Host "=== done ===" -ForegroundColor Cyan
