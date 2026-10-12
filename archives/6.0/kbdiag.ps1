<#
.SYNOPSIS
    KVM 链路诊断 —— 搞清楚 Windows 到底通过 KVM 看见了什么。

.DESCRIPTION
    两件事，按需要用：

      1) 枚举 + 快照对比（默认，最有价值）
         跑一次 → 拔掉键盘 USB → 再跑一次 → -Diff 列出差异。
         任何「消失的设备」都是键盘相关的，这是 macOS 的 IOUSB 看不到的。

      2) -Write 挨个试写 LED 输出报告
         对每个 HID 接口都写一次 1 字节 LED 报告，看谁接受。
         Windows 不限制 HID 写权限，不需要管理员。

.EXAMPLE
    # 键盘插着，跑一次
    powershell -ExecutionPolicy Bypass -File .\kbdiag.ps1 -Save A

    # 拔掉键盘 USB，再跑一次
    powershell -ExecutionPolicy Bypass -File .\kbdiag.ps1 -Save B -Diff

.EXAMPLE
    # 试写 LED 报告（盯着实体键盘的灯）
    powershell -ExecutionPolicy Bypass -File .\kbdiag.ps1 -Write
#>

param(
    [string]$Save = "",
    [string]$Compare = "",
    [switch]$Diff,
    [switch]$Write
)

$ErrorActionPreference = "Continue"
$OutFile = if ($Save) { $Save } else { "kbdiag-snapshot.txt" }

