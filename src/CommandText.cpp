// SPDX-License-Identifier: GPL-3.0-or-later
#include "CommandText.h"

namespace {

bool horizontal(char c) {
    return c == ' ' || c == '\t';
}

std::size_t breakSize(std::string_view text, std::size_t i, bool decodeEscapes) {
    if (text[i] == '\r') {
        return i + 1 < text.size() && text[i + 1] == '\n' ? 2 : 1;
    }
    if (text[i] == '\n') {
        return 1;
    }
    if (!decodeEscapes || (i && !horizontal(text[i - 1]))) {
        return 0;
    }
    const auto tail = text.substr(i);
    const auto size = tail.substr(0, 4) == "\\r\\n" ? 4 :
                      tail.substr(0, 2) == "\\n" || tail.substr(0, 2) == "\\r" ? 2 : 0;
    if (!size || (i + size < text.size() && !horizontal(text[i + size]) &&
                  text[i + size] != '\r' && text[i + size] != '\n')) {
        return 0;
    }
    return size;
}

bool removeContinuation(std::string& result, Shell shell, char quote) {
    if (result.empty()) {
        return false;
    }
    const char marker = result.back();
    if (marker == '\\' && !quote && result.size() > 1 && horizontal(result[result.size() - 2])) {
        result.pop_back();
        return true;
    }
    const bool isCmd = shell == Shell::CommandPrompt;
    if (!(isCmd ? marker == '^' && !quote : marker == '`' && quote != '\'')) {
        return false;
    }
    std::size_t start = result.size();
    while (start && result[start - 1] == marker) {
        --start;
    }
    if ((result.size() - start) % 2 == 0) {
        return false;
    }
    result.pop_back();
    return true;
}

constexpr std::wstring_view jsonWhitespace = L" \t\r\n";

std::wstring_view trimJsonWhitespace(std::wstring_view text) {
    const auto start = text.find_first_not_of(jsonWhitespace);
    if (start == std::wstring_view::npos) {
        return {};
    }
    return text.substr(start, text.find_last_not_of(jsonWhitespace) - start + 1);
}

bool decimal(wchar_t c) {
    return c >= L'0' && c <= L'9';
}

bool hexadecimal(wchar_t c) {
    return decimal(c) || (c >= L'a' && c <= L'f') || (c >= L'A' && c <= L'F');
}

class JsonFormatter {
public:
    explicit JsonFormatter(std::wstring_view input) : input_(input) {}

    std::optional<std::wstring> format() {
        if ((peek() != L'{' && peek() != L'[') || !value(0) || position_ != input_.size()) {
            return std::nullopt;
        }
        return std::move(output_);
    }

private:
    wchar_t peek() const {
        return position_ < input_.size() ? input_[position_] : L'\0';
    }

    bool take(wchar_t c) {
        if (peek() != c) {
            return false;
        }
        ++position_;
        return true;
    }

    void skipWhitespace() {
        while (position_ < input_.size() && jsonWhitespace.find(peek()) != std::wstring_view::npos) {
            ++position_;
        }
    }

    void newline(unsigned depth) {
        output_ += L"\r\n";
        output_.append(depth * 2, L' ');
    }

    bool quotedString() {
        if (!take(L'"')) {
            return false;
        }
        while (position_ < input_.size()) {
            const wchar_t c = input_[position_++];
            if (c == L'"') {
                return true;
            }
            if (c < L' ') {
                return false;
            }
            if (c != L'\\') {
                continue;
            }
            if (take(L'u')) {
                for (unsigned i = 0; i < 4; ++i) {
                    if (!hexadecimal(peek())) {
                        return false;
                    }
                    ++position_;
                }
            } else if (std::wstring_view(L"\"\\/bfnrt").find(peek()) != std::wstring_view::npos) {
                ++position_;
            } else {
                return false;
            }
        }
        return false;
    }

    bool digits() {
        const auto start = position_;
        while (decimal(peek())) {
            ++position_;
        }
        return position_ != start;
    }

    bool number() {
        take(L'-');
        if (!take(L'0') && !digits()) {
            return false;
        }
        if (take(L'.') && !digits()) {
            return false;
        }
        if (take(L'e') || take(L'E')) {
            if (!take(L'+')) {
                take(L'-');
            }
            return digits();
        }
        return true;
    }

    bool token(std::wstring_view text) {
        if (input_.substr(position_, text.size()) != text) {
            return false;
        }
        position_ += text.size();
        return true;
    }

