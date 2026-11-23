// Licensed under the MIT license.
#pragma once

#include <algorithm>
#include <atomic>
#include <charconv>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// No XAML dependencies: protocol handling and argument quoting are also exercised
// by the standalone tests in src/cascadia/LocalTests_TmuxWorkspace.
namespace TmuxWorkspace
{
    enum class MouseMode { Native, Compatibility, Off };
    struct MouseFocusState;
    struct Server
    {
        std::wstring target;
        std::wstring user;
        std::wstring port;
        std::wstring identity;
        std::wstring jumpHost;
        std::wstring configFile;
        std::wstring label;
        bool operator==(const Server&) const = default;
    };

    struct Session
    {
        std::string serverPid;
        std::string id;
        std::string created;
        unsigned windows{};
        unsigned clients{};
        std::string name;

        std::string key() const { return serverPid + "/" + id + "/" + created; }
        bool operator==(const Session&) const = default;
    };

    struct TabContext
    {
        Server server;
        std::optional<Session> session;
        bool installer{};
    };

    inline bool hasControl(std::wstring_view text)
    {
        return std::any_of(text.begin(), text.end(), [](wchar_t c) { return c < 32 || c == 127; });
    }

    inline std::wstring validate(const Server& server)
    {
        if (server.target.empty() || server.target.front() == L'-' || hasControl(server.target) ||
            server.target.find_first_of(L" \t\"';&|<>$()/\\#") != std::wstring::npos ||
            server.target.find(static_cast<wchar_t>(96)) != std::wstring::npos)
        {
            return L"请填写主机名、IP 地址或 SSH config 别名，不要填写整条 ssh 命令。";
        }
        if (hasControl(server.user) || hasControl(server.identity) || hasControl(server.configFile) ||
            hasControl(server.label) || server.user.starts_with(L"-"))
        {
            return L"用户名和密钥路径不能包含控制字符。";
        }
        if (hasControl(server.jumpHost) || server.jumpHost.starts_with(L"-") ||
            server.jumpHost.find_first_of(L" \t\"';&|<>$()/\\") != std::wstring::npos ||
            server.jumpHost.find(static_cast<wchar_t>(96)) != std::wstring::npos)
        {
            return L"跳板机应使用 SSH 别名或 user@host:port，多台跳板机用逗号分隔。";
        }
        if (!server.port.empty())
        {
            unsigned port = 0;
            for (const auto c : server.port)
            {
                if (c < L'0' || c > L'9' || port > 6553)
                {
                    return L"端口必须是 1 到 65535 之间的数字，或留空以使用 SSH 配置。";
                }
                port = port * 10 + static_cast<unsigned>(c - L'0');
            }
            if (port == 0 || port > 65535)
            {
                return L"端口必须是 1 到 65535 之间的数字。";
            }
        }
        return {};
    }

    inline std::wstring validateSessionName(std::wstring_view name)
    {
        if (name.empty() || name.size() > 80 || hasControl(name) ||
            name.find_first_of(L".:") != std::wstring::npos ||
            name.front() == L' ' || name.back() == L' ')
        {
            return L"请输入 1–80 个字符；名称不能含句点、冒号、换行，首尾不能有空格。";
        }
        return {};
    }

    // Windows CommandLineToArgvW/CRT quoting. Never invoke cmd.exe or PowerShell.
    inline std::wstring quoteArgument(std::wstring_view value)
    {
        std::wstring result{ L"\"" };
        size_t slashes = 0;
        for (const auto c : value)
        {
            if (c == L'\\')
            {
                ++slashes;
                continue;
            }
            if (c == L'"')
            {
                result.append(slashes * 2 + 1, L'\\');
            }
            else
            {
                result.append(slashes, L'\\');
            }
            slashes = 0;
            result += c;
        }
        result.append(slashes * 2, L'\\');
        result += L'"';
        return result;
    }

    // OpenSSH joins the remote command arguments before handing them to the
    // remote shell. Quote remote data separately from Windows process arguments.
    inline std::string quoteShell(std::string_view value)
    {
        std::string result{ "'" };
        for (const auto c : value)
        {
            if (c == '\'')
            {
                result += "'\\''";
            }
            else
            {
                result += c;
            }
        }
        result += '\'';
        return result;
    }

    inline std::wstring sshArguments(const Server& server, bool interactive)
    {
        const auto error = validate(server);
        if (!error.empty())
        {
            throw std::invalid_argument("Invalid SSH connection settings");
        }
        std::wstring result = interactive ? L" -t" : L" -T -o BatchMode=yes -o ConnectionAttempts=1 -o ConnectTimeout=8";
        result += L" -o ServerAliveInterval=15 -o ServerAliveCountMax=2";
        if (!server.port.empty())
        {
            result += L" -p " + quoteArgument(server.port);
        }
        if (!server.user.empty())
        {
            result += L" -l " + quoteArgument(server.user);
        }
        if (!server.identity.empty())
        {
            result += L" -i " + quoteArgument(server.identity);
        }
        if (!server.jumpHost.empty())
        {
            result += L" -J " + quoteArgument(server.jumpHost);
        }
        if (!server.configFile.empty())
        {
            result += L" -F " + quoteArgument(server.configFile);
        }
        result += L" -- " + quoteArgument(server.target);
        return result;
    }

    inline constexpr std::string_view sessionFormat =
        "#{pid}|#{session_id}|#{session_created}|#{session_windows}|#{session_attached}|#{session_name}";

    inline std::string listCommand()
    {
        return "command -v tmux >/dev/null 2>&1 || { echo 'TMUX_NOT_INSTALLED' >&2; exit 127; }; "
               "LC_ALL=C tmux list-sessions -F " + quoteShell(sessionFormat);
    }

    inline std::string createCommand(std::string_view name)
    {
        // The caller validates the user-facing name as well. All bytes are
        // still shell-quoted so existing or non-ASCII names cannot become code.
        return "tmux new-session -d -P -F " + quoteShell(sessionFormat) + " -s " + quoteShell(name);
    }

    inline std::string actionCommand(const Session& session, std::wstring_view action)
    {
        const auto target = quoteShell(session.id);
        if (action == L"split-horizontal") { return "tmux split-window -h -t " + target; }
        if (action == L"split-vertical") { return "tmux split-window -v -t " + target; }
        if (action == L"next-pane") { return "tmux select-pane -t " + quoteShell(session.id + ":.+"); }
        if (action == L"zoom-pane") { return "tmux resize-pane -Z -t " + target; }
        if (action == L"new-window") { return "tmux new-window -t " + quoteShell(session.id + ":"); }
        if (action == L"previous-window") { return "tmux previous-window -t " + target; }
        if (action == L"next-window") { return "tmux next-window -t " + target; }
        if (action == L"next-layout") { return "tmux next-layout -t " + target; }
        if (action == L"copy-mode") { return "tmux copy-mode -t " + target; }
        throw std::invalid_argument("Unknown tmux action");
    }

    inline std::string sessionGuard(const Session& session)
    {
        return "if [ \"$(tmux display-message -p -t " + quoteShell("=" + session.id + ":") +
               " '#{pid}/#{session_id}/#{session_created}')\" != " + quoteShell(session.key()) +
               " ]; then printf '%s\\n' 'Session changed or ended. Refresh the session list.' >&2; exit 1; fi; ";
    }

    inline std::string mouseCommand(const Session& session, MouseMode mode)
    {
        return sessionGuard(session) + "tmux set-option -t " + quoteShell(session.id) +
               " mouse " + (mode == MouseMode::Native ? "on" : "off");
    }

    inline std::string attachCommand(const Session& session, MouseMode mode = MouseMode::Native)
    {
        return mouseCommand(session, mode) + " || exit $?; exec tmux attach-session -t " + quoteShell("=" + session.id);
    }

