# SPDX-License-Identifier: GPL-3.0-or-later
param([Parameter(Mandatory)] [string] $Notepad, [Parameter(Mandatory)] [string] $Plugin)
$ErrorActionPreference = 'Stop'

Add-Type @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;

public static class NotepadTest {
    public delegate bool EnumProc(IntPtr window, IntPtr param);
    [StructLayout(LayoutKind.Sequential)]
    public struct Rect { public int Left, Top, Right, Bottom; }
    [StructLayout(LayoutKind.Sequential)]
    private struct MenuBarInfo {
        public int Size;
        public Rect Bar;
        public IntPtr Menu, MenuWindow;
        public uint Flags;
    }

    [DllImport("user32")] private static extern bool EnumWindows(EnumProc proc, IntPtr param);
    [DllImport("user32")] private static extern bool EnumChildWindows(IntPtr parent, EnumProc proc, IntPtr param);
    [DllImport("user32")] private static extern uint GetWindowThreadProcessId(IntPtr window, out uint process);
    [DllImport("user32")] private static extern bool IsWindowVisible(IntPtr window);
    [DllImport("user32", CharSet = CharSet.Unicode)]
    private static extern int GetClassName(IntPtr window, StringBuilder text, int capacity);
    [DllImport("user32", CharSet = CharSet.Unicode)]
    private static extern IntPtr SendMessage(IntPtr window, uint message, IntPtr size, StringBuilder text);
    [DllImport("user32", EntryPoint = "SendMessageW")]
    public static extern IntPtr Send(IntPtr window, uint message, IntPtr w, IntPtr l);
    [DllImport("user32", EntryPoint = "PostMessageW")]
    public static extern bool Post(IntPtr window, uint message, IntPtr w, IntPtr l);
    [DllImport("user32")] public static extern bool SetForegroundWindow(IntPtr window);
    [DllImport("user32")] private static extern bool GetWindowRect(IntPtr window, out Rect rect);
    [DllImport("user32")] private static extern bool GetMenuBarInfo(IntPtr window, int objectId, int item, ref MenuBarInfo info);
    [DllImport("user32")] private static extern int GetMenuItemCount(IntPtr menu);
    [DllImport("user32")] private static extern uint GetMenuState(IntPtr menu, uint item, uint flags);
    [DllImport("user32")] private static extern uint GetMenuItemID(IntPtr menu, int position);
    [DllImport("user32", CharSet = CharSet.Unicode)]
    private static extern int GetMenuString(IntPtr menu, uint item, StringBuilder text, int capacity, uint flags);
    [DllImport("user32")] public static extern IntPtr GetDlgItem(IntPtr parent, int id);

    public static IntPtr[] Windows(IntPtr parent, uint pid, string windowClass) {
        var matches = new List<IntPtr>();
        EnumProc visit = (window, param) => {
            uint process;
            GetWindowThreadProcessId(window, out process);
            var name = new StringBuilder(128);
            GetClassName(window, name, name.Capacity);
            if (process == pid && IsWindowVisible(window) && name.ToString() == windowClass)
                matches.Add(window);
            return true;
        };
        if (parent == IntPtr.Zero) EnumWindows(visit, IntPtr.Zero);
        else EnumChildWindows(parent, visit, IntPtr.Zero);
        return matches.ToArray();
    }

    public static string Text(IntPtr window) {
        var text = new StringBuilder(4096);
        SendMessage(window, 0x000D, new IntPtr(text.Capacity), text); // WM_GETTEXT
        return text.ToString();
    }

    public static Rect Bounds(IntPtr window) {
        Rect result;
        if (!GetWindowRect(window, out result)) throw new InvalidOperationException("Window has no bounds.");
        return result;
    }

    public static int MenuCommand(IntPtr popup, string label, bool expectedEnabled) {
        var info = new MenuBarInfo { Size = Marshal.SizeOf<MenuBarInfo>() };
        if (!GetMenuBarInfo(popup, -4, 0, ref info)) return 0; // OBJID_CLIENT
        for (var i = 0; i < GetMenuItemCount(info.Menu); ++i) {
            var text = new StringBuilder(256);
            GetMenuString(info.Menu, (uint)i, text, text.Capacity, 0x400); // MF_BYPOSITION
            if (text.ToString() != label) continue;
            var id = GetMenuItemID(info.Menu, i);
            var enabled = (GetMenuState(info.Menu, id, 0) & 3) == 0;
            if (enabled != expectedEnabled) throw new InvalidOperationException("Unexpected menu state for " + label);
            return (int)id;
        }
        return 0;
    }
}
'@

function Wait-For([scriptblock] $Condition, [string] $Description) {
    $deadline = [DateTime]::UtcNow.AddSeconds(20)
    do {
        $result = & $Condition
        if ($result) { return $result }
        if ($process.HasExited) { throw "Notepad++ exited while waiting for $Description." }
        Start-Sleep -Milliseconds 100
    } until ([DateTime]::UtcNow -ge $deadline)
    throw "Timed out waiting for $Description."
}

function Get-ContextCommand($editor, [string] $label, [bool] $enabled) {
    [void] [NotepadTest]::Post($notepadWindow, 0x007B, $editor, [IntPtr](-1)) # WM_CONTEXTMENU
    try {
        return Wait-For {
            foreach ($menu in [NotepadTest]::Windows([IntPtr]::Zero, $process.Id, '#32768')) {
                $command = [NotepadTest]::MenuCommand($menu, $label, $enabled)
                if ($command) { return $command }
            }
        } "the editor context menu item '$label'"
    } finally {
        [void] [NotepadTest]::Post($notepadWindow, 0x001F, [IntPtr]::Zero, [IntPtr]::Zero) # WM_CANCELMODE
        [void] (Wait-For {
            if (![NotepadTest]::Windows([IntPtr]::Zero, $process.Id, '#32768').Length) { $true }
        } 'the context menu to close')
    }
}

