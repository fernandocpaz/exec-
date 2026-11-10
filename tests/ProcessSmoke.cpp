// SPDX-License-Identifier: GPL-3.0-or-later
#include "Process.h"

#include <chrono>
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
            throw std::runtime_error("Could not launch or read Windows PowerShell");
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
    throw std::runtime_error("Timed out waiting for Windows PowerShell");
}

void require(bool condition, const char* error) {
    if (!condition) {
        throw std::runtime_error(error);
    }
}

} // namespace

int main() {
    try {
        ProcessRun success("#" + std::string(70000, 'x') +
                           "\r\nWrite-Output 'hello, día 🌴'\r\n"
                           "[Console]::Error.WriteLine('stderr marker')\r\nexit 17", {});
        const auto finished = waitFor(success);
        require(finished.exitCode == 17, "Explicit PowerShell exit code was not preserved");
        require(finished.output.find("hello, día 🌴") != std::string::npos,
                "Multiline or Unicode selection output was lost");
        require(finished.output.find("stderr marker") != std::string::npos,
                "Standard error was not captured");

        ProcessRun fail("throw 'expected error'", {});
        const auto failure = waitFor(fail);
        require(failure.exitCode != 0 && failure.output.find("expected error") != std::string::npos,
                "PowerShell error text or exit status was lost");

        ProcessRun noisy("[Console]::Write('x' * (2 * 1024 * 1024))", {});
        const auto capped = waitFor(noisy);
        require(capped.exitCode == 0 && capped.truncated && capped.output.size() == ProcessRun::outputLimit,
                "Output did not stop at the display limit");

        ProcessRun slow("Write-Output 'ready to stop'; Start-Sleep -Seconds 60", {});
        require(waitFor(slow, true).stopped, "Stop did not terminate PowerShell");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