# ============================================================
# 设备枚举
# ============================================================
function Get-UsbSnapshot {
    $lines = @()
    try {
        Get-PnpDevice -PresentOnly:$false -ErrorAction Stop | ForEach-Object {
            $inst = $_.InstanceId
            if ($inst -match '^(USB|HID|SW)\\') {
                $vid = if ($inst -match 'VID_([0-9A-Fa-f]{4})') { $Matches[1] } else { "????" }
                $pid = if ($inst -match 'PID_([0-9A-Fa-f]{4})') { $Matches[1] } else { "????" }
                $mi  = if ($inst -match 'MI_([0-9A-Fa-f]{2})') { $Matches[1] } else { "--" }
                $lines += ("{0,-4} {1}:{2}  MI_{3}  {4,-42}  {5}" -f `
                           $inst.Split('\')[0], $vid, $pid, $mi, $_.FriendlyName, $_.Status)
            }
        }
    } catch {
        $lines += "Get-PnpDevice 失败: $($_.Exception.Message)"
    }
    return ($lines | Sort-Object -Unique)
}

Write-Host "=== 枚举 USB / HID 设备 ===" -ForegroundColor Cyan
$snap = Get-UsbSnapshot
$snap | ForEach-Object { Write-Host "  $_" }

# 标出值得关注的 VID:PID
$known = @{
    "1100:2222" = "KVM 控制芯片(已知)"
    "303A:001F" = "MX9.0 键盘"
    "046D"     = "Logitech"
    "05AC"     = "Apple"
    "2109"     = "VIA Labs 扩展器/Hub"
}
Write-Host ""
Write-Host "=== 重点关注 ===" -ForegroundColor Yellow
$hit = $snap | Where-Object { $_ -match 'VID_[0-9A-Fa-f]{4}.*PID_[0-9A-Fa-f]{4}' }
foreach ($l in $hit) {
    if ($l -match 'VID_([0-9A-Fa-f]{4})\s+PID_([0-9A-Fa-f]{4})') {
        $key = "$($Matches[1]):$($Matches[2])"
        $tag = if ($known.ContainsKey($key)) { "  <<< $($known[$key])" } else { "" }
        if ($tag) { Write-Host "  $l$tag" -ForegroundColor Green }
    }
}

# ============================================================
# 快照保存 / 对比
# ============================================================
if ($Save) {
    $snap | Set-Content -Path $OutFile -Encoding UTF8
    Write-Host "`n[快照已存] $OutFile" -ForegroundColor Cyan
}

if ($Diff) {
    if (-not (Test-Path $OutFile)) {
        Write-Host "`n没有可对比的快照 $OutFile，先跑一次不带 -Diff" -ForegroundColor Red
    } else {
        $old = Get-Content $OutFile -Encoding UTF8
        Write-Host "`n=== 与 $OutFile 的差异 ===" -ForegroundColor Cyan
        $gone = $old | Where-Object { $snap -notcontains $_ }
        $new  = $snap | Where-Object { $old -notcontains $_ }
        if (-not $gone -and -not $new) {
            Write-Host "  完全一致 —— 拔键盘没有让任何设备消失。" -ForegroundColor Yellow
        }
        if ($gone) {
            Write-Host "`n  【消失了】(这些多半和键盘有关):" -ForegroundColor Yellow
            $gone | ForEach-Object { Write-Host "    - $_" }
        }
        if ($new) {
            Write-Host "`n  【新增了】:" -ForegroundColor Green
            $new | ForEach-Object { Write-Host "    + $_" }
        }
    }
}

# ============================================================
# 试写 LED 输出报告
# ============================================================
if ($Write) {
    Write-Host "`n=== 准备 HID 写入组件 ===" -ForegroundColor Cyan
    if (-not ("KbHid" -as [type])) {
        Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;

public static class KbHid {
    [DllImport("setupapi.dll", CharSet=CharSet.Auto, SetLastError=true)]
    public static extern IntPtr SetupDiGetClassDevs(ref Guid cls, IntPtr enumer, IntPtr parent, int flags);
    [DllImport("setupapi.dll", CharSet=CharSet.Auto, SetLastError=true)]
    public static extern bool SetupDiEnumDeviceInterfaces(IntPtr di, IntPtr devInfo, ref Guid cls, uint member, out IntPtr devInfoData);
    [DllImport("setupapi.dll", CharSet=CharSet.Auto, SetLastError=true)]
    public static extern bool SetupDiGetDeviceInterfaceDetail(IntPtr di, IntPtr devInfoData, IntPtr detail, uint detailSize, out uint required, IntPtr devInfo);
    [DllImport("setupapi.dll", CharSet=CharSet.Auto, SetLastError=true)]
    public static extern bool SetupDiDestroyDeviceInfoList(IntPtr di);
    [DllImport("hid.dll", SetLastError=true)]
    public static extern void HidD_GetHidGuid(out Guid g);
    [DllImport("hid.dll", SetLastError=true)]
    public static extern bool HidD_SetOutputReport(SafeFileHandle h, byte[] buf, uint len);
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
    public static extern SafeFileHandle CreateFile(string name, uint access, uint share, IntPtr sec, uint disp, uint flags, IntPtr tmpl);
}
'@
    }

    $guid = New-Object Guid
    [KbHid]::HidD_GetHidGuid([ref]$guid)
    $di = [KbHid]::SetupDiGetClassDevs([ref]$guid, [IntPtr]::Zero, [IntPtr]::Zero, 0)
    Write-Host "  SetupDiGetClassDevs = $di"

    $buf = [Runtime.InteropServices.Marshal]::AllocHGlobal(2048)
    if ([IntPtr]::Size -eq 8) {
        [Runtime.InteropServices.Marshal]::WriteInt32($buf, 8)
    } else {
        [Runtime.InteropServices.Marshal]::WriteInt32($buf, 6)
    }

    $idx = 0
    $devInfoData = [IntPtr]::Zero
    $tried = 0
    while ([KbHid]::SetupDiEnumDeviceInterfaces($di, [IntPtr]::Zero, [ref]$guid, $idx, [ref]$devInfoData)) {
        $idx++
        $required = 0
        [void][KbHid]::SetupDiGetDeviceInterfaceDetail($di, $devInfoData, $buf, 2048, [ref]$required, [IntPtr]::Zero)
        $path = [Runtime.InteropServices.Marshal]::PtrToStringUni([IntPtr]($buf.ToInt64() + 4))
        if (-not $path) { continue }

        $vid = if ($path -match 'vid_([0-9a-f]{4})') { $Matches[1] } else { "????" }
        $pid = if ($path -match 'pid_([0-9a-f]{4})') { $Matches[1] } else { "????" }
        $mi  = if ($path -match 'mi_([0-9a-f]{2})')    { $Matches[1] } else { "--" }

        $h = [KbHid]::CreateFile($path, 0x00000002, 0, [IntPtr]::Zero, 3, 0, [IntPtr]::Zero)
        if ($h.IsInvalid) {
            Write-Host ("  {0}:{1} MI_{2}  打开失败 Win32={3}" -f $vid,$pid,$mi,[Runtime.InteropServices.Marshal]::GetLastWin32Error()) -ForegroundColor DarkGray
            continue
        }

        # bit0=Num bit1=Caps bit2=Scroll
        foreach ($case in @(@(0x01,"Num"), @(0x02,"Caps"), @(0x04,"Scroll"))) {
            $ok = [KbHid]::HidD_SetOutputReport($h, [byte[]]@($case[0]), 1)
            $tag = if ($ok) { "已接受" } else { "拒绝 Win32=" + [Runtime.InteropServices.Marshal]::GetLastWin32Error() }
            $color = if ($ok) { "Green" } else { "DarkGray" }
            Write-Host ("  {0}:{1} MI_{2}  写 0x{3:x2} ({4,-6}) -> {5}" -f $vid,$pid,$mi,$case[0],$case[1],$tag) -ForegroundColor $color
            Start-Sleep -Milliseconds 400
        }
        [void]$h.Close()
        $tried++
    }
    [Runtime.InteropServices.Marshal]::FreeHGlobal($buf)
    [void][KbHid]::SetupDiDestroyDeviceInfoList($di)
    Write-Host "`n  一共写了 $tried 个 HID 接口。" -ForegroundColor Cyan
    Write-Host "  如果 KVM 芯片显示『已接受』但实体键盘灯没亮 —— 说明 KVM 不转发 LED 报告。" -ForegroundColor Yellow
}

Write-Host "`n完成。" -ForegroundColor Cyan