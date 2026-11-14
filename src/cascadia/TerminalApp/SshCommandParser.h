// Licensed under the MIT license.
#pragma once
#include "TmuxWorkspace.h"
#include <cwctype>

namespace TmuxWorkspace
{
    inline std::wstring trimSsh(std::wstring_view value)
    {
        const auto first = value.find_first_not_of(L" \t\r\n");
        return first == std::wstring_view::npos ? std::wstring{} :
               std::wstring(value.substr(first, value.find_last_not_of(L" \t\r\n") - first + 1));
    }

    inline std::wstring lowerSsh(std::wstring value)
    {
        std::transform(value.begin(), value.end(), value.begin(),
                       [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
        return value;
    }

    // Parse text only. The pasted command is never executed by a shell.
    inline std::vector<std::wstring> tokenizeSsh(std::wstring_view command, bool configLine = false)
    {
        std::vector<std::wstring> tokens;
        std::wstring token;
        wchar_t quote{};
        bool started{};
        for (size_t i = 0; i < command.size(); ++i)
        {
            const auto c = command[i];
            if (c < 32 && c != L'\t')
            {
                throw std::invalid_argument("Paste a single SSH command");
            }
            if (quote == L'\'')
            {
                if (c == quote) { quote = 0; }
                else { token += c; }
                continue;
            }
            if (c == L'\\')
            {
                size_t count = 1;
                while (i + count < command.size() && command[i + count] == L'\\') { ++count; }
                if (i + count < command.size() && command[i + count] == L'"')
                {
                    token.append(count / 2, L'\\');
                    i += count;
                    if (count % 2) { token += L'"'; }
                    else { quote = quote == L'"' ? 0 : L'"'; }
                }
                else
                {
                    token.append(count, L'\\');
                    i += count - 1;
                }
                started = true;
                continue;
            }
            if (quote == L'"')
            {
                if (c == quote) { quote = 0; }
                else { token += c; }
                continue;
            }
            if (configLine && c == L'#') { break; }
            if (c == L'"' || c == L'\'')
            {
                quote = c;
                started = true;
            }
            else if (c == L' ' || c == L'\t')
            {
                if (started) { tokens.emplace_back(std::move(token)); token.clear(); started = false; }
            }
            else
            {
                if (!configLine && (std::wstring_view(L";&|<>").find(c) != std::wstring_view::npos ||
                                    c == static_cast<wchar_t>(96)))
                {
                    throw std::invalid_argument("Shell operators are not SSH connection settings");
                }
                token += c;
                started = true;
            }
        }
        if (quote) { throw std::invalid_argument("Unclosed quote in SSH command"); }
        if (started) { tokens.emplace_back(std::move(token)); }
        return tokens;
    }

    inline void parseSshDestination(std::wstring value, Server& server)
    {
        bool uri = lowerSsh(value).starts_with(L"ssh://");
        if (uri)
        {
            value.erase(0, 6);
            if (value.ends_with(L"/")) { value.pop_back(); }
            if (value.find_first_of(L"/?#") != std::wstring::npos)
            {
                throw std::invalid_argument("SSH URLs may contain only a user, host and port");
            }
        }
        const auto at = value.rfind(L'@');
        if (at != std::wstring::npos)
        {
            if (server.user.empty()) { server.user = value.substr(0, at); }
            value.erase(0, at + 1);
        }
        if (uri)
        {
            if (value.starts_with(L"["))
            {
                const auto bracket = value.find(L']');
                if (bracket == std::wstring::npos) { throw std::invalid_argument("Invalid IPv6 SSH URL"); }
                const auto suffix = value.substr(bracket + 1);
                if (!suffix.empty())
                {
                    if (!suffix.starts_with(L":")) { throw std::invalid_argument("Invalid SSH URL port"); }
                    if (server.port.empty()) { server.port = suffix.substr(1); }
                }
                value = value.substr(1, bracket - 1);
            }
            else
            {
                const auto colon = value.rfind(L':');
                if (colon != std::wstring::npos)
                {
                    if (value.find(L':') != colon) { throw std::invalid_argument("Put IPv6 addresses in square brackets in SSH URLs"); }
                    if (server.port.empty()) { server.port = value.substr(colon + 1); }
                    value.resize(colon);
                }
            }
        }
        server.target = std::move(value);
    }

    inline Server parseSshCommand(std::wstring_view input)
    {
        auto tokens = tokenizeSsh(trimSsh(input));
        if (tokens.empty()) { throw std::invalid_argument("Enter an SSH command or host"); }
        auto executable = lowerSsh(tokens.front());
        const auto slash = executable.find_last_of(L"/\\");
        if (slash != std::wstring::npos) { executable.erase(0, slash + 1); }
        size_t index = executable == L"ssh" || executable == L"ssh.exe" ? 1 : 0;
        if (index == 0 && tokens.size() != 1)
        {
            throw std::invalid_argument("Start with ssh, or enter only a host or SSH config alias");
        }
        Server server;
        std::wstring destination;
        bool endOptions{};
        for (; index < tokens.size(); ++index)
        {
            const auto& token = tokens[index];
            if (!endOptions && token == L"--") { endOptions = true; continue; }
            if (!endOptions && token.size() > 1 && token.front() == L'-')
            {
                if (token == L"-t" || token == L"-tt" || token == L"-T" ||
                    token == L"-v" || token == L"-vv" || token == L"-vvv")
                {
                    continue;
                }
                const auto option = token[1];
                std::wstring value = token.substr(2);
                if (std::wstring_view(L"pilJFo").find(option) == std::wstring_view::npos)
                {
                    throw std::invalid_argument("Unsupported SSH option. Use -p, -l, -i, -J, -F or an SSH config alias");
                }
                if (value.empty())
                {
                    if (++index == tokens.size()) { throw std::invalid_argument("An SSH option is missing its value"); }
                    value = tokens[index];
                }
                if (value.empty()) { throw std::invalid_argument("An SSH option has an empty value"); }
                switch (option)
                {
                case L'p': server.port = value; break;
                case L'l': server.user = value; break;
                case L'i': server.identity = value; break;
                case L'J': server.jumpHost = value; break;
                case L'F': server.configFile = value; break;
                case L'o':
                {
                    const auto split = value.find_first_of(L"= \t");
                    if (split == std::wstring::npos) { throw std::invalid_argument("Use -o Name=value"); }
                    const auto key = lowerSsh(value.substr(0, split));
                    const auto setting = trimSsh(value.substr(split + 1));
                    if (setting.empty()) { throw std::invalid_argument("An SSH setting is missing its value"); }
                    if (key == L"port") { server.port = setting; }
                    else if (key == L"user") { server.user = setting; }
                    else if (key == L"identityfile") { server.identity = setting; }
                    else if (key == L"proxyjump") { server.jumpHost = setting; }
                    else { throw std::invalid_argument("Put other SSH options in your SSH config and use its Host alias"); }
                    break;
                }
                }
            }
            else if (destination.empty())
            {
                destination = token;
            }
            else
            {
                throw std::invalid_argument("Remote commands are not supported when adding a server");
            }
        }
        if (destination.empty()) { throw std::invalid_argument("The SSH host is missing"); }
        parseSshDestination(std::move(destination), server);
        if (!validate(server).empty()) { throw std::invalid_argument("Invalid host, port or connection settings"); }
        return server;
    }

    inline bool sameConnection(const Server& a, const Server& b)
    {
        return a.target == b.target && a.user == b.user && a.port == b.port &&
               a.identity == b.identity && a.jumpHost == b.jumpHost && a.configFile == b.configFile;
    }

    inline std::string installTmuxCommand()
    {
        return
            "set -e; if command -v tmux >/dev/null 2>&1; then tmux -V; exit 0; fi; "
            "if [ \"$(id -u)\" = 0 ]; then SUDO=; "
            "elif command -v sudo >/dev/null 2>&1; then SUDO=sudo; "
            "else printf '%s\\n' 'Administrator privileges are required to install tmux.' >&2; exit 1; fi; "
            "if command -v apt-get >/dev/null 2>&1; then $SUDO apt-get update && $SUDO apt-get install -y tmux; "
            "elif command -v dnf >/dev/null 2>&1; then $SUDO dnf install -y tmux; "
            "elif command -v yum >/dev/null 2>&1; then $SUDO yum install -y tmux; "
            "elif command -v zypper >/dev/null 2>&1; then $SUDO zypper --non-interactive install tmux; "
            "elif command -v pacman >/dev/null 2>&1; then $SUDO pacman -S --needed tmux; "
            "elif command -v apk >/dev/null 2>&1; then $SUDO apk add tmux; "
            "else printf '%s\\n' 'No supported package manager found. Install tmux manually.' >&2; exit 1; fi; "
            "tmux -V; printf '%s\\n' 'tmux is ready. Return to the session list.'";
    }
}
