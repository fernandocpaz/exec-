// SPDX-License-Identifier: GPL-3.0-or-later
#include <windows.h>
#include <commctrl.h>

#include "PluginInterface.h"
#include "OutputPanel.h"
#include "WindowsText.h"

#include <array>
#include <climits>
#include <optional>
#include <string>

namespace {

constexpr wchar_t pluginName[] = L"exec++";
constexpr int runCommand = 0;
constexpr int showOutputCommand = 1;
constexpr int oneLineCommand = 2;
constexpr int powerShellCommand = 3;
constexpr UINT_PTR editorMenuHook = 1;

void runSelection();
void showOutput();
void runOneLine();
void runPowerShell();

NppData npp;
HINSTANCE module = nullptr;
ShortcutKey runShortcut{true, false, true, VK_RETURN};
FuncItem commands[] = {
    {L"exec++", runSelection, 0, false, &runShortcut},
    {L"Show Output", showOutput},
    {L"Run as One Line", runOneLine},
    {L"Run in Windows PowerShell", runPowerShell}
};
HWND pendingEditorPopup = nullptr;

HWND currentEditor() {
    int view = -1;
    SendMessageW(npp._nppHandle, NPPM_GETCURRENTSCINTILLA, 0, reinterpret_cast<LPARAM>(&view));
    return view == 0 ? npp._scintillaMainHandle : view == 1 ? npp._scintillaSecondHandle : nullptr;
}

bool hasSelection(HWND editor) {
    return editor && !SendMessageW(editor, SCI_GETSELECTIONEMPTY, 0, 0);
}

void addPopupCommand(HMENU popup, int commandIndex, const wchar_t* label, UINT state) {
    const UINT commandId = commands[commandIndex]._cmdID;
    if (GetMenuState(popup, commandId, MF_BYCOMMAND) == UINT(-1)) {
        InsertMenuW(popup, 0, MF_BYPOSITION | MF_STRING | state, commandId, label);
    } else {
        EnableMenuItem(popup, commandId, MF_BYCOMMAND | state);
    }
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
        const UINT state = hasSelection(pendingEditorPopup) ? MF_ENABLED : MF_GRAYED;
        pendingEditorPopup = nullptr;
        addPopupCommand(popup, powerShellCommand, L"exec++ (Windows PowerShell)", state);
        addPopupCommand(popup, oneLineCommand, L"exec++ (Join lines)", state);
        addPopupCommand(popup, runCommand, pluginName, state);
    } else if (message == WM_NCDESTROY) {
        RemoveWindowSubclass(hwnd, mainSubclass, id);
    }
    return DefSubclassProc(hwnd, message, wParam, lParam);
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

OutputPanel* panel = nullptr;

void showMessage(const wchar_t* text) {
    MessageBoxW(npp._nppHandle, text, pluginName, MB_OK | MB_ICONINFORMATION);
}

void showOutput() {
    try {
        if (!panel) {
            panel = new OutputPanel(npp._nppHandle, module, showOutputCommand);
        }
        if (panel->show()) {
            return;
        }
    } catch (...) {
        // Exceptions must not cross the plugin ABI.
    }
    showMessage(L"Could not open the exec++ output pane.");
}

void executeSelection(bool joinLines, std::optional<Shell> shellOverride = std::nullopt) {
    const HWND editor = currentEditor();
    if (!hasSelection(editor)) {
        showMessage(L"Highlight a command first.");
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
            panel = new OutputPanel(npp._nppHandle, module, showOutputCommand);
        }
        if (!panel->show()) {
            showMessage(L"Could not open the exec++ output pane.");
            return;
        }
        panel->start(std::move(selected), workingDirectory(), joinLines, shellOverride);
    } catch (...) {
        showMessage(L"Could not run the selected text.");
    }
}

void runSelection() { executeSelection(false); }
void runOneLine() { executeSelection(true); }
void runPowerShell() { executeSelection(false, Shell::WindowsPowerShell); }

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
    } else if (notification->nmhdr.code == NPPN_DARKMODECHANGED && panel) {
        panel->applyTheme();
    } else if (notification->nmhdr.code == NPPN_SHUTDOWN) {
        RemoveWindowSubclass(npp._nppHandle, mainSubclass, editorMenuHook);
        // Notepad++ owns the docking window hierarchy. Only stop our worker here.
        delete panel;
        panel = nullptr;
    }
}

LRESULT messageProc(UINT, WPARAM, LPARAM) { return TRUE; }
BOOL isUnicode() { return TRUE; }
