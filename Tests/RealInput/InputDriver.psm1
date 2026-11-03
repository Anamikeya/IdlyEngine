# Real mouse and keyboard input for acceptance runs: SendInput events to a window owned by a process we started, and
# captures of the desktop afterwards. These are the same events a person's hands produce, so they go through GLFW,
# ImGui and the game exactly as play does. Needs an unlocked, interactive desktop, and moves the real mouse.
#
# SendInput goes to whatever has focus, not to a chosen window, so a person using the machine must never receive it.
# A run waits for the desktop to be idle before it starts. Every press, click, key and movement first checks that the
# editor still has focus and that nobody else moved the cursor; if not, the run stops at once. It never takes focus back.

Add-Type -AssemblyName System.Drawing
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class Win32Input {
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    [StructLayout(LayoutKind.Sequential)] public struct MOUSEINPUT { public int dx, dy; public uint mouseData, dwFlags, time; public IntPtr extra; }
    [StructLayout(LayoutKind.Sequential)] public struct KEYBDINPUT { public ushort wVk, wScan; public uint dwFlags, time; public IntPtr extra; }
    [StructLayout(LayoutKind.Explicit)] public struct UNION { [FieldOffset(0)] public MOUSEINPUT mi; [FieldOffset(0)] public KEYBDINPUT ki; }
    [StructLayout(LayoutKind.Sequential)] public struct INPUT { public uint type; public UNION u; }
    [DllImport("user32.dll", SetLastError=true)] public static extern uint SendInput(uint n, INPUT[] inputs, int size);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern bool GetCursorPos(out POINT p);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    [DllImport("user32.dll")] public static extern uint MapVirtualKey(uint code, uint map);
    [StructLayout(LayoutKind.Sequential)] public struct LASTINPUTINFO { public uint cbSize, dwTime; }
    [DllImport("user32.dll")] public static extern bool GetLastInputInfo(ref LASTINPUTINFO info);
    public static long IdleMilliseconds() {
        var info = new LASTINPUTINFO { cbSize = (uint)Marshal.SizeOf(typeof(LASTINPUTINFO)) };
        GetLastInputInfo(ref info);
        return (long)(uint)((uint)Environment.TickCount - info.dwTime);
    }
    public static void Mouse(int dx, int dy, uint flags, uint data = 0) {
        var i = new INPUT { type = 0 }; i.u.mi = new MOUSEINPUT { dx = dx, dy = dy, mouseData = data, dwFlags = flags };
        SendInput(1, new[] { i }, Marshal.SizeOf(typeof(INPUT)));
    }
    public static void Character(char c) {
        const uint unicode = 4, keyUp = 2;
        foreach (var flags in new[] { unicode, unicode | keyUp }) {
            var i = new INPUT { type = 1 }; i.u.ki = new KEYBDINPUT { wVk = 0, wScan = c, dwFlags = flags };
            SendInput(1, new[] { i }, Marshal.SizeOf(typeof(INPUT)));
        }
    }
    public static void Key(ushort vk, bool up) {
        uint flags = up ? 2u : 0u;
        // Navigation keys need the extended flag, or Windows reports their numpad twins.
        if (vk == 0x2E || vk == 0x2D || (vk >= 0x21 && vk <= 0x28)) flags |= 1u;
        var i = new INPUT { type = 1 }; i.u.ki = new KEYBDINPUT { wVk = vk, wScan = (ushort)MapVirtualKey(vk, 0), dwFlags = flags };
        SendInput(1, new[] { i }, Marshal.SizeOf(typeof(INPUT)));
    }
}
'@
[void][Win32Input]::SetProcessDPIAware()

$script:editor = $null
$script:window = [IntPtr]::Zero
$script:origin = $null
$script:width = 0
$script:height = 0

# Waits until nobody has touched the mouse or keyboard for a while; a person at the machine keeps it waiting.
function Wait-ForIdleDesktop { param([int]$IdleSeconds = 45, [int]$TimeoutSeconds = 1800)
    $clock = [Diagnostics.Stopwatch]::StartNew()
    while ([Win32Input]::IdleMilliseconds() -lt $IdleSeconds * 1000) {
        if ($clock.Elapsed.TotalSeconds -gt $TimeoutSeconds) { throw "The desktop was in use for $TimeoutSeconds s; not starting a real-input run." }
        Start-Sleep -Seconds 2
    }
}

# Before any input: it would go to the focused window, which must be ours.
function Assert-EditorFocused {
    if ([Win32Input]::GetForegroundWindow() -ne $script:window) {
        throw 'The editor lost focus (someone is using the desktop): stopping so no input reaches another application.'
    }
}

