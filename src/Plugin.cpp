// SPDX-License-Identifier: GPL-3.0-or-later
#include <windows.h>
#include <commctrl.h>
#include <richedit.h>

#include "PluginInterface.h"
#include "Docking.h"
#include "ProcessRun.h"

#include <algorithm>
#include <array>
#include <climits>
#include <memory>
#include <string>
#include <string_view>

namespace {

constexpr wchar_t pluginName[] = L"exec++";
constexpr wchar_t moduleName[] = L"exec++.dll";
constexpr wchar_t panelName[] = L"exec++ Output";
constexpr wchar_t panelClass[] = L"ExecPlusPlusOutput";
constexpr int runCommand = 0;
constexpr int showOutputCommand = 1;
constexpr int stopButton = 101;
constexpr UINT_PTR pollTimer = 1;
constexpr UINT_PTR editorMenuHook = 1;

void runSelection();
void showOutput();

NppData npp;
HINSTANCE module = nullptr;
FuncItem commands[] = {{L"exec++", runSelection}, {L"Show Output", showOutput}};
HWND pendingEditorPopup = nullptr;

HWND currentEditor() {
    int view = -1;
    SendMessageW(npp._nppHandle, NPPM_GETCURRENTSCINTILLA, 0, reinterpret_cast<LPARAM>(&view));
    return view == 0 ? npp._scintillaMainHandle : view == 1 ? npp._scintillaSecondHandle : nullptr;
}

bool hasSelection(HWND editor) {
    return editor && !SendMessageW(editor, SCI_GETSELECTIONEMPTY, 0, 0);
}

LRESULT CALLBACK mainSubclass(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam,
                              UINT_PTR id, DWORD_PTR) {
    if (message == WM_CONTEXTMENU) {
        const HWND source = reinterpret_cast<HWND>(wParam);
        if (source == npp._scintillaMainHandle || source == npp._scintillaSecondHandle) {
            // Notepad++ builds and tracks its editor popup during WM_CONTEXTMENU.
            // Only add the item to that popup. Leave the tab and main menus alone.
            const HWND previous = pendingEditorPopup;
            pendingEditorPopup = source;
            const LRESULT result = DefSubclassProc(hwnd, message, wParam, lParam);
            pendingEditorPopup = previous;
            return result;
        }
    } else if (message == WM_INITMENUPOPUP && pendingEditorPopup && !HIWORD(lParam)) {
        const HMENU popup = reinterpret_cast<HMENU>(wParam);
        const UINT commandId = commands[runCommand]._cmdID;
        const UINT state = hasSelection(pendingEditorPopup) ? MF_ENABLED : MF_GRAYED;
        pendingEditorPopup = nullptr;
        if (GetMenuState(popup, commandId, MF_BYCOMMAND) == UINT(-1)) {
            InsertMenuW(popup, 0, MF_BYPOSITION | MF_STRING | state, commandId, pluginName);
        } else {
            EnableMenuItem(popup, commandId, MF_BYCOMMAND | state);
        }
    } else if (message == WM_NCDESTROY) {
        RemoveWindowSubclass(hwnd, mainSubclass, id);
    }
    return DefSubclassProc(hwnd, message, wParam, lParam);
}

std::wstring toWide(std::string_view bytes, UINT codePage) {
    if (bytes.empty()) {
        return {};
    }
    const int length = static_cast<int>(bytes.size());
    const int count = MultiByteToWideChar(codePage, 0, bytes.data(), length, nullptr, 0);
    std::wstring result(count, L'\0');
    if (count) {
        MultiByteToWideChar(codePage, 0, bytes.data(), length, result.data(), count);
    }
    return result;
}

std::string toUtf8(std::wstring_view text) {
    if (text.empty()) {
        return {};
    }
    const int length = static_cast<int>(text.size());
    const int count = WideCharToMultiByte(CP_UTF8, 0, text.data(), length, nullptr, 0, nullptr, nullptr);
    std::string result(count, '\0');
    if (count) {
        WideCharToMultiByte(CP_UTF8, 0, text.data(), length, result.data(), count, nullptr, nullptr);
    }
    return result;
}

// A pipe read can end between UTF-8 bytes. Keep that character for the next update.
std::size_t completeUtf8Prefix(std::string_view bytes) {
    const std::size_t size = bytes.size();
    for (std::size_t offset = 1; offset <= std::min<std::size_t>(size, 3); ++offset) {
        const auto byte = static_cast<unsigned char>(bytes[size - offset]);
        if ((byte & 0xc0) == 0x80) {
            continue;
        }
        const std::size_t expected = byte >= 0xf0 && byte <= 0xf4 ? 4 :
                                     byte >= 0xe0 && byte <= 0xef ? 3 :
                                     byte >= 0xc2 && byte <= 0xdf ? 2 : 1;
        return offset < expected ? size - offset : size;
    }
    return size;
}

std::wstring workingDirectory() {
    std::array<wchar_t, MAX_PATH> path{};
    if (SendMessageW(npp._nppHandle, NPPM_GETCURRENTDIRECTORY, path.size(),
                     reinterpret_cast<LPARAM>(path.data())) && path[0]) {
        const DWORD attributes = GetFileAttributesW(path.data());
        if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY)) {
            return path.data();
        }
    }
    return {};
}

