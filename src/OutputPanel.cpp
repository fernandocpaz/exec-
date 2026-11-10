// SPDX-License-Identifier: GPL-3.0-or-later
#include "OutputPanel.h"
#include "WindowsText.h"
#include "PluginInterface.h"
#include "Docking.h"

#include <commctrl.h>
#include <commdlg.h>
#include <richedit.h>

#include <algorithm>
#include <array>
#include <climits>
#include <cstring>
#include <utility>

namespace {

constexpr wchar_t panelName[] = L"exec++ Output";
constexpr wchar_t panelClass[] = L"ExecPlusPlusOutput";
constexpr wchar_t settingsSection[] = L"exec++";
constexpr UINT_PTR pollTimer = 1;
constexpr std::size_t historyLimit = 10;

enum ControlId {
    stopButton = 101, shellPicker, inputPicker, historyPicker, searchBox,
    findButton, wrapButton, jsonButton, copyButton, clearButton, outputBox
};

struct ThemeColors {
    COLORREF background, softerBackground, hotBackground, pureBackground;
    COLORREF errorBackground, text, darkerText, disabledText, linkText;
    COLORREF edge, hotEdge, disabledEdge;
};

std::wstring controlText(HWND window) {
    const auto length = GetWindowTextLengthW(window);
    std::wstring text(static_cast<std::size_t>(length) + 1, L'\0');
    text.resize(GetWindowTextW(window, text.data(), length + 1));
    return text;
}

void addChoice(HWND combo, const wchar_t* label) {
    SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label));
}

int selectedChoice(HWND combo) {
    return static_cast<int>(SendMessageW(combo, CB_GETCURSEL, 0, 0));
}

std::wstring elapsedSince(std::chrono::steady_clock::time_point started) {
    using namespace std::chrono;
    const auto tenths = duration_cast<milliseconds>(steady_clock::now() - started).count() / 100;
    return std::to_wstring(tenths / 10) + L"." + std::to_wstring(tenths % 10) + L" s";
}

// Pipe reads can split UTF-8, an ANSI double-byte character, or a CRLF pair.
std::size_t completePrefix(std::string_view bytes, UINT codePage) {
    std::size_t size = bytes.size();
    if (codePage == CP_UTF8) {
        for (std::size_t offset = 1; offset <= std::min<std::size_t>(size, 3); ++offset) {
            const auto byte = static_cast<unsigned char>(bytes[size - offset]);
            if ((byte & 0xc0) == 0x80) {
                continue;
            }
            const std::size_t expected = byte >= 0xf0 && byte <= 0xf4 ? 4 :
                                         byte >= 0xe0 && byte <= 0xef ? 3 :
                                         byte >= 0xc2 && byte <= 0xdf ? 2 : 1;
            if (offset < expected) {
                size -= offset;
            }
            break;
        }
    } else {
        for (std::size_t i = 0; i < size; ++i) {
            if (IsDBCSLeadByteEx(codePage, static_cast<BYTE>(bytes[i])) && ++i == size) {
                --size;
                break;
            }
        }
    }
    return size && bytes[size - 1] == '\r' ? size - 1 : size;
}

LRESULT CALLBACK searchSubclass(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam,
                                UINT_PTR id, DWORD_PTR) {
    if (wParam == VK_RETURN && (message == WM_KEYDOWN || message == WM_CHAR)) {
        if (message == WM_KEYDOWN) {
            SendMessageW(GetParent(hwnd), WM_COMMAND, MAKEWPARAM(findButton, BN_CLICKED), 0);
        }
        return 0;
    }
    if (message == WM_NCDESTROY) {
        RemoveWindowSubclass(hwnd, searchSubclass, id);
    }
    return DefSubclassProc(hwnd, message, wParam, lParam);
}

} // namespace