    bool collection(unsigned depth, bool object) {
        const wchar_t close = object ? L'}' : L']';
        output_ += input_[position_++];
        skipWhitespace();
        if (take(close)) {
            output_ += close;
            return true;
        }
        for (;;) {
            newline(depth + 1);
            if (object) {
                const auto start = position_;
                if (!quotedString()) {
                    return false;
                }
                output_.append(input_.substr(start, position_ - start));
                skipWhitespace();
                if (!take(L':')) {
                    return false;
                }
                output_ += L": ";
                skipWhitespace();
            }
            if (!value(depth + 1)) {
                return false;
            }
            skipWhitespace();
            if (!take(L',')) {
                break;
            }
            output_ += L',';
            skipWhitespace();
        }
        if (!take(close)) {
            return false;
        }
        newline(depth);
        output_ += close;
        return true;
    }

    bool value(unsigned depth) {
        // A bounded formatter prevents deeply nested or expanded results from stalling the pane.
        if (depth > 64 || output_.size() >= 4 * 1024 * 1024) {
            return false;
        }
        const auto start = position_;
        switch (peek()) {
        case L'{': return collection(depth, true);
        case L'[': return collection(depth, false);
        case L'"': if (!quotedString()) return false; break;
        case L't': if (!token(L"true")) return false; break;
        case L'f': if (!token(L"false")) return false; break;
        case L'n': if (!token(L"null")) return false; break;
        default: if (!number()) return false; break;
        }
        output_.append(input_.substr(start, position_ - start));
        return true;
    }

    std::wstring_view input_;
    std::size_t position_ = 0;
    std::wstring output_;
};

} // namespace

const wchar_t* shellName(Shell shell) {
    switch (shell) {
    case Shell::PowerShell7: return L"PowerShell 7";
    case Shell::CommandPrompt: return L"Command Prompt (CMD)";
    default: return L"Windows PowerShell";
    }
}

std::string prepareCommand(std::string_view text, Shell shell, InputMode mode) {
    if (mode == InputMode::Script) {
        return std::string(text);
    }
    std::string result;
    result.reserve(text.size());
    char quote = '\0';
    bool escaped = false;
    for (std::size_t i = 0; i < text.size();) {
        const auto skip = breakSize(text, i, mode == InputMode::JoinEscapedLines && !quote && !escaped);
        if (skip) {
            const bool hadSpace = !result.empty() && horizontal(result.back());
            while (!result.empty() && horizontal(result.back())) {
                result.pop_back();
            }
            const bool continuation = removeContinuation(result, shell, quote);
            const bool spaceBeforeMarker = !result.empty() && horizontal(result.back());
            while (!result.empty() && horizontal(result.back())) {
                result.pop_back();
            }
            i += skip;
            const auto afterBreak = i;
            while (i < text.size() && horizontal(text[i])) {
                ++i;
            }
            const bool addSpace = quote ? hadSpace || spaceBeforeMarker :
                                  !continuation || hadSpace || spaceBeforeMarker || i > afterBreak;
            if (addSpace && !result.empty() && i < text.size()) {
                result += ' ';
            }
            escaped = false;
            continue;
        }

        const char c = text[i++];
        result += c;
        if (escaped) {
            escaped = false;
            continue;
        }
        const bool cmd = shell == Shell::CommandPrompt;
        if (cmd ? c == '^' && !quote : c == '`' && quote != '\'') {
            escaped = true;
        } else if (c == quote) {
            quote = '\0';
        } else if (!quote && (c == '"' || (!cmd && c == '\''))) {
            quote = c;
        }
    }
    const auto start = result.find_first_not_of(" \t");
    if (start == std::string::npos) {
        return {};
    }
    return result.substr(start, result.find_last_not_of(" \t") - start + 1);
}

std::optional<std::wstring> prettyJsonBody(std::wstring_view output) {
    if (!output.empty() && output.front() == L'\uFEFF') {
        output.remove_prefix(1);
    }
    output = trimJsonWhitespace(output);
    while (output.substr(0, 5) == L"HTTP/") {
        auto end = output.find(L"\r\n\r\n");
        std::size_t delimiter = 4;
        const auto lf = output.find(L"\n\n");
        if (lf < end) {
            end = lf;
            delimiter = 2;
        }
        if (end == std::wstring_view::npos) {
            return std::nullopt;
        }
        output = trimJsonWhitespace(output.substr(end + delimiter));
    }
    return JsonFormatter(output).format();
}