    inline std::string paneAtCommand(const Session& session, int column, int row, std::string_view onHit)
    {
        if (column < 0 || row < 0 || column > 32766 || row > 32766) { throw std::invalid_argument("Invalid terminal cell"); }
        const auto target = quoteShell(session.id);
        return sessionGuard(session) + "x=" + std::to_string(column) + "; y=" + std::to_string(row) +
            "; status=$(tmux show-options -Aqv -t " + target + " status) || exit 1; "
            "case \"$status\" in off|0) lines=0;; on) lines=1;; [1-5]) lines=$status;; *) lines=1;; esac; "
            "position=$(tmux show-options -Aqv -t " + target + " status-position) || exit 1; "
            "if [ \"$position\" = top ]; then y=$((y-lines)); fi; "
            "[ \"$y\" -ge 0 ] || exit 2; panes=$(tmux list-panes -t " + quoteShell(session.id + ":") +
            " -F '#{pane_id} #{pane_left} #{pane_top} #{pane_width} #{pane_height} #{window_zoomed_flag} #{pane_active} #{window_width} #{window_height}') || exit 1; printf '%s\\n' \"$panes\" | "
            "{ while read -r pane left top width height zoom active ww wh; do "
            "if [ \"$zoom\" = 1 ]; then "
            "[ \"$active\" = 1 ] && [ \"$x\" -lt \"$ww\" ] && [ \"$y\" -lt \"$wh\" ] || continue; "
            "else [ \"$x\" -ge \"$left\" ] && [ \"$x\" -lt $((left+width)) ] && "
            "[ \"$y\" -ge \"$top\" ] && [ \"$y\" -lt $((top+height)) ] || continue; fi; "
            + std::string{ onHit } + "; exit $?; done; exit 2; }";
    }

    inline std::string focusPaneCommand(const Session& session, int column, int row)
    {
        return paneAtCommand(session, column, row, "tmux select-pane -t \"$pane\" && printf '%s\\n' \"$pane\"");
    }

    inline std::string capturePaneAtCommand(const Session& session, int column, int row)
    {
        return paneAtCommand(session, column, row, "tmux capture-pane -p -S -500 -t \"$pane\"");
    }

    inline bool isEmptyServerError(std::string_view error)
    {
        return error.find("no server running on ") != std::string_view::npos ||
               error.find("no sessions") != std::string_view::npos ||
               (error.find("error connecting to ") != std::string_view::npos &&
                error.find("(No such file or directory)") != std::string_view::npos);
    }

    inline unsigned parseNumber(std::string_view value)
    {
        unsigned number{};
        const auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), number);
        if (value.empty() || ec != std::errc{} || end != value.data() + value.size())
        {
            throw std::runtime_error("Invalid numeric field in tmux response");
        }
        return number;
    }

    inline std::vector<Session> parseSessions(std::string_view output)
    {
        std::vector<Session> result;
        while (!output.empty())
        {
            const auto end = output.find('\n');
            auto line = output.substr(0, end);
            output = end == std::string_view::npos ? std::string_view{} : output.substr(end + 1);
            if (!line.empty() && line.back() == '\r')
            {
                line.remove_suffix(1);
            }
            if (line.empty())
            {
                continue;
            }
            std::string_view fields[6];
            // Control characters in format strings can be sanitized by
            // tmux/SSH. Printable separators survive the real transport.
            // Keep reading old tab-delimited snapshots as well. Only split
            // the five metadata fields so names can contain either delimiter.
            const auto firstSeparator = line.find_first_of("|\t");
            const auto delimiter = firstSeparator == std::string_view::npos ? '|' : line[firstSeparator];
            for (size_t i = 0; i < 5; ++i)
            {
                const auto separator = line.find(delimiter);
                if (separator == std::string_view::npos)
                {
                    throw std::runtime_error("Unexpected tmux output (check remote shell startup output)");
                }
                fields[i] = line.substr(0, separator);
                line.remove_prefix(separator + 1);
            }
            fields[5] = line;
            parseNumber(fields[0]);
            parseNumber(fields[2]);
            if (fields[1].size() < 2 || fields[1].front() != '$')
            {
                throw std::runtime_error("Invalid tmux session ID");
            }
            parseNumber(fields[1].substr(1));
            if (fields[5].empty())
            {
                throw std::runtime_error("Empty tmux session name");
            }
            Session session{ std::string(fields[0]), std::string(fields[1]), std::string(fields[2]),
                             parseNumber(fields[3]), parseNumber(fields[4]), std::string(fields[5]) };
            if (std::any_of(result.begin(), result.end(), [&](const auto& existing) { return existing.id == session.id; }))
            {
                throw std::runtime_error("Duplicate tmux session ID");
            }
            result.emplace_back(std::move(session));
        }
        return result;
    }

    struct CommandResult
    {
        unsigned long exitCode{};
        std::string output;
        std::string error;
    };

    std::wstring sshExecutable();
    std::vector<Server> discoverSshServers(const std::wstring& userConfig = {}, const std::wstring& systemConfig = {});
    CommandResult runSsh(const Server& server, const std::string& command,
                         const std::shared_ptr<std::atomic_bool>& cancelled,
                         std::wstring_view configFile = {});
}