OutputPanel::OutputPanel(HWND host, HINSTANCE module, int dockCommand)
    : host_(host), module_(module), dockCommand_(dockCommand) {
    std::array<wchar_t, MAX_PATH> folder{};
    if (SendMessageW(host_, NPPM_GETPLUGINSCONFIGDIR, folder.size(),
                     reinterpret_cast<LPARAM>(folder.data())) && folder[0]) {
        preferences_ = std::wstring(folder.data()) + L"\\exec++.ini";
        const auto shell = GetPrivateProfileIntW(settingsSection, L"Shell", 0, preferences_.c_str());
        const auto input = GetPrivateProfileIntW(settingsSection, L"Input", 0, preferences_.c_str());
        if (shell <= static_cast<UINT>(Shell::CommandPrompt)) {
            selectedShell_ = static_cast<Shell>(shell);
        }
        if (input <= static_cast<UINT>(InputMode::JoinEscapedLines)) {
            inputMode_ = static_cast<InputMode>(input);
        }
        wrapEnabled_ = GetPrivateProfileIntW(settingsSection, L"Wrap", 1, preferences_.c_str()) != 0;
    }
}

OutputPanel::~OutputPanel() {
    if (hwnd_ && IsWindow(hwnd_)) {
        KillTimer(hwnd_, pollTimer);
        SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
    }
}

bool OutputPanel::show() {
    if (!hwnd_ && !create()) {
        return false;
    }
    SendMessageW(host_, NPPM_DMMSHOW, 0, reinterpret_cast<LPARAM>(hwnd_));
    return true;
}

void OutputPanel::start(std::string selection, std::wstring directory, bool joinLines) {
    if (run_) {
        MessageBeep(MB_OK);
        return;
    }
    auto script = prepareCommand(selection, selectedShell_, joinLines ? InputMode::JoinLines : inputMode_);
    if (script.find_first_not_of(" \t\r\n") == std::string::npos) {
        SetWindowTextW(status_, L"Select a command with text to run.");
        return;
    }
    if (!SetTimer(hwnd_, pollTimer, 50, nullptr)) {
        SetWindowTextW(status_, L"Could not start the output timer.");
        return;
    }
    try {
        run_ = std::make_unique<ProcessRun>(std::move(script), std::move(directory), selectedShell_);
    } catch (...) {
        KillTimer(hwnd_, pollTimer);
        SetWindowTextW(status_, L"Could not start the shell worker.");
        return;
    }

    current_ = {};
    pendingBytes_.clear();
    jsonView_ = false;
    stopping_ = false;
    selectedResult_ = -1;
    started_ = std::chrono::steady_clock::now();
    SetWindowTextW(output_, L"");
    SetWindowTextW(json_, L"View JSON");
    SetWindowTextW(status_, (std::wstring(L"Running in ") + shellName(selectedShell_) + L" ...").c_str());
    SendMessageW(history_, CB_SETCURSEL, 0, 0);
    for (HWND control : {history_, shell_, input_, json_, clear_}) {
        EnableWindow(control, FALSE);
    }
    EnableWindow(stop_, TRUE);
}