$exe = Get-Item -LiteralPath $Notepad
$pluginFolder = Join-Path $exe.DirectoryName 'plugins/exec++'
[void] (New-Item -ItemType Directory -Path $pluginFolder -Force)
Copy-Item -LiteralPath $Plugin -Destination (Join-Path $pluginFolder 'exec++.dll')
$fixture = Join-Path $exe.DirectoryName 'execpp-smoke.ps1'
$firstCommand = "Write-Output`r`n  '{`"items`":[1,2]}'"
$secondCommand = "Write-Output 'second view marker'"
$commands = $firstCommand + "`r`n" + $secondCommand
[IO.File]::WriteAllText($fixture, $commands, [Text.UTF8Encoding]::new($false))
$process = Start-Process -FilePath $exe.FullName -WorkingDirectory $exe.DirectoryName -PassThru -ArgumentList '-multiInst', '-nosession', "`"$fixture`""
try {
    $notepadWindow = Wait-For {
        [NotepadTest]::Windows([IntPtr]::Zero, $process.Id, 'Notepad++') |
            Where-Object { [NotepadTest]::Text($_).Contains('execpp-smoke.ps1') } |
            Select-Object -First 1
    } 'the Notepad++ test document'
    [void] [NotepadTest]::SetForegroundWindow($notepadWindow)

    $editor = Wait-For {
        [NotepadTest]::Windows($notepadWindow, $process.Id, 'Scintilla') | Select-Object -First 1
    } 'the main editor'
    [void] (Get-ContextCommand $editor 'exec++' $false)
    [void] [NotepadTest]::Send($editor, 2160, [IntPtr]::Zero, [IntPtr] $firstCommand.Length) # SCI_SETSEL
    $join = Get-ContextCommand $editor 'exec++ (Join lines)' $true
    [void] [NotepadTest]::Post($notepadWindow, 0x0111, [IntPtr] $join, [IntPtr]::Zero) # WM_COMMAND

    $panel = Wait-For {
        [NotepadTest]::Windows($notepadWindow, $process.Id, 'ExecPlusPlusOutput') | Select-Object -First 1
    } 'the docked output pane'
    $status = [NotepadTest]::GetDlgItem($panel, 0)
    $output = [NotepadTest]::GetDlgItem($panel, 111)
    $json = [NotepadTest]::GetDlgItem($panel, 108)
    if ([NotepadTest]::Bounds($panel).Top -lt [NotepadTest]::Bounds($editor).Bottom) {
        throw 'The output pane is not docked below the editor.'
    }
    [void] (Wait-For {
        if ([NotepadTest]::Text($output).Contains('  "items": [') -and
            [NotepadTest]::Text($status).Contains('Exit code: 0') -and
            [NotepadTest]::Text($json) -eq 'View Raw') { $true }
    } 'automatic JSON formatting after a successful exit')
    [void] [NotepadTest]::Send($json, 0x00F5, [IntPtr]::Zero, [IntPtr]::Zero) # BM_CLICK
    [void] (Wait-For {
        if ([NotepadTest]::Text($output).Contains('{"items":[1,2]}') -and
            [NotepadTest]::Text($json) -eq 'View JSON') { $true }
    } 'the restored raw output')
    [void] [NotepadTest]::Send($json, 0x00F5, [IntPtr]::Zero, [IntPtr]::Zero)
    [void] (Wait-For {
        if ([NotepadTest]::Text($output).Contains('  "items": [')) { $true }
    } 'the formatted JSON view')
    [void] [NotepadTest]::Send($json, 0x00F5, [IntPtr]::Zero, [IntPtr]::Zero)

    [void] [NotepadTest]::Post($notepadWindow, 2072, [IntPtr]::Zero, [IntPtr] 10002) # Clone to other view
    $otherEditor = Wait-For {
        [NotepadTest]::Windows($notepadWindow, $process.Id, 'Scintilla') |
            Where-Object { $_ -ne $editor } |
            Select-Object -First 1
    } 'the second editor view'
    [void] [NotepadTest]::Send($otherEditor, 2160, [IntPtr] ($firstCommand.Length + 2), [IntPtr] $commands.Length)
    $run = Get-ContextCommand $otherEditor 'exec++' $true
    [void] [NotepadTest]::Post($notepadWindow, 0x0111, [IntPtr] $run, [IntPtr]::Zero)
    [void] (Wait-For {
        if ([NotepadTest]::Text($output).Contains('second view marker') -and
            [NotepadTest]::Text($status).Contains('Exit code: 0')) { $true }
    } 'output from the second editor view')
    $history = [NotepadTest]::GetDlgItem($panel, 104)
    [void] [NotepadTest]::Send($history, 0x014E, [IntPtr] 2, [IntPtr]::Zero) # CB_SETCURSEL
    [void] [NotepadTest]::Send($panel, 0x0111, [IntPtr] 65640, $history) # CBN_SELCHANGE
    [void] (Wait-For {
        if ([NotepadTest]::Text($output).Contains('{"items":[1,2]}')) { $true }
    } 'the previous run and its raw view from history')
    Write-Host 'Notepad++ editor menus, bottom docking, line joining, automatic JSON, and history passed.'
} finally {
    if (!$process.HasExited) {
        [void] $process.CloseMainWindow()
        if (!$process.WaitForExit(3000)) { Stop-Process -Id $process.Id -Force }
    }
}