function Start-Editor {
    param([string]$Executable, [string[]]$Arguments, [string]$WorkingDirectory, [string]$Log)
    $script:editor = Start-Process -FilePath $Executable -ArgumentList $Arguments -WorkingDirectory $WorkingDirectory -PassThru -RedirectStandardOutput "$Log.out.txt" -RedirectStandardError "$Log.err.txt"
    $null = $script:editor.Handle
    for ($i = 0; $i -lt 100 -and $script:editor.MainWindowHandle -eq [IntPtr]::Zero; $i++) {
        Start-Sleep -Milliseconds 200
        $script:editor.Refresh()
    }
    if ($script:editor.MainWindowHandle -eq [IntPtr]::Zero) { throw 'The editor showed no window' }
    $script:window = $script:editor.MainWindowHandle
    Update-Window
    Focus-Editor
    return $script:editor
}

function Update-Window {
    $r = New-Object Win32Input+RECT
    [void][Win32Input]::GetClientRect($script:window, [ref]$r)
    $p = New-Object Win32Input+POINT
    [void][Win32Input]::ClientToScreen($script:window, [ref]$p)
    $script:origin = $p
    $script:width = $r.Right
    $script:height = $r.Bottom
}

function Get-WindowSize { Update-Window; return @($script:width, $script:height) }

function Focus-Editor {
    # A newly started window normally gets focus. No key press to force it: that would land in whatever has focus now.
    [void][Win32Input]::ShowWindow($script:window, 5)
    [void][Win32Input]::SetForegroundWindow($script:window)
    Start-Sleep -Milliseconds 300
    Assert-EditorFocused
}

# Positions are fractions of the client area, so a run does not depend on the DPI scale.
function Move-To { param([double]$X, [double]$Y)
    Assert-EditorFocused
    Update-Window
    [void][Win32Input]::SetCursorPos($script:origin.X + [int]($X * $script:width), $script:origin.Y + [int]($Y * $script:height))
    Start-Sleep -Milliseconds 60
}

$script:drift = 0
function Get-CursorDrift { return $script:drift }
# A cursor that moved without this driver means a person is using the mouse: stop, rather than fight them for it.
function Assert-Cursor { param([int]$ScreenX, [int]$ScreenY)
    Assert-EditorFocused
    $p = New-Object Win32Input+POINT
    [void][Win32Input]::GetCursorPos([ref]$p)
    if ([Math]::Abs($p.X - $ScreenX) -gt 2 -or [Math]::Abs($p.Y - $ScreenY) -gt 2) {
        $script:drift++
        throw 'The cursor moved without this driver (someone is using the desktop): stopping.'
    }
}

# The same, for a point given as fractions of the client area. Before a press that starts a drag.
function Assert-CursorOn { param([double]$X, [double]$Y)
    Update-Window
    Assert-Cursor ($script:origin.X + [int]($X * $script:width)) ($script:origin.Y + [int]($Y * $script:height))
}

function Click-At { param([double]$X, [double]$Y, [switch]$Right)
    Move-To $X $Y
    $sx = $script:origin.X + [int]($X * $script:width); $sy = $script:origin.Y + [int]($Y * $script:height)
    # A small real movement, so the window sees the mouse arrive, then a pause of a few frames for hover.
    [Win32Input]::Mouse(1, 0, 1); Start-Sleep -Milliseconds 40
    [void][Win32Input]::SetCursorPos($sx, $sy); Start-Sleep -Milliseconds 80
    Assert-Cursor $sx $sy
    $down, $up = if ($Right) { 8, 16 } else { 2, 4 }
    [Win32Input]::Mouse(0, 0, $down); Start-Sleep -Milliseconds 70
    [Win32Input]::Mouse(0, 0, $up); Start-Sleep -Milliseconds 150
}

# Fast enough for the UI to read it as one double-click.
function Double-Click-At { param([double]$X, [double]$Y)
    Move-To $X $Y
    $sx = $script:origin.X + [int]($X * $script:width); $sy = $script:origin.Y + [int]($Y * $script:height)
    [Win32Input]::Mouse(1, 0, 1); Start-Sleep -Milliseconds 40
    [void][Win32Input]::SetCursorPos($sx, $sy); Start-Sleep -Milliseconds 80
    Assert-Cursor $sx $sy
    foreach ($i in 1, 2) {
        [Win32Input]::Mouse(0, 0, 2); Start-Sleep -Milliseconds 35
        [Win32Input]::Mouse(0, 0, 4); Start-Sleep -Milliseconds 35
    }
    Start-Sleep -Milliseconds 120
}