LRESULT CALLBACK OutputPanel::windowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_NCCREATE) {
        const auto* info = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(info->lpCreateParams));
    }
    auto* panel = reinterpret_cast<OutputPanel*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!panel) {
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }
    try {
        switch (message) {
        case WM_SIZE:
            panel->layout(hwnd);
            return 0;
        case WM_TIMER:
            if (wParam == pollTimer) {
                panel->poll();
                return 0;
            }
            break;
        case WM_COMMAND:
            panel->command(LOWORD(wParam), HIWORD(wParam));
            return 0;
        case WM_DPICHANGED_AFTERPARENT:
            panel->layout(hwnd);
            break;
        case WM_NCDESTROY:
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            panel->hwnd_ = nullptr;
            panel->output_ = nullptr;
            break;
        }
    } catch (...) {
        SetWindowTextW(panel->status_, L"Could not update the output pane.");
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

bool OutputPanel::create() {
    if (!GetModuleHandleW(L"Msftedit.dll") &&
        !LoadLibraryExW(L"Msftedit.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)) {
        return false;
    }
    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = windowProc;
    windowClass.hInstance = module_;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
    windowClass.lpszClassName = panelClass;
    if (!RegisterClassW(&windowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return false;
    }
    hwnd_ = CreateWindowExW(0, panelClass, panelName, WS_CHILD | WS_CLIPCHILDREN,
                            0, 0, 0, 0, host_, nullptr, module_, this);
    if (!hwnd_) {
        return false;
    }

    auto make = [this](const wchar_t* type, const wchar_t* text, int id, DWORD style, DWORD exStyle = 0) {
        return CreateWindowExW(exStyle, type, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0,
                               hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), module_, nullptr);
    };
    const DWORD comboStyle = WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST;
    status_ = make(L"STATIC", L"Select text in the editor and choose exec++.", 0, SS_LEFTNOWORDWRAP);
    stop_ = make(L"BUTTON", L"Stop", stopButton, WS_TABSTOP | WS_DISABLED);
    shell_ = make(L"COMBOBOX", L"", shellPicker, comboStyle);
    input_ = make(L"COMBOBOX", L"", inputPicker, comboStyle);
    history_ = make(L"COMBOBOX", L"", historyPicker, comboStyle | WS_DISABLED);
    search_ = make(L"EDIT", L"", searchBox, WS_TABSTOP | ES_AUTOHSCROLL, WS_EX_CLIENTEDGE);
    find_ = make(L"BUTTON", L"Find Next", findButton, WS_TABSTOP);
    wrap_ = make(L"BUTTON", L"Wrap", wrapButton, WS_TABSTOP | BS_AUTOCHECKBOX);
    json_ = make(L"BUTTON", L"View JSON", jsonButton, WS_TABSTOP | WS_DISABLED);
    copy_ = make(L"BUTTON", L"Copy All", copyButton, WS_TABSTOP);
    clear_ = make(L"BUTTON", L"Clear", clearButton, WS_TABSTOP | WS_DISABLED);
    output_ = make(MSFTEDIT_CLASS, L"", outputBox, WS_TABSTOP | WS_VSCROLL | WS_HSCROLL |
                    ES_MULTILINE | ES_AUTOVSCROLL | ES_AUTOHSCROLL | ES_READONLY | ES_NOHIDESEL,
                    WS_EX_CLIENTEDGE);

    for (HWND control : {status_, stop_, shell_, input_, history_, search_, find_, wrap_, json_, copy_, clear_, output_}) {
        if (!control) {
            DestroyWindow(hwnd_);
            return false;
        }
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), FALSE);
    }

    for (const auto shell : {Shell::WindowsPowerShell, Shell::PowerShell7, Shell::CommandPrompt}) {
        addChoice(shell_, shellName(shell));
    }
    for (const auto* mode : {L"Script (keep line breaks)", L"Join lines", L"Join lines + escaped breaks"}) {
        addChoice(input_, mode);
    }
    SendMessageW(shell_, CB_SETCURSEL, static_cast<int>(selectedShell_), 0);
    SendMessageW(input_, CB_SETCURSEL, static_cast<int>(inputMode_), 0);
    SendMessageW(search_, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"Find in output"));
    SetWindowSubclass(search_, searchSubclass, 1, 0);
    SendMessageW(output_, EM_SETTEXTMODE, TM_PLAINTEXT, 0);
    SendMessageW(output_, EM_EXLIMITTEXT, 0, 6 * ProcessRun::outputLimit);
    setWrap();
    refreshHistory();

    DockedWidgetData data{};
    data.hClient = hwnd_;
    data.pszName = panelName;
    data.dlgID = dockCommand_;
    data.uMask = DWS_DF_CONT_BOTTOM;
    data.pszModuleName = L"exec++.dll";
    SendMessageW(host_, NPPM_DMMREGASDCKDLG, 0, reinterpret_cast<LPARAM>(&data));
    applyTheme();
    layout(hwnd_);
    return true;
}

