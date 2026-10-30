# Real mouse and keyboard input for acceptance runs: SendInput events to a window owned by a process we started, and
# captures of the desktop afterwards. These are the same events a person's hands produce, so they go through GLFW,
# ImGui and the game exactly as play does. Needs an unlocked, interactive desktop, and moves the real mouse.

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
    public static void Mouse(int dx, int dy, uint flags, uint data = 0) {
        var i = new INPUT { type = 0 }; i.u.mi = new MOUSEINPUT { dx = dx, dy = dy, mouseData = data, dwFlags = flags };
        SendInput(1, new[] { i }, Marshal.SizeOf(typeof(INPUT)));
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
    # Windows only lets the foreground process hand over focus; a key press beforehand qualifies this one.
    [Win32Input]::Key(0x12, $false); [Win32Input]::Key(0x12, $true)
    [void][Win32Input]::ShowWindow($script:window, 5)
    [void][Win32Input]::SetForegroundWindow($script:window)
    Start-Sleep -Milliseconds 300
}

# Positions are fractions of the client area, so a run does not depend on the DPI scale.
function Move-To { param([double]$X, [double]$Y)
    Update-Window
    [void][Win32Input]::SetCursorPos($script:origin.X + [int]($X * $script:width), $script:origin.Y + [int]($Y * $script:height))
    Start-Sleep -Milliseconds 60
}

function Click-At { param([double]$X, [double]$Y, [switch]$Right)
    Move-To $X $Y
    $down, $up = if ($Right) { 8, 16 } else { 2, 4 }
    [Win32Input]::Mouse(0, 0, $down); Start-Sleep -Milliseconds 70
    [Win32Input]::Mouse(0, 0, $up); Start-Sleep -Milliseconds 150
}

function Mouse-Button { param([bool]$Down) [Win32Input]::Mouse(0, 0, $(if ($Down) { 2 } else { 4 })) }

# Relative movement, as a real mouse: this is what turns the player's view. Sent in small steps over Milliseconds.
function Move-Relative { param([int]$Dx, [int]$Dy, [int]$Milliseconds = 300)
    $steps = [Math]::Max(1, [int]($Milliseconds / 16))
    $doneX = 0; $doneY = 0
    for ($i = 1; $i -le $steps; $i++) {
        $x = [int]($Dx * $i / $steps) - $doneX; $y = [int]($Dy * $i / $steps) - $doneY
        $doneX += $x; $doneY += $y
        [Win32Input]::Mouse($x, $y, 1)
        Start-Sleep -Milliseconds 16
    }
}

function Key-Down { param([string]$Key) [Win32Input]::Key((Get-VirtualKey $Key), $false) }
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
        'Ctrl' { return [uint16]0xA2 } 'Delete' { return [uint16]0x2E }
        default { return [uint16][char]$Key.ToUpper() }
    }
}
function Type-Text { param([string]$Text)
    foreach ($c in $Text.ToCharArray()) {
        $code = [int][char]$c
        $i = New-Object Win32Input+INPUT; $i.type = 1
        $k = New-Object Win32Input+KEYBDINPUT; $k.wScan = [uint16]$code; $k.dwFlags = 4
        $i.u.ki = $k
        $size = [System.Runtime.InteropServices.Marshal]::SizeOf([type][Win32Input+INPUT])
        [void][Win32Input]::SendInput(1, @($i), $size)
        $k.dwFlags = 6; $i.u.ki = $k
        [void][Win32Input]::SendInput(1, @($i), $size)
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
