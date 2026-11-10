# exec++

exec++ is a Notepad++ plugin that runs the highlighted text in Windows PowerShell and shows the live output in a resizable pane below the editor. The pane combines standard output and standard error and shows the exit code when the command finishes.

## Install on Windows x64

1. Close Notepad++.
2. Copy the `exec++` folder containing `exec++.dll` into your Notepad++ `plugins` directory. The usual installed path is `C:\Program Files\Notepad++\plugins\exec++\exec++.dll`. For a portable copy, use the `plugins` folder next to `notepad++.exe`.
3. Start Notepad++.

The DLL and its parent folder both use the name `exec++`. Use the 64-bit x64 edition of Notepad++ with the packaged DLL.

## Run a selection

Paste and highlight these two lines, right-click the highlighted text, and choose **exec++**.

```powershell
Write-Output "Hello from exec++"
Get-Date
```

**Stop** ends the current run. **Plugins → exec++ → Show Output** reopens the pane. Each run starts a fresh Windows PowerShell session and uses the active file's folder when available. To run a Command Prompt built-in, select a PowerShell command such as `cmd.exe /c ver`.

The pane captures up to 1 MiB per run and supports selecting and copying the output. Commands run locally with your Windows account's permissions. Interactive prompts and persistent shell sessions are outside the scope of this version.

## Build from source

Use Visual Studio 2022 or newer with the **Desktop development with C++** workload and CMake.

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

The DLL is at `build\Release\exec++.dll`. The smoke test exercises the Windows PowerShell runner with a long Unicode selection, standard error, an explicit exit code, output limits, and cancellation.

The Notepad++ and Scintilla interface headers are vendored from the Notepad++ repository at commit `37482a9bf3efb2e38a6e6b4c8d759533a0b329dc`. See the root license and the Scintilla license in `vendor/scintilla`.
