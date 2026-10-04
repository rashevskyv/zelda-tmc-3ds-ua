param([int]$Wait = 25, [string]$Out = "shot.png", [string]$Keys = "")
Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms
Add-Type @"
using System; using System.Runtime.InteropServices;
public class W {
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int c);
  public struct RECT { public int L, T, R, B; }
}
"@
[W]::SetProcessDPIAware() | Out-Null
$dir = Split-Path -Parent $MyInvocation.MyCommand.Path
$p = Get-Process azahar -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $p) {
  $rom = "$env:APPDATA\Azahar\sdmc\3ds\The Minish Cap 3DS\tmc-3ds-3dtest.3dsx"
  $p = Start-Process -FilePath "C:\Program Files\Azahar\azahar.exe" -ArgumentList "`"$rom`"" -PassThru
}
Start-Sleep -Seconds $Wait
$p.Refresh()
$h = $p.MainWindowHandle
[W]::ShowWindow($h, 9) | Out-Null
[W]::SetForegroundWindow($h) | Out-Null
Start-Sleep -Milliseconds 700
if ($Keys) { foreach ($k in $Keys.Split(",")) { [System.Windows.Forms.SendKeys]::SendWait($k); Start-Sleep -Milliseconds 900 } }
$r = New-Object W+RECT
[W]::GetWindowRect($h, [ref]$r) | Out-Null
$w = $r.R - $r.L; $hh = $r.B - $r.T
$bmp = New-Object System.Drawing.Bitmap $w, $hh
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($r.L, $r.T, 0, 0, (New-Object System.Drawing.Size $w, $hh))
$bmp.Save((Join-Path $dir $Out), [System.Drawing.Imaging.ImageFormat]::Png)
"saved $Out ${w}x${hh} title=$($p.MainWindowTitle)"