void OutputPanel::applyTheme() const {
    if (!output_) {
        return;
    }
    auto foreground = GetSysColor(COLOR_WINDOWTEXT);
    auto background = GetSysColor(COLOR_WINDOW);
    ThemeColors colors{};
    if (SendMessageW(host_, NPPM_ISDARKMODEENABLED, 0, 0) &&
        SendMessageW(host_, NPPM_GETDARKMODECOLORS, sizeof(colors), reinterpret_cast<LPARAM>(&colors))) {
        foreground = colors.text;
        background = colors.pureBackground;
    }
    SendMessageW(output_, EM_SETBKGNDCOLOR, 0, background);
    CHARFORMAT2W format{};
    format.cbSize = sizeof(format);
    format.dwMask = CFM_FACE | CFM_SIZE | CFM_COLOR;
    format.crTextColor = foreground;
    format.yHeight = 220; // 11 pt in twips, scaled by Rich Edit for the current DPI.
    const wchar_t font[] = L"Consolas";
    std::copy(std::begin(font), std::end(font), format.szFaceName);
    SendMessageW(output_, EM_SETCHARFORMAT, SCF_ALL, reinterpret_cast<LPARAM>(&format));
    SendMessageW(output_, EM_SETCHARFORMAT, SCF_DEFAULT, reinterpret_cast<LPARAM>(&format));
}

void OutputPanel::layout(HWND hwnd) const {
    if (!output_) {
        return;
    }
    RECT client{};
    GetClientRect(hwnd, &client);
    const auto dpi = GetDpiForWindow(hwnd);
    const auto scale = [dpi](int value) { return MulDiv(value, dpi ? dpi : 96, 96); };
    const int gap = scale(6);
    const int row = scale(30);
    const int height = scale(24);
    const int width = std::max<int>(0, client.right - gap * 2);
    const bool narrow = width < scale(590);
    const auto place = [height](HWND control, int x, int y, int w, int h = 0) {
        MoveWindow(control, x, y, std::max(0, w), h ? h : height, TRUE);
    };

    const int stopWidth = scale(64);
    place(status_, gap, gap + scale(3), width - stopWidth - gap);
    place(stop_, gap + std::max(0, width - stopWidth), gap, stopWidth);

    const int pickerSpace = std::max(0, width - 2 * gap);
    const int shellWidth = std::min(scale(185), pickerSpace / 3);
    const int inputWidth = std::min(scale(220), pickerSpace / 3 + gap);
    place(shell_, gap, gap + row, shellWidth, scale(210));
    place(input_, 2 * gap + shellWidth, gap + row, inputWidth, scale(210));
    place(history_, 3 * gap + shellWidth + inputWidth, gap + row,
          pickerSpace - shellWidth - inputWidth, scale(260));
    SendMessageW(shell_, CB_SETDROPPEDWIDTH, scale(185), 0);
    SendMessageW(input_, CB_SETDROPPEDWIDTH, scale(225), 0);
    SendMessageW(history_, CB_SETDROPPEDWIDTH, scale(255), 0);

    int y = gap + 2 * row;
    const int findWidth = scale(78);
    const int wrapWidth = scale(62);
    const int jsonWidth = scale(85);
    const int copyWidth = scale(77);
    const int clearWidth = scale(58);
    const int actionWidth = wrapWidth + jsonWidth + copyWidth + clearWidth + gap * 3;
    const int findSpace = width - (narrow ? 0 : actionWidth + gap);
    place(search_, gap, y, findSpace - findWidth - gap);
    place(find_, gap + std::max(0, findSpace - findWidth), y, findWidth);
    int x = narrow ? gap : gap + findSpace + gap;
    if (narrow) {
        y += row;
    }
    place(wrap_, x, y, wrapWidth);
    x += wrapWidth + gap;
    place(json_, x, y, jsonWidth);
    x += jsonWidth + gap;
    place(copy_, x, y, copyWidth);
    x += copyWidth + gap;
    place(clear_, x, y, clearWidth);
    y += row;
    place(output_, 0, y, client.right, std::max<int>(1, client.bottom - y));
    SendMessageW(output_, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(gap, gap));
}

