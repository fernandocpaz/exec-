// SPDX-License-Identifier: GPL-3.0-or-later
#include "ProcessRun.h"
#include "WindowsText.h"

#include <algorithm>
#include <array>
#include <memory>
#include <utility>

namespace {

struct CloseHandleOnExit {
    void operator()(void* handle) const { CloseHandle(handle); }
};

using Handle = std::unique_ptr<void, CloseHandleOnExit>;

struct WindowsError {
    const wchar_t* operation;
    DWORD code = GetLastError();
};

std::wstring describe(WindowsError error) {
    wchar_t* message = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                       FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, error.code, 0, reinterpret_cast<wchar_t*>(&message), 0, nullptr);
    std::unique_ptr<wchar_t, decltype(&LocalFree)> ownedMessage(message, LocalFree);
    std::wstring detail = message ? message : L"";
    if (const auto end = detail.find_last_not_of(L"\r\n "); end != std::wstring::npos) {
        detail.resize(end + 1);
    }
    return std::wstring(error.operation) + L" (Windows error " + std::to_wstring(error.code) +
           L")" + (detail.empty() ? L"" : L". " + detail);
}

struct Pipe {
    Handle read;
    Handle write;
};

Pipe createPipe() {
    SECURITY_ATTRIBUTES attributes{sizeof(attributes), nullptr, TRUE};
    HANDLE read = nullptr;
    HANDLE write = nullptr;
    if (!CreatePipe(&read, &write, &attributes, 0)) {
        throw WindowsError{L"Could not create a shell pipe"};
    }
    return {Handle(read), Handle(write)};
}

struct DeleteAttributes {
    void operator()(LPPROC_THREAD_ATTRIBUTE_LIST attributes) const { DeleteProcThreadAttributeList(attributes); }
};

class ChildHandles {
public:
    explicit ChildHandles(std::array<HANDLE, 2> handles) : handles_(handles) {
        SIZE_T bytes = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
        storage_.reset(LocalAlloc(LMEM_FIXED, bytes));
        if (!storage_) {
            throw WindowsError{L"Could not allocate shell startup attributes", ERROR_OUTOFMEMORY};
        }
        auto* attributes = static_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage_.get());
        if (!InitializeProcThreadAttributeList(attributes, 1, 0, &bytes)) {
            throw WindowsError{L"Could not initialize shell startup attributes"};
        }
        list_.reset(attributes);
        if (!UpdateProcThreadAttribute(list_.get(), 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                       handles_.data(), sizeof(handles_), nullptr, nullptr)) {
            throw WindowsError{L"Could not configure shell handle inheritance"};
        }
    }

    LPPROC_THREAD_ATTRIBUTE_LIST get() const { return list_.get(); }

private:
    std::array<HANDLE, 2> handles_;
    std::unique_ptr<void, decltype(&LocalFree)> storage_{nullptr, LocalFree};
    std::unique_ptr<_PROC_THREAD_ATTRIBUTE_LIST, DeleteAttributes> list_;
};

std::wstring environmentValue(const wchar_t* name) {
    const DWORD length = GetEnvironmentVariableW(name, nullptr, 0);
    if (!length) {
        return {};
    }
    std::wstring value(length, L'\0');
    const DWORD copied = GetEnvironmentVariableW(name, value.data(), length);
    if (copied >= length) {
        return {};
    }
    value.resize(copied);
    return value;
}

std::wstring shellPath(Shell shell) {
    if (shell == Shell::PowerShell7) {
        const auto programFiles = environmentValue(L"ProgramFiles");
        if (!programFiles.empty()) {
            const auto installed = programFiles + L"\\PowerShell\\7\\pwsh.exe";
            const DWORD attributes = GetFileAttributesW(installed.c_str());
            if (attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY)) {
                return installed;
            }
        }
        const auto path = environmentValue(L"PATH");
        std::array<wchar_t, 32768> resolved{};
        if (!path.empty()) {
            const DWORD length = SearchPathW(path.c_str(), L"pwsh.exe", nullptr,
                                             static_cast<DWORD>(resolved.size()), resolved.data(), nullptr);
            if (length && length < resolved.size()) {
                return std::wstring(resolved.data(), length);
            }
        }
        throw L"PowerShell 7 was not found. Install it or select a different shell.";
    }

    wchar_t systemDirectory[MAX_PATH]{};
    const UINT length = GetSystemDirectoryW(systemDirectory, MAX_PATH);
    if (!length || length >= MAX_PATH) {
        throw WindowsError{L"Could not find the Windows system directory"};
    }
    const std::wstring systemPath(systemDirectory, length);
    if (shell == Shell::Wsl) {
        const auto wsl = systemPath + L"\\wsl.exe";
        if (GetFileAttributesW(wsl.c_str()) == INVALID_FILE_ATTRIBUTES) {
            throw L"WSL was not found. Install WSL and a Linux distribution with Bash.";
        }
        return wsl;
    }
    return systemPath + (shell == Shell::CommandPrompt ? L"\\cmd.exe" : L"\\WindowsPowerShell\\v1.0\\powershell.exe");
}