# ---- Controls by name: the editor's --ui-map file says where each one is, in client pixels ----
$script:uiMap = $null
function Set-UiMap { param([string]$Path) $script:uiMap = $Path; Remove-Item $Path -ErrorAction SilentlyContinue }
function Get-Widget { param([string]$Name, [int]$TimeoutMilliseconds = 4000)
    $clock = [Diagnostics.Stopwatch]::StartNew()
    do {
        try {
            $stream = [IO.File]::Open($script:uiMap, 'Open', 'Read', 'ReadWrite,Delete')
            $reader = New-Object IO.StreamReader $stream
            $text = $reader.ReadToEnd(); $reader.Close(); $stream.Close()
            foreach ($line in $text -split "`n") {
                $p = $line.Split("`t")
                if ($p.Count -eq 5 -and $p[0] -ceq $Name) {
                    $culture = [Globalization.CultureInfo]::InvariantCulture
                    return @{ X0 = [double]::Parse($p[1], $culture); Y0 = [double]::Parse($p[2], $culture); X1 = [double]::Parse($p[3], $culture); Y1 = [double]::Parse($p[4], $culture) }
                }
            }
        } catch [IO.IOException] { }
        Start-Sleep -Milliseconds 50
    } while ($clock.ElapsedMilliseconds -lt $TimeoutMilliseconds)
    throw "The editor shows no control called '$Name'."
}
# Whether the control is on screen now, without waiting long for it.
function Test-Widget { param([string]$Name)
    try { $null = Get-Widget $Name 400; return $true } catch { if ("$_" -like "*no control called*") { return $false }; throw }
}
# Client-area fractions of a point inside a control; Fx and Fy are fractions of the control.
function Get-WidgetPoint { param([string]$Name, [double]$Fx = 0.5, [double]$Fy = 0.5)
    $w = Get-Widget $Name
    Update-Window
    return @((($w.X0 + $Fx * ($w.X1 - $w.X0)) / $script:width), (($w.Y0 + $Fy * ($w.Y1 - $w.Y0)) / $script:height))
}
# Presses on one point and releases on another, with a real movement between: what a drag and drop is made of.
function Drag-Between { param([double[]]$From, [double[]]$To, [string]$Capture)
    Move-To $From[0] $From[1]; [Win32Input]::Mouse(1, 0, 1); Start-Sleep -Milliseconds 150
    Assert-CursorOn $From[0] $From[1]
    Mouse-Press 'Left' $true; Start-Sleep -Milliseconds 120
    try {
        Move-Relative 0 -12 120
        Move-To $To[0] $To[1]; [Win32Input]::Mouse(1, 0, 1); Start-Sleep -Milliseconds 150
        [Win32Input]::Mouse(1, 0, 1); Start-Sleep -Milliseconds 200
        if ($Capture) { Save-Capture $Capture }
    } finally { Mouse-Press 'Left' $false; Start-Sleep -Milliseconds 350 }
}
function Click-Widget { param([string]$Name, [double]$Fx = 0.5, [double]$Fy = 0.5, [switch]$Right)
    $p = Get-WidgetPoint $Name $Fx $Fy
    Click-At $p[0] $p[1] -Right:$Right
}
function Double-Click-Widget { param([string]$Name, [double]$Fx = 0.5, [double]$Fy = 0.5)
    $p = Get-WidgetPoint $Name $Fx $Fy
    Double-Click-At $p[0] $p[1]
}
function Move-ToWidget { param([string]$Name, [double]$Fx = 0.5, [double]$Fy = 0.5)
    $p = Get-WidgetPoint $Name $Fx $Fy
    Move-To $p[0] $p[1]
    # Hover needs a real movement, and a few frames.
    [Win32Input]::Mouse(1, 0, 1); Start-Sleep -Milliseconds 80
}
# A row of the list a combo has opened, counted from zero. Its rows are not named, so this goes by the combo's own height.
function Click-ComboRow { param([string]$Name, [int]$Row)
    $w = Get-Widget $Name
    $unit = ($w.Y1 - $w.Y0) / 23
    Update-Window
    Click-At (($w.X0 + 0.5 * ($w.X1 - $w.X0)) / $script:width) (($w.Y1 + (8 + ($Row + 0.5) * 17) * $unit) / $script:height)
}
# Any mouse button. As with the left button, a release is always sent.
function Mouse-Press { param([string]$Button, [bool]$Down)
    if ($Down) { Assert-EditorFocused }
    $flags = switch ($Button) { 'Left' { if ($Down) { 2 } else { 4 } } 'Right' { if ($Down) { 8 } else { 16 } } 'Middle' { if ($Down) { 32 } else { 64 } } }
    [Win32Input]::Mouse(0, 0, $flags)
}
function Scroll-Wheel { param([int]$Notches)
    Assert-EditorFocused
    # mouseData holds a signed amount in an unsigned field.
    [Win32Input]::Mouse(0, 0, 0x800, [uint32]([BitConverter]::ToUInt32([BitConverter]::GetBytes([int32](120 * $Notches)), 0)))
    Start-Sleep -Milliseconds 40
}
function Get-CursorPosition {
    $p = New-Object Win32Input+POINT
    [void][Win32Input]::GetCursorPos([ref]$p)
    return @($p.X, $p.Y)
}
# A release is always sent: a button left down would be worse wherever focus went.
function Mouse-Button { param([bool]$Down)
    if ($Down) { Assert-EditorFocused }
    [Win32Input]::Mouse(0, 0, $(if ($Down) { 2 } else { 4 }))
}

