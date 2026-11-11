// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ProcessRun.h"

#include <chrono>
#include <deque>
#include <memory>
#include <optional>

class OutputPanel {
public:
    OutputPanel(HWND host, HINSTANCE module, int dockCommand);
    ~OutputPanel();

    bool show();
    void start(std::string selection, std::wstring directory, bool joinLines);
    void applyTheme() const;

private:
    struct Result {
        std::wstring output;
        std::wstring status;
        std::wstring title;
        bool truncated = false;
        bool preferJson = true;
    };

    static LRESULT CALLBACK windowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    bool create();
    void layout(HWND hwnd) const;
    void command(int id, int event);
    void append(const std::wstring& text);
    void poll();
    void selectResult(int index, bool keepRawOutput = false);
    void showResult(bool keepRawOutput = false);
    Result* displayedResult();
    void refreshHistory();
    void findNext() const;
    void copyAll() const;
    void setWrap() const;
    void savePreferences() const;

    HWND host_;
    HINSTANCE module_;
    int dockCommand_;
    HWND hwnd_ = nullptr;
    HWND status_ = nullptr;
    HWND stop_ = nullptr;
    HWND shell_ = nullptr;
    HWND input_ = nullptr;
    HWND history_ = nullptr;
    HWND clearHistory_ = nullptr;
    HWND search_ = nullptr;
    HWND find_ = nullptr;
    HWND wrap_ = nullptr;
    HWND json_ = nullptr;
    HWND copy_ = nullptr;
    HWND clear_ = nullptr;
    HWND output_ = nullptr;

    std::wstring preferences_;
    bool wrapEnabled_ = true;
    bool stopping_ = false;
    Shell selectedShell_ = Shell::WindowsPowerShell;
    InputMode inputMode_ = InputMode::Script;
    std::unique_ptr<ProcessRun> run_;
    std::chrono::steady_clock::time_point started_;
    std::string pendingBytes_;
    Result current_;
    std::deque<Result> results_;
    std::optional<Result> displayedAfterClear_;
    std::size_t runNumber_ = 0;
    int selectedResult_ = -1;
    std::optional<std::wstring> selectedJson_;
};