std::wstring wslEnvironment() {
    std::unique_ptr<wchar_t, decltype(&FreeEnvironmentStringsW)> source(GetEnvironmentStringsW(), FreeEnvironmentStringsW);
    if (!source) {
        throw WindowsError{L"Could not read the environment for WSL"};
    }
    // wsl.exe emits UTF-16 diagnostics by default. Use UTF-8 for this child only.
    constexpr std::wstring_view setting = L"WSL_UTF8";
    constexpr wchar_t utf8[] = L"WSL_UTF8=1";
    std::wstring environment;
    bool added = false;
    for (const wchar_t* current = source.get(); *current;) {
        const std::wstring_view entry(current);
        const auto name = entry.substr(0, entry.find(L'=', 1));
        const int order = CompareStringOrdinal(name.data(), static_cast<int>(name.size()),
                                               setting.data(), static_cast<int>(setting.size()), TRUE);
        if (!added && order >= CSTR_EQUAL) {
            environment.append(utf8, std::size(utf8));
            added = true;
        }
        if (order != CSTR_EQUAL) {
            environment.append(current, entry.size() + 1);
        }
        current += entry.size() + 1;
    }
    if (!added) {
        environment.append(utf8, std::size(utf8));
    }
    environment += L'\0';
    return environment;
}

std::wstring quoteWindowsArgument(std::wstring_view argument) {
    std::wstring quoted = L"\"";
    std::size_t backslashes = 0;
    for (const wchar_t character : argument) {
        if (character == L'\\') {
            ++backslashes;
            continue;
        }
        quoted.append(character == L'"' ? backslashes * 2 + 1 : backslashes, L'\\');
        quoted += character;
        backslashes = 0;
    }
    quoted.append(backslashes * 2, L'\\');
    quoted += L'"';
    return quoted;
}

std::wstring wslDirectoryArguments(std::wstring_view directory) {
    if (directory.empty()) {
        return L" --cd ~";
    }
    // The process working directory is enough for Windows drive paths. For WSL
    // UNC paths, select the owning distro and give it the native Linux path.
    for (const auto prefix : {std::wstring_view(L"\\\\wsl$\\"), std::wstring_view(L"\\\\wsl.localhost\\")}) {
        if (directory.size() <= prefix.size() ||
            CompareStringOrdinal(directory.data(), static_cast<int>(prefix.size()),
                                 prefix.data(), static_cast<int>(prefix.size()), TRUE) != CSTR_EQUAL) {
            continue;
        }
        const auto path = directory.substr(prefix.size());
        const auto separator = path.find(L'\\');
        const auto distro = path.substr(0, separator);
        if (distro.empty()) {
            break;
        }
        auto linuxPath = separator == std::wstring_view::npos ? std::wstring(L"/") : std::wstring(path.substr(separator));
        std::replace(linuxPath.begin(), linuxPath.end(), L'\\', L'/');
        return L" --distribution " + quoteWindowsArgument(distro) + L" --cd " + quoteWindowsArgument(linuxPath);
    }
    return {};
}

// The source goes over stdin so quotes and multiline selections reach PowerShell verbatim.
constexpr wchar_t shellArguments[] =
    L" -NoLogo -NoProfile -NonInteractive -Command \""
    L"$utf8 = [System.Text.UTF8Encoding]::new($false); "
    L"$OutputEncoding = $utf8; "
    L"$reader = [IO.StreamReader]::new([Console]::OpenStandardInput(), $utf8); "
    L"$stdout = [IO.StreamWriter]::new([Console]::OpenStandardOutput(), $utf8); $stdout.AutoFlush = $true; [Console]::SetOut($stdout); "
    L"$stderr = [IO.StreamWriter]::new([Console]::OpenStandardError(), $utf8); $stderr.AutoFlush = $true; [Console]::SetError($stderr); "
    L"try { "
    // Windows PowerShell 5.1 shadows native curl with a web cmdlet alias.
    L"if ((Get-Alias -Name curl -ErrorAction Ignore).Definition -eq 'Invoke-WebRequest' "
    L"-and (Get-Command -Name curl.exe -CommandType Application -ErrorAction Ignore)) { "
    L"Remove-Item -LiteralPath Alias:curl -Force -ErrorAction Stop }; "
    L". ([ScriptBlock]::Create($reader.ReadToEnd())); "
    L"if ($?) { exit 0 }; if ($LASTEXITCODE) { exit $LASTEXITCODE }; exit 1 } "
    L"catch { [Console]::Error.WriteLine(($_ | Out-String).TrimEnd()); exit 1 }\"";

