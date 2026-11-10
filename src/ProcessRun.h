#pragma once

#include "CommandText.h"

#include <windows.h>
#include <atomic>
#include <cstddef>
#include <mutex>
#include <string>
#include <thread>

struct RunUpdate {
    std::string output;
    std::wstring error;
    DWORD exitCode = 0;
    bool finished = false;
    bool stopped = false;
    bool truncated = false;
};

class ProcessRun {
public:
    static constexpr std::size_t outputLimit = 1024 * 1024;

    ProcessRun(std::string script, std::wstring directory, Shell shell = Shell::WindowsPowerShell);
    ~ProcessRun();
    void stop();
    RunUpdate takeUpdate();

private:
    void run(const std::string& script, const std::wstring& directory, Shell shell);
    void appendOutput(const char* bytes, std::size_t count);

    std::atomic<bool> stopRequested_{false};
    std::mutex mutex_;
    RunUpdate update_;
    std::size_t outputSize_ = 0;
    std::thread worker_;
};
