// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <optional>
#include <string>
#include <string_view>

enum class Shell { WindowsPowerShell, PowerShell7, CommandPrompt };
enum class InputMode { Script, JoinLines, JoinEscapedLines };

const wchar_t* shellName(Shell shell);
std::string prepareCommand(std::string_view text, Shell shell, InputMode mode);
std::optional<std::wstring> prettyJsonBody(std::wstring_view output);