void OutputPanel::command(int id, int event) {
    if (event == CBN_SELCHANGE) {
        if (id == shellPicker) {
            selectedShell_ = static_cast<Shell>(selectedChoice(shell_));
            savePreferences();
        } else if (id == inputPicker) {
            inputMode_ = static_cast<InputMode>(selectedChoice(input_));
            savePreferences();
        } else if (id == historyPicker && !run_) {
            selectedResult_ = selectedChoice(history_) - 1;
            jsonView_ = false;
            showResult();
        }
        return;
    }
    if (event != BN_CLICKED) {
        return;
    }
    switch (id) {
    case stopButton:
        if (run_) {
            run_->stop();
            stopping_ = true;
            EnableWindow(stop_, FALSE);
        }
        break;
    case findButton:
        findNext();
        break;
    case wrapButton:
        wrapEnabled_ = SendMessageW(wrap_, BM_GETCHECK, 0, 0) == BST_CHECKED;
        setWrap();
        savePreferences();
        break;
    case jsonButton:
        if (!run_ && selectedResult_ >= 0) {
            jsonView_ = !jsonView_;
            showResult();
        }
        break;
    case copyButton:
        copyAll();
        break;
    case clearButton:
        if (!run_) {
            selectedResult_ = -1;
            jsonView_ = false;
            SendMessageW(history_, CB_SETCURSEL, 0, 0);
            showResult();
        }
        break;
    }
}

void OutputPanel::append(const std::wstring& text) {
    if (text.empty()) {
        return;
    }
    current_.output += text;
    CHARRANGE selection{};
    POINT position{};
    SCROLLINFO scroll{};
    scroll.cbSize = sizeof(scroll);
    scroll.fMask = SIF_POS | SIF_RANGE | SIF_PAGE;
    SendMessageW(output_, EM_EXGETSEL, 0, reinterpret_cast<LPARAM>(&selection));
    SendMessageW(output_, EM_GETSCROLLPOS, 0, reinterpret_cast<LPARAM>(&position));
    GetScrollInfo(output_, SB_VERT, &scroll);
    const bool follow = selection.cpMin == selection.cpMax &&
                        scroll.nPos + static_cast<int>(scroll.nPage) >= scroll.nMax;
    CHARRANGE end{LONG_MAX, LONG_MAX};
    SendMessageW(output_, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&end));
    SendMessageW(output_, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(text.c_str()));
    if (follow) {
        SendMessageW(output_, EM_SCROLLCARET, 0, 0);
    } else {
        SendMessageW(output_, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&selection));
        SendMessageW(output_, EM_SETSCROLLPOS, 0, reinterpret_cast<LPARAM>(&position));
    }
}

void OutputPanel::poll() {
    if (!run_) {
        return;
    }
    auto update = run_->takeUpdate();
    pendingBytes_ += update.output;
    const UINT codePage = selectedShell_ == Shell::CommandPrompt ? GetACP() : CP_UTF8;
    const std::size_t count = update.finished ? pendingBytes_.size() : completePrefix(pendingBytes_, codePage);
    auto text = toWide(std::string_view(pendingBytes_).substr(0, count), codePage);
    std::replace(text.begin(), text.end(), L'\0', L'\uFFFD');
    append(text);
    pendingBytes_.erase(0, count);
    const auto elapsed = elapsedSince(started_);

    if (!update.finished) {
        const std::wstring activity = stopping_ ? L"Stopping" :
            std::wstring(L"Running in ") + shellName(selectedShell_);
        SetWindowTextW(status_, (activity + L"  |  " + elapsed).c_str());
        return;
    }

    std::wstring result;
    if (!update.error.empty()) {
        result = L"Run failed";
        append(update.error + L"\r\n");
    } else if (update.stopped) {
        result = L"Stopped";
    } else {
        result = L"Exit code: " + std::to_wstring(update.exitCode);
    }
    if (update.truncated) {
        append(L"\r\n[Output reached the 1 MiB display limit.]\r\n");
    }

    current_.truncated = update.truncated;
    current_.status = result + L"  |  " + elapsed + L"  |  " + shellName(selectedShell_);
    current_.title = L"#" + std::to_wstring(++runNumber_) + L"  " + shellName(selectedShell_) +
                     L"  -  " + result + L" (" + elapsed + L")";
    results_.push_front(std::move(current_));
    if (results_.size() > historyLimit) {
        results_.pop_back();
    }
    selectedResult_ = 0;
    KillTimer(hwnd_, pollTimer);
    run_.reset();
    EnableWindow(stop_, FALSE);
    EnableWindow(shell_, TRUE);
    EnableWindow(input_, TRUE);
    refreshHistory();
    SetWindowTextW(status_, results_.front().status.c_str());
    EnableWindow(clear_, TRUE);
    EnableWindow(json_, !results_.front().truncated && !results_.front().output.empty());
}

