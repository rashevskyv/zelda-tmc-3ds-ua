param([string]$Keys = "", [int]$Hold = 120, [int]$Gap = 700)
Add-Type @"
using System; using System.Runtime.InteropServices;
public class K {
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint msg, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern uint MapVirtualKey(uint code, uint type);
}
"@
$p = Get-Process azahar -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $p) { "azahar not running"; exit 1 }
$h = $p.MainWindowHandle
foreach ($k in $Keys.Split(",")) {
  if ($k -match '^w(\d+)$') { Start-Sleep -Milliseconds ([int]$Matches[1]); continue }
  $hold = $Hold
  if ($k -match '^(.+)\*(\d+)$') { $k = $Matches[1]; $hold = [int]$Matches[2] }
  $vk = switch ($k) { "up" {0x26} "down" {0x28} "left" {0x25} "right" {0x27} default { [int][char]$k.ToUpper() } }
  $sc = [K]::MapVirtualKey([uint32]$vk, 0)
  $ext = if ($vk -ge 0x25 -and $vk -le 0x28) { 0x01000000 } else { 0 }
  $down = [IntPtr](1 -bor ($sc -shl 16) -bor $ext)
  $up = [IntPtr]::new([long](1 -bor ($sc -shl 16) -bor $ext) -bor 0xC0000000L)
  [K]::PostMessage($h, 0x0100, [IntPtr]$vk, $down) | Out-Null
  Start-Sleep -Milliseconds $hold
  [K]::PostMessage($h, 0x0101, [IntPtr]$vk, $up) | Out-Null
  Start-Sleep -Milliseconds $Gap
}
"sent $Keys"