// The writer sends exactly $1 bytes. The open pipe then keeps the watcher alive.
// On EOF the watcher signals the script's Linux process group, including children.
constexpr wchar_t wslLauncher[] = LR"bash(
LC_ALL=C IFS= read -r -N "$1" execpp_script || exit 1
set -m
(
    set +m
    set --
    eval "$execpp_script"
) </dev/null &
script_pid=$!
(
    IFS= read -r unused
    kill -TERM -- "-$script_pid" 2>/dev/null || exit 0
    sleep 0.2
    kill -KILL -- "-$script_pid" 2>/dev/null || :
) <&0 &
watcher_pid=$!
wait "$script_pid" 2>/dev/null
status=$?
kill "$watcher_pid" 2>/dev/null || :
wait "$watcher_pid" 2>/dev/null || :
kill -KILL -- "-$script_pid" 2>/dev/null || :
exit "$status"
)bash";

std::wstring buildCommandLine(const std::wstring& executable, const std::string& script,
                              const std::wstring& directory, Shell shell) {
    auto commandLine = L"\"" + executable + L"\"";
    if (shell == Shell::Wsl) {
        return commandLine + wslDirectoryArguments(directory) + L" --exec bash -c " +
               quoteWindowsArgument(wslLauncher) + L" execpp " + std::to_wstring(script.size());
    }
    if (shell != Shell::CommandPrompt) {
        return commandLine + shellArguments;
    }
    const auto text = toWide(script, CP_UTF8);
    const auto first = text.find_first_not_of(L" \t\r\n");
    if (first == std::wstring::npos) {
        throw L"Select a Command Prompt command first.";
    }
    const auto command = text.substr(first, text.find_last_not_of(L" \t\r\n") - first + 1);
    if (command.find_first_of(L"\r\n") != std::wstring::npos) {
        throw L"Command Prompt accepts one line here. Choose Join lines for a wrapped command.";
    }
    commandLine += L" /d /a /s /c \"" + command + L"\"";
    if (commandLine.size() > 8191) {
        throw L"This command exceeds the 8,191-character Command Prompt limit.";
    }
    return commandLine;
}

} // namespace

ProcessRun::ProcessRun(std::string script, std::wstring directory, Shell shell)
    : worker_([this, script = std::move(script), directory = std::move(directory), shell] {
          try {
              run(script, directory, shell);
          } catch (const WindowsError& error) {
              std::lock_guard lock(mutex_);
              update_.error = describe(error);
              update_.finished = true;
          } catch (const wchar_t* error) {
              std::lock_guard lock(mutex_);
              update_.error = error;
              update_.finished = true;
          } catch (...) {
              std::lock_guard lock(mutex_);
              update_.error = L"Could not run the selection";
              update_.finished = true;
          }
      }) {}

ProcessRun::~ProcessRun() {
    stop();
    worker_.join();
}

void ProcessRun::stop() {
    stopRequested_ = true;
}

RunUpdate ProcessRun::takeUpdate() {
    std::lock_guard lock(mutex_);
    return std::exchange(update_, {});
}

void ProcessRun::appendOutput(const char* bytes, std::size_t count) {
    const std::size_t keep = std::min(count, outputLimit - outputSize_);
    outputSize_ += keep;
    if (keep == 0) {
        return;
    }
    std::lock_guard lock(mutex_);
    update_.output.append(bytes, keep);
}