void OutputPanel::showResult() {
    if (selectedResult_ < 0 || static_cast<std::size_t>(selectedResult_) >= results_.size()) {
        SetWindowTextW(output_, L"");
        SetWindowTextW(status_, L"Select text in the editor and choose exec++.");
        SetWindowTextW(json_, L"View JSON");
        EnableWindow(clear_, FALSE);
        EnableWindow(json_, FALSE);
        return;
    }
    const auto& result = results_[selectedResult_];
    auto status = result.status;
    if (jsonView_) {
        const auto formatted = result.truncated ? std::nullopt : prettyJsonBody(result.output);
        if (formatted) {
            SetWindowTextW(output_, formatted->c_str());
            status += L"  |  JSON body";
        } else {
            jsonView_ = false;
            status += L"  |  No complete JSON object or array";
        }
    }
    if (!jsonView_) {
        SetWindowTextW(output_, result.output.c_str());
    }
    SetWindowTextW(status_, status.c_str());
    SetWindowTextW(json_, jsonView_ ? L"View Raw" : L"View JSON");
    EnableWindow(clear_, TRUE);
    EnableWindow(json_, !result.truncated && !result.output.empty());
}

void OutputPanel::refreshHistory() {
    SendMessageW(history_, CB_RESETCONTENT, 0, 0);
    addChoice(history_, L"Run history");
    for (const auto& result : results_) {
        addChoice(history_, result.title.c_str());
    }
    SendMessageW(history_, CB_SETCURSEL, selectedResult_ + 1, 0);
    EnableWindow(history_, !results_.empty());
}

void OutputPanel::findNext() const {
    auto needle = controlText(search_);
    if (needle.empty()) {
        SetFocus(search_);
        return;
    }
    CHARRANGE selection{};
    SendMessageW(output_, EM_EXGETSEL, 0, reinterpret_cast<LPARAM>(&selection));
    FINDTEXTEXW match{{selection.cpMax, -1}, needle.data(), {}};
    auto found = SendMessageW(output_, EM_FINDTEXTEXW, FR_DOWN, reinterpret_cast<LPARAM>(&match));
    if (found < 0 && selection.cpMax > 0) {
        match.chrg = {0, selection.cpMax};
        found = SendMessageW(output_, EM_FINDTEXTEXW, FR_DOWN, reinterpret_cast<LPARAM>(&match));
    }
    if (found < 0) {
        MessageBeep(MB_OK);
        return;
    }
    SendMessageW(output_, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&match.chrgText));
    SendMessageW(output_, EM_SCROLLCARET, 0, 0);
}

void OutputPanel::copyAll() const {
    const auto text = controlText(output_);
    if (text.empty() || !OpenClipboard(hwnd_)) {
        return;
    }
    const std::size_t size = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL data = GlobalAlloc(GMEM_MOVEABLE, size);
    if (data) {
        if (void* destination = GlobalLock(data)) {
            std::memcpy(destination, text.c_str(), size);
            GlobalUnlock(data);
            if (EmptyClipboard() && SetClipboardData(CF_UNICODETEXT, data)) {
                data = nullptr; // The clipboard owns it now.
            }
        }
        if (data) {
            GlobalFree(data);
        }
    }
    CloseClipboard();
}

void OutputPanel::setWrap() const {
    SendMessageW(wrap_, BM_SETCHECK, wrapEnabled_ ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(output_, EM_SETTARGETDEVICE, 0, wrapEnabled_ ? 0 : 1);
    ShowScrollBar(output_, SB_HORZ, !wrapEnabled_);
}

void OutputPanel::savePreferences() const {
    if (preferences_.empty()) {
        return;
    }
    const auto shell = std::to_wstring(static_cast<int>(selectedShell_));
    const auto input = std::to_wstring(static_cast<int>(inputMode_));
    WritePrivateProfileStringW(settingsSection, L"Shell", shell.c_str(), preferences_.c_str());
    WritePrivateProfileStringW(settingsSection, L"Input", input.c_str(), preferences_.c_str());
    WritePrivateProfileStringW(settingsSection, L"Wrap", wrapEnabled_ ? L"1" : L"0", preferences_.c_str());
}
