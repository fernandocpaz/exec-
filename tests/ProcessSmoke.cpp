// SPDX-License-Identifier: GPL-3.0-or-later
#include "ProcessRun.h"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>

namespace {

using namespace std::chrono_literals;

RunUpdate waitFor(ProcessRun& run, bool stopWhenStarted = false) {
    const auto deadline = std::chrono::steady_clock::now() + 20s;
    std::string output;
    while (std::chrono::steady_clock::now() < deadline) {
        auto update = run.takeUpdate();
        output += update.output;
        if (!update.error.empty()) {
            std::wcerr << update.error << L'\n';
            throw std::runtime_error("Could not launch or read the shell");
        }
        if (update.finished) {
            update.output = std::move(output);
            return update;
        }
        if (stopWhenStarted && output.find("ready to stop") != std::string::npos) {
            run.stop();
        }
        std::this_thread::sleep_for(20ms);
    }
    throw std::runtime_error("Timed out waiting for the shell");
}

void require(bool condition, const char* error) {
    if (!condition) {
        throw std::runtime_error(error);
    }
}

void checkCommandText() {
    const auto ps = Shell::WindowsPowerShell;
    const auto cmd = Shell::CommandPrompt;
    const auto join = InputMode::JoinLines;
    const auto escaped = InputMode::JoinEscapedLines;
    require(prepareCommand("curl.exe `\r\n  -i\n\t --silent\r URL", ps, join) ==
                "curl.exe -i --silent URL",
            "CRLF, LF, CR, and the PowerShell continuation should join");
    require(prepareCommand("curl.exe ^\r\n -i \\\n  URL", cmd, join) == "curl.exe -i URL",
            "CMD and Bash-style continuations should join");
    require(prepareCommand("curl.exe \"https://example.test/long\r\n  -url\"", ps, join) ==
                "curl.exe \"https://example.test/long-url\"",
            "A line break inside a quoted URL should not insert a space");
    require(prepareCommand("curl.exe \\r\\n -i \\n C:\\new '\\r'", ps, escaped) ==
                "curl.exe -i C:\\new '\\r'",
            "Only separated escape tokens outside quotes should join");
    require(prepareCommand("echo C:\\ \\n next", cmd, join) == "echo C:\\ \\n next",
            "Ordinary Join lines should leave escaped text and paths alone");
    require(prepareCommand("echo C:\\\n next", cmd, join) == "echo C:\\ next",
            "A path ending in backslash should not be treated as a continuation");
    require(prepareCommand("one\r\ntwo", ps, InputMode::Script) == "one\r\ntwo",
            "Script mode must preserve original input");
    const auto wsl = Shell::Wsl;
    require(prepareCommand("one\r\ntwo\rthree\n", wsl, InputMode::Script) == "one\ntwo\nthree\n",
            "WSL script mode must normalize Windows line endings");
    require(prepareCommand("printf '%s' \"hel\\\r\nlo\"", wsl, join) == "printf '%s' \"hello\"",
            "Bash continuations inside a quoted argument should join without spaces");
    require(prepareCommand("printf '%s' \\\r\n  'hello'", wsl, join) == "printf '%s' 'hello'",
            "Bash continuations between arguments should join with a space");

    const auto json = prettyJsonBody(L"HTTP/1.1 100 Continue\r\n\r\n"
                                    L"HTTP/2 200 OK\nContent-Type: application/json\n\n"
                                    L"{\"a\":[1,true,\"x\\n\"],\"a\":-5e+2}\r\n");
    require(json && *json == L"{\r\n  \"a\": [\r\n    1,\r\n    true,\r\n    \"x\\n\"\r\n"
                              L"  ],\r\n  \"a\": -5e+2\r\n}",
            "JSON should strip HTTP headers and preserve escapes, numbers, and duplicate keys");
    const auto encoded = prettyJsonBody(LR"json("\n{\"name\":\"caf\u00e9\",\"line\":\"one\\ntwo\"}\r\n")json");
    require(encoded && *encoded == L"{\r\n  \"name\": \"caf\u00e9\",\r\n  \"line\": \"one\\ntwo\"\r\n}",
            "JSON should decode one string layer and preserve inner JSON escapes");
    require(prettyJsonBody(LR"json("[1,2]")json") == prettyJsonBody(L"[1,2]"),
            "JSON should also unwrap an encoded array");
    require(!prettyJsonBody(L"{\"a\":01}") && !prettyJsonBody(L"[1,]") &&
                !prettyJsonBody(L"prefix {\"a\":1}") && !prettyJsonBody(L"{\"a\":1} trailing") &&
                !prettyJsonBody(L"\"just text\"") && !prettyJsonBody(L"\"[1,]\"") &&
                !prettyJsonBody(LR"json("\u00zz")json"),
            "JSON formatting should reject incomplete, invalid, and mixed output");
}

void checkPowerShell7() {
    ProcessRun modern("Write-Output ('Version ' + $PSVersionTable.PSVersion.Major); exit 19", {},
                      Shell::PowerShell7);
    const auto result = waitFor(modern);
    require(result.exitCode == 19 && result.output.find("Version 7") != std::string::npos,
            "PowerShell 7 did not run the selection");
}

void checkWsl() {
    const auto shell = Shell::Wsl;
    const auto script = prepareCommand("#" + std::string(150000, 'x') +
        "\r\ntest -f CMakeLists.txt || exit 98\r\nprintf 'hello, día 🌴\\n'\r\n"
        "printf 'stderr marker\\n' >&2\r\nexit 27", shell, InputMode::Script);
    ProcessRun wsl(script, std::filesystem::current_path().wstring(), shell);
    const auto result = waitFor(wsl);
    require(result.exitCode == 27 && result.output.find("hello, día 🌴") != std::string::npos &&
                result.output.find("stderr marker") != std::string::npos,
            "WSL did not preserve the directory, multiline input, Unicode, stderr, or exit status");

    const auto marker = "/tmp/execpp-stop-" + std::to_string(GetCurrentProcessId());
    ProcessRun slow("rm -f " + marker + "; (trap '' TERM; sleep 4; touch " + marker +
                        ") & printf 'ready to stop\\n'; wait", {}, shell);
    require(waitFor(slow, true).stopped, "Stop did not cancel the WSL run");

    ProcessRun check("sleep 4; test ! -e " + marker, {}, shell);
    require(waitFor(check).exitCode == 0, "A child of the stopped WSL command survived cancellation");
}

} // namespace

int main(int argc, char* argv[]) {
    try {
        if (argc > 1 && std::string_view(argv[1]) == "--wsl") {
            checkWsl();
            return 0;
        }
        if (argc > 1 && std::string_view(argv[1]) == "--pwsh") {
            checkPowerShell7();
            return 0;
        }
        checkCommandText();
        if (argc > 1 && std::string_view(argv[1]) == "--text") {
            return 0;
        }
        ProcessRun success("#" + std::string(70000, 'x') +
                           "\r\nWrite-Output 'hello, día 🌴'\r\n"
                           "[Console]::Error.WriteLine('stderr marker')\r\nexit 17", {});
        const auto finished = waitFor(success);
        require(finished.exitCode == 17, "Explicit PowerShell exit code was not preserved");
        require(finished.output.find("hello, día 🌴") != std::string::npos,
                "Multiline or Unicode selection output was lost");
        require(finished.output.find("stderr marker") != std::string::npos,
                "Standard error was not captured");

        ProcessRun curl("curl --version", {});
        const auto curlVersion = waitFor(curl);
        require(curlVersion.exitCode == 0 && curlVersion.output.find("libcurl/") != std::string::npos,
                "Windows PowerShell resolved curl to a web cmdlet instead of curl.exe");

        ProcessRun fail("throw 'expected error'", {});
        const auto failure = waitFor(fail);
        require(failure.exitCode != 0 && failure.output.find("expected error") != std::string::npos,
                "PowerShell error text or exit status was lost");

        ProcessRun noisy("[Console]::Write('x' * (2 * 1024 * 1024))", {});
        const auto capped = waitFor(noisy);
        require(capped.exitCode == 0 && capped.truncated && capped.output.size() == ProcessRun::outputLimit,
                "Output did not stop at the display limit");

        const auto oneLine = prepareCommand("echo \"two words\" ^\r\n & exit /b 23", Shell::CommandPrompt,
                                            InputMode::JoinLines);
        ProcessRun commandPrompt(oneLine, {}, Shell::CommandPrompt);
        const auto cmd = waitFor(commandPrompt);
        require(cmd.exitCode == 23 && cmd.output.find("\"two words\"") != std::string::npos,
                "CMD did not preserve quotes and execute the whole command line");

        ProcessRun slow("Write-Output 'ready to stop'; Start-Sleep -Seconds 60", {});
        require(waitFor(slow, true).stopped, "Stop did not terminate PowerShell");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
