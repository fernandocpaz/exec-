// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <windows.h>

#include <string>
#include <string_view>

inline std::wstring toWide(std::string_view bytes, UINT codePage) {
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

inline std::string toUtf8(std::wstring_view text) {
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
