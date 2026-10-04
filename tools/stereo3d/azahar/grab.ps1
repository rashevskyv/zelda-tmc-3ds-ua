param([string]$Out = "grab.png")
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System; using System.Runtime.InteropServices;
public class W2 {
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
  public struct RECT { public int L, T, R, B; }
}
"@
[W2]::SetProcessDPIAware() | Out-Null
$dir = Split-Path -Parent $MyInvocation.MyCommand.Path
$p = Get-Process azahar -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $p) { "azahar not running"; exit 1 }
$h = $p.MainWindowHandle
$r = New-Object W2+RECT
[W2]::GetWindowRect($h, [ref]$r) | Out-Null
$w = $r.R - $r.L; $hh = $r.B - $r.T
$bmp = New-Object System.Drawing.Bitmap $w, $hh
$g = [System.Drawing.Graphics]::FromImage($bmp)
$dc = $g.GetHdc()
$ok = [W2]::PrintWindow($h, $dc, 2)
$g.ReleaseHdc($dc)
$bmp.Save((Join-Path $dir $Out), [System.Drawing.Imaging.ImageFormat]::Png)
"saved $Out ${w}x${hh} ok=$ok title=$($p.MainWindowTitle)"