class OutputPanel {
public:
    ~OutputPanel() {
        if (hwnd_ && IsWindow(hwnd_)) {
            KillTimer(hwnd_, pollTimer);
            SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
        }
    }

    bool show() {
        if (!hwnd_ && !create()) {
            return false;
        }
        SendMessageW(npp._nppHandle, NPPM_DMMSHOW, 0, reinterpret_cast<LPARAM>(hwnd_));
        return true;
    }

    void start(std::string script, std::wstring directory) {
        if (run_) {
            MessageBeep(MB_OK);
            return;
        }
        SetWindowTextW(output_, L"");
        pendingUtf8_.clear();
        if (!SetTimer(hwnd_, pollTimer, 50, nullptr)) {
            SetWindowTextW(status_, L"Could not start the output timer.");
            return;
        }
        try {
            run_ = std::make_unique<ProcessRun>(std::move(script), std::move(directory));
        } catch (...) {
            KillTimer(hwnd_, pollTimer);
            SetWindowTextW(status_, L"Could not start the PowerShell worker.");
            return;
        }
        SetWindowTextW(status_, L"Running in Windows PowerShell...");
        EnableWindow(stop_, TRUE);
    }

private:
    static LRESULT CALLBACK windowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
        if (message == WM_NCCREATE) {
            const auto* info = reinterpret_cast<const CREATESTRUCTW*>(lParam);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(info->lpCreateParams));
        }
        auto* panel = reinterpret_cast<OutputPanel*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (panel) {
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
                if (LOWORD(wParam) == stopButton && HIWORD(wParam) == BN_CLICKED && panel->run_) {
                    panel->run_->stop();
                    EnableWindow(panel->stop_, FALSE);
                    SetWindowTextW(panel->status_, L"Stopping...");
                    return 0;
                }
                break;
            case WM_NCDESTROY:
                SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
                panel->hwnd_ = nullptr;
                break;
            }
        }
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }

    bool create() {
        if (!GetModuleHandleW(L"Msftedit.dll") &&
            !LoadLibraryExW(L"Msftedit.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)) {
            return false;
        }
        WNDCLASSW windowClass{};
        windowClass.lpfnWndProc = windowProc;
        windowClass.hInstance = module;
        windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        windowClass.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
        windowClass.lpszClassName = panelClass;
        if (!RegisterClassW(&windowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            return false;
        }
        hwnd_ = CreateWindowExW(0, panelClass, panelName, WS_CHILD | WS_CLIPCHILDREN,
                                0, 0, 0, 0, npp._nppHandle, nullptr, module, this);
        if (!hwnd_) {
            return false;
        }

        status_ = CreateWindowExW(0, L"STATIC", L"Select text in the editor and choose exec++.",
                                  WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP, 0, 0, 0, 0,
                                  hwnd_, nullptr, module, nullptr);
        stop_ = CreateWindowExW(0, L"BUTTON", L"Stop", WS_CHILD | WS_VISIBLE | WS_DISABLED,
                                0, 0, 0, 0, hwnd_,
                                reinterpret_cast<HMENU>(static_cast<INT_PTR>(stopButton)), module, nullptr);
        output_ = CreateWindowExW(WS_EX_CLIENTEDGE, MSFTEDIT_CLASS, L"",
                                  WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE |
                                      ES_AUTOVSCROLL | ES_AUTOHSCROLL | ES_READONLY | ES_NOHIDESEL,
                                  0, 0, 0, 0, hwnd_, nullptr, module, nullptr);
        if (!status_ || !stop_ || !output_) {
            DestroyWindow(hwnd_);
            return false;
        }
        SendMessageW(status_, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), FALSE);
        SendMessageW(stop_, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), FALSE);
        SendMessageW(output_, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(ANSI_FIXED_FONT)), FALSE);
        SendMessageW(output_, EM_SETTEXTMODE, TM_PLAINTEXT, 0);
        SendMessageW(output_, EM_EXLIMITTEXT, 0, ProcessRun::outputLimit + 8192);

        DockedWidgetData data{};
        data.hClient = hwnd_;
        data.pszName = panelName;
        data.dlgID = showOutputCommand;
        data.uMask = DWS_DF_CONT_BOTTOM;
        data.pszModuleName = moduleName;
        SendMessageW(npp._nppHandle, NPPM_DMMREGASDCKDLG, 0, reinterpret_cast<LPARAM>(&data));
        return true;
    }

    void layout(HWND hwnd) const {
        RECT client{};
        GetClientRect(hwnd, &client);
        const auto dpi = GetDpiForWindow(hwnd);
        const int padding = MulDiv(6, dpi, 96);
        const int header = MulDiv(27, dpi, 96);
        const int buttonWidth = MulDiv(65, dpi, 96);
        const int buttonLeft = std::max<int>(padding, client.right - padding - buttonWidth);
        MoveWindow(status_, padding, padding, std::max(0, buttonLeft - 2 * padding), header, TRUE);
        MoveWindow(stop_, buttonLeft, 2, buttonWidth, header - 4, TRUE);
        MoveWindow(output_, 0, header, client.right, std::max<LONG>(0, client.bottom - header), TRUE);
    }

    void append(const std::wstring& text) const {
        if (text.empty()) {
            return;
        }
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

    void poll() {
        if (!run_) {
            return;
        }
        auto update = run_->takeUpdate();
        pendingUtf8_ += update.output;
        const std::size_t count = update.finished ? pendingUtf8_.size() : completeUtf8Prefix(pendingUtf8_);
        auto text = toWide(std::string_view(pendingUtf8_).substr(0, count), CP_UTF8);
        std::replace(text.begin(), text.end(), L'\0', L'\uFFFD');
        append(text);
        pendingUtf8_.erase(0, count);

        if (!update.finished) {
            return;
        }
        std::wstring result;
        if (!update.error.empty()) {
            result = L"Run failed";
            append(L"\r\n" + update.error + L"\r\n");
        } else if (update.stopped) {
            result = L"Stopped";
        } else {
            result = L"Exit code: " + std::to_wstring(update.exitCode);
        }
        if (update.truncated) {
            append(L"\r\n[Output reached the 1 MiB display limit.]\r\n");
        }
        SetWindowTextW(status_, result.c_str());
        EnableWindow(stop_, FALSE);
        KillTimer(hwnd_, pollTimer);
        run_.reset();
    }

    HWND hwnd_ = nullptr;
    HWND status_ = nullptr;
    HWND stop_ = nullptr;
    HWND output_ = nullptr;
    std::string pendingUtf8_;
    std::unique_ptr<ProcessRun> run_;
};