void ProcessRun::run(const std::string& script, const std::wstring& directory, Shell shell) {
    if (stopRequested_) {
        std::lock_guard lock(mutex_);
        update_.stopped = true;
        update_.finished = true;
        return;
    }

    const std::wstring executable = shellPath(shell);
    std::wstring commandLine = buildCommandLine(executable, script, directory, shell);
    auto environment = shell == Shell::Wsl ? wslEnvironment() : std::wstring();
    auto input = createPipe();
    auto output = createPipe();
    if (!SetHandleInformation(input.write.get(), HANDLE_FLAG_INHERIT, 0) ||
        !SetHandleInformation(output.read.get(), HANDLE_FLAG_INHERIT, 0)) {
        throw WindowsError{L"Could not configure shell pipes"};
    }
    Handle wslControl;
    if (shell == Shell::Wsl) {
        const HANDLE self = GetCurrentProcess();
        HANDLE control = nullptr;
        if (!DuplicateHandle(self, input.write.get(), self, &control, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
            throw WindowsError{L"Could not create the WSL cancellation pipe"};
        }
        wslControl.reset(control);
    }

    Handle job(CreateJobObjectW(nullptr, nullptr));
    if (!job) {
        throw WindowsError{L"Could not create a job for the shell"};
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation,
                                 &limits, sizeof(limits))) {
        throw WindowsError{L"Could not configure the shell job"};
    }

    const ChildHandles inherited({input.read.get(), output.write.get()});
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = input.read.get();
    startup.StartupInfo.hStdOutput = output.write.get();
    startup.StartupInfo.hStdError = output.write.get();
    startup.lpAttributeList = inherited.get();
    PROCESS_INFORMATION child{};
    if (!CreateProcessW(executable.c_str(), commandLine.data(), nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT,
                        environment.empty() ? nullptr : environment.data(), directory.empty() ? nullptr : directory.c_str(),
                        &startup.StartupInfo, &child)) {
        throw WindowsError{L"Could not start the selected shell"};
    }

    Handle process(child.hProcess);
    Handle primaryThread(child.hThread);
    if (!AssignProcessToJobObject(job.get(), process.get())) {
        const WindowsError error{L"Could not attach the shell to its job"};
        TerminateProcess(process.get(), ERROR_PROCESS_ABORTED);
        throw error;
    }
    input.read.reset();
    output.write.reset();
    if (ResumeThread(primaryThread.get()) == DWORD(-1)) {
        throw WindowsError{L"Could not resume the selected shell"};
    }

    // The shell can emit startup errors before it reads stdin. Read and write concurrently.
    std::thread writer;
    if (shell == Shell::CommandPrompt) {
        input.write.reset();
    } else {
        writer = std::thread([this, &script, shell, pipe = std::move(input.write)] {
            std::size_t offset = 0;
            while (offset < script.size() && !(shell == Shell::Wsl && stopRequested_)) {
                const DWORD count = static_cast<DWORD>(std::min<std::size_t>(65536, script.size() - offset));
                DWORD written = 0;
                if (!WriteFile(pipe.get(), script.data() + offset, count, &written, nullptr) || !written) {
                    break;
                }
                offset += written;
            }
        });
    }

    bool stopped = false;
    ULONGLONG stopDeadline = 0;
    bool truncated = false;
    DWORD exitCode = 0;
    std::array<char, 8192> bytes{};
    auto readOutput = [&] {
        DWORD ready = 0;
        if (!PeekNamedPipe(output.read.get(), nullptr, 0, nullptr, &ready, nullptr) || ready == 0) {
            return false;
        }
        const DWORD toRead = static_cast<DWORD>(std::min<std::size_t>(ready, bytes.size()));
        DWORD count = 0;
        if (!ReadFile(output.read.get(), bytes.data(), toRead, &count, nullptr) || !count) {
            return false;
        }
        truncated |= count > outputLimit - outputSize_;
        appendOutput(bytes.data(), count);
        return true;
    };

    try {
        for (;;) {
            if (stopRequested_ && !stopped) {
                stopped = true;
                if (shell != Shell::Wsl) {
                    break;
                }
                wslControl.reset();
                stopDeadline = GetTickCount64() + 1500;
            }
            if (stopped && GetTickCount64() >= stopDeadline) {
                break;
            }
            const DWORD state = WaitForSingleObject(process.get(), 0);
            if (state == WAIT_OBJECT_0) {
                break;
            }
            if (state == WAIT_FAILED) {
                throw WindowsError{L"Could not wait for the shell"};
            }
            if (!readOutput()) {
                WaitForSingleObject(process.get(), 15);
            }
        }
        // Close the job before draining so a descendant cannot keep the pipe open indefinitely.
        job.reset();
        if (writer.joinable()) {
            writer.join();
        }
        while (readOutput()) {}
        if (!stopped && !GetExitCodeProcess(process.get(), &exitCode)) {
            throw WindowsError{L"Could not read the shell exit code"};
        }
    } catch (...) {
        wslControl.reset();
        job.reset();
        if (writer.joinable()) {
            writer.join();
        }
        throw;
    }

    std::lock_guard lock(mutex_);
    update_.stopped = stopped;
    update_.exitCode = exitCode;
    update_.truncated = truncated;
    update_.finished = true;
}
