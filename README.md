# exec++

exec++ runs selected text from Notepad++ and shows live output in a docked pane below the editor. It combines standard output and standard error and shows the exit code and elapsed time.

## Install on Windows x64

1. Download and extract the Windows x64 ZIP from [GitHub Releases](https://github.com/fernandocpaz/exec-/releases).
2. Close Notepad++.
3. Copy the `exec++` folder containing `exec++.dll` into the Notepad++ `plugins` directory. For an installed copy, the path is usually `C:\Program Files\Notepad++\plugins\exec++\exec++.dll`. For a portable copy, use the `plugins` folder next to `notepad++.exe`.
4. Start 64-bit x64 Notepad++.

## Run a selection

Highlight a command and press **Ctrl+Shift+Enter** or right-click and choose **exec++**. Change the shortcut under **Settings → Shortcut Mapper → Plugin commands**. **Plugins → exec++ → Show Output** opens the pane so you can choose a shell and input mode before running.

The shell picker offers **Windows PowerShell**, **PowerShell 7** (if installed), and **Command Prompt**. Each run starts a fresh shell in the active file’s folder when available. In Windows PowerShell, `curl` runs native `curl.exe` when it is installed. Use `Invoke-WebRequest` explicitly for the PowerShell cmdlet. CMD accepts a single command line, which can chain commands with `&` or `&&`.

| Input mode | Behavior |
| --- | --- |
| **Script (keep line breaks)** | Keeps the original line endings and indentation for PowerShell scripts. This is the default. |
| **Join lines** | Turns a wrapped command into one line. Handles CRLF, LF, and CR. Removes PowerShell backtick and CMD caret continuations for their respective shells, as well as a pasted Bash-style space followed by a backslash at the end of a line. |
| **Join lines + escaped breaks** | Also recognizes space-separated literal `\r\n`, `\n`, and `\r` outside quotes. Quoted JSON escapes and Windows paths are preserved. |

Choose **exec++ (Join lines)** from the editor context menu to join a selection once without changing the saved mode. A break between arguments becomes a space. A break inside a quoted argument with no preceding space is removed, so a wrapped URL stays intact. Use Script mode when actual line breaks or indentation are part of the data.

For example, select these lines, right-click, and choose **exec++ (Join lines)**.

```powershell
Write-Output
    '{"status":"ok","items":[1,2]}'
```

## Read the output

The pane uses 11-point Consolas, follows Notepad++’s light or dark theme, and wraps long lines by default. Use **Find Next** or Enter in the search box, **Copy All** for the current view, and **Clear** to clear the display. The **Run history** dropdown holds the last ten results in memory and can restore a cleared result. **Stop** ends the active run.

When a run finishes, exec++ automatically formats a complete JSON object or array, including the body of a `curl.exe -sS -i` response or a JSON string that contains an object or array. **View Raw** restores the original output with its headers. Plain text and mixed output stay raw. Output is capped at 1 MiB per run. Shell, input mode, and word wrap are saved across restarts.

## Build from source

Use Visual Studio 2022 or newer with the **Desktop development with C++** workload and CMake.

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

The DLL is at `build\Release\exec++.dll`. The smoke tests cover line joining, JSON formatting, shell output, exit codes, truncation, and cancellation. The PowerShell 7 test is enabled when CMake finds `pwsh`.

A successful push to `main` with a new version in `CMakeLists.txt` publishes a GitHub Release with the tested Windows x64 ZIP.

The Notepad++ and Scintilla interface headers are vendored from the Notepad++ repository at commit `37482a9bf3efb2e38a6e6b4c8d759533a0b329dc`. Their license files are in `vendor`.