# Relative movement, as a real mouse: this is what turns the player's view. Sent in small steps over Milliseconds.
function Move-Relative { param([int]$Dx, [int]$Dy, [int]$Milliseconds = 300)
    $steps = [Math]::Max(1, [int]($Milliseconds / 16))
    $doneX = 0; $doneY = 0
    for ($i = 1; $i -le $steps; $i++) {
        $x = [int]($Dx * $i / $steps) - $doneX; $y = [int]($Dy * $i / $steps) - $doneY
        $doneX += $x; $doneY += $y
        Assert-EditorFocused
        [Win32Input]::Mouse($x, $y, 1)
        Start-Sleep -Milliseconds 16
    }
}

function Key-Down { param([string]$Key) Assert-EditorFocused; [Win32Input]::Key((Get-VirtualKey $Key), $false) }
function Key-Up { param([string]$Key) [Win32Input]::Key((Get-VirtualKey $Key), $true) }
function Press-Key { param([string]$Key, [int]$Milliseconds = 60) Key-Down $Key; Start-Sleep -Milliseconds $Milliseconds; Key-Up $Key; Start-Sleep -Milliseconds 80 }
function Hold-Key { param([string[]]$Keys, [int]$Milliseconds)
    foreach ($k in $Keys) { Key-Down $k }
    Start-Sleep -Milliseconds $Milliseconds
    foreach ($k in $Keys) { Key-Up $k }
    Start-Sleep -Milliseconds 60
}
function Get-VirtualKey { param([string]$Key)
    switch ($Key) {
        'Space' { return [uint16]0x20 } 'Shift' { return [uint16]0xA0 } 'Esc' { return [uint16]0x1B } 'Enter' { return [uint16]0x0D }
        'Ctrl' { return [uint16]0xA2 } 'Delete' { return [uint16]0x2E } 'Alt' { return [uint16]0xA4 } 'End' { return [uint16]0x23 } 'F1' { return [uint16]0x70 } 'F2' { return [uint16]0x71 } 'F8' { return [uint16]0x77 } 'F11' { return [uint16]0x7A }
        'Up' { return [uint16]0x26 } 'Down' { return [uint16]0x28 } 'Tab' { return [uint16]0x09 } 'Slash' { return [uint16]0xBF } 'Backspace' { return [uint16]0x08 }
        'Grave' { return [uint16]0xC0 } 'LeftBracket' { return [uint16]0xDB } 'RightBracket' { return [uint16]0xDD }
        default { return [uint16][char]$Key.ToUpper() }
    }
}
function Type-Text { param([string]$Text)
    foreach ($c in $Text.ToCharArray()) {
        Assert-EditorFocused
        [Win32Input]::Character($c)
        Start-Sleep -Milliseconds 15
    }
}

# Saves the window's client area as it is on the screen: what a person sees, not a GPU readback.
function Save-Capture { param([string]$Path)
    Update-Window
    $bitmap = New-Object System.Drawing.Bitmap $script:width, $script:height
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    $graphics.CopyFromScreen($script:origin.X, $script:origin.Y, 0, 0, $bitmap.Size)
    $graphics.Dispose()
    $extension = [IO.Path]::GetExtension($Path).ToLower()
    if ($extension -eq '.jpg') {
        $codec = [System.Drawing.Imaging.ImageCodecInfo]::GetImageEncoders() | Where-Object { $_.MimeType -eq 'image/jpeg' }
        $parameters = New-Object System.Drawing.Imaging.EncoderParameters 1
        $parameters.Param[0] = New-Object System.Drawing.Imaging.EncoderParameter ([System.Drawing.Imaging.Encoder]::Quality), 82L
        $bitmap.Save($Path, $codec, $parameters)
    } else { $bitmap.Save($Path, [System.Drawing.Imaging.ImageFormat]::Png) }
    $bitmap.Dispose()
}

function Test-EditorRunning { $script:editor.Refresh(); return -not $script:editor.HasExited }

function Stop-Editor { param([int]$TimeoutMilliseconds = 15000)
    if ($script:editor.HasExited) { return $script:editor.ExitCode }
    [void]$script:editor.CloseMainWindow()
    if (-not $script:editor.WaitForExit($TimeoutMilliseconds)) { return $null }
    return $script:editor.ExitCode
}

function Kill-Editor { if ($script:editor -and -not $script:editor.HasExited) { $script:editor.Kill(); [void]$script:editor.WaitForExit(5000) } }

Export-ModuleMember -Function *
