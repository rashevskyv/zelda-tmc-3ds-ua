param([int]$X, [int]$Y, [int]$Hold = 150)
Add-Type @"
using System; using System.Runtime.InteropServices;
public class M {
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint msg, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
  [DllImport("user32.dll")] public static extern IntPtr ChildWindowFromPointEx(IntPtr h, POINT p, uint flags);
  [DllImport("user32.dll")] public static extern bool ScreenToClient(IntPtr h, ref POINT p);
  public struct RECT { public int L, T, R, B; }
  public struct POINT { public int X, Y; }
}
"@
[M]::SetProcessDPIAware() | Out-Null
$p = Get-Process azahar -ErrorAction SilentlyContinue | Select-Object -First 1
$h = $p.MainWindowHandle
# X,Y are in window (grab.png) coordinates; convert to the client coords of the deepest child window
$r = New-Object M+RECT; [M]::GetWindowRect($h, [ref]$r) | Out-Null
$o = New-Object M+POINT; $o.X = 0; $o.Y = 0; [M]::ClientToScreen($h, [ref]$o) | Out-Null
$c = New-Object M+POINT; $c.X = $r.L + $X - $o.X; $c.Y = $r.T + $Y - $o.Y
$target = $h
for ($i = 0; $i -lt 6; $i++) {
  $child = [M]::ChildWindowFromPointEx($target, $c, 0)
  if ($child -eq [IntPtr]::Zero -or $child -eq $target) { break }
  $s = New-Object M+POINT; $s.X = $c.X; $s.Y = $c.Y; [M]::ClientToScreen($target, [ref]$s) | Out-Null
  [M]::ScreenToClient($child, [ref]$s) | Out-Null
  $target = $child; $c = $s
}
$l = [IntPtr](($c.Y -shl 16) -bor ($c.X -band 0xFFFF))
[M]::PostMessage($target, 0x0200, [IntPtr]0, $l) | Out-Null
[M]::PostMessage($target, 0x0201, [IntPtr]1, $l) | Out-Null
Start-Sleep -Milliseconds $Hold
[M]::PostMessage($target, 0x0202, [IntPtr]0, $l) | Out-Null
"clicked $X,$Y -> hwnd $target client $($c.X),$($c.Y)"