OutputPanel* panel = nullptr;

void showMessage(const wchar_t* text) {
    MessageBoxW(npp._nppHandle, text, pluginName, MB_OK | MB_ICONINFORMATION);
}

void showOutput() {
    try {
        if (!panel) {
            panel = new OutputPanel;
        }
        if (panel->show()) {
            return;
        }
    } catch (...) {
        // Exceptions must not cross the plugin ABI.
    }
    showMessage(L"Could not open the exec++ output pane.");
}

void runSelection() {
    const HWND editor = currentEditor();
    if (!hasSelection(editor)) {
        showMessage(L"Highlight a PowerShell command first.");
        return;
    }
    try {
        const auto length = SendMessageW(editor, SCI_GETSELTEXT, 0, 0);
        if (length <= 0 || length >= INT_MAX) {
            showMessage(L"The selection could not be read.");
            return;
        }
        std::string selected(static_cast<std::size_t>(length) + 1, '\0');
        SendMessageW(editor, SCI_GETSELTEXT, 0, reinterpret_cast<LPARAM>(selected.data()));
        selected.resize(static_cast<std::size_t>(length));
        if (selected.back() == '\0') {
            selected.pop_back();
        }
        if (selected.find('\0') != std::string::npos) {
            showMessage(L"The selection contains a null character.");
            return;
        }
        const auto codePage = static_cast<UINT>(SendMessageW(editor, SCI_GETCODEPAGE, 0, 0));
        if (codePage != CP_UTF8) {
            selected = toUtf8(toWide(selected, codePage));
        }
        if (selected.empty()) {
            showMessage(L"The selection could not be read.");
            return;
        }
        if (!panel) {
            panel = new OutputPanel;
        }
        if (!panel->show()) {
            showMessage(L"Could not open the exec++ output pane.");
            return;
        }
        panel->start(std::move(selected), workingDirectory());
    } catch (...) {
        showMessage(L"Could not run the selected text.");
    }
}

} // namespace

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        module = instance;
    }
    return TRUE;
}

void setInfo(NppData data) { npp = data; }
const wchar_t* getName() { return pluginName; }
FuncItem* getFuncsArray(int* count) {
    *count = static_cast<int>(std::size(commands));
    return commands;
}

void beNotified(SCNotification* notification) {
    if (notification->nmhdr.code == NPPN_READY) {
        SetWindowSubclass(npp._nppHandle, mainSubclass, editorMenuHook, 0);
    } else if (notification->nmhdr.code == NPPN_SHUTDOWN) {
        RemoveWindowSubclass(npp._nppHandle, mainSubclass, editorMenuHook);
        // Notepad++ owns the docking window hierarchy. Only stop our worker here.
        delete panel;
        panel = nullptr;
    }
}

LRESULT messageProc(UINT, WPARAM, LPARAM) { return TRUE; }
BOOL isUnicode() { return TRUE; }
