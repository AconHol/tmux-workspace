// Licensed under the MIT license.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include "../TerminalApp/TmuxWorkspace.h"
#include "../TerminalApp/SshCommandParser.h"
#include <iostream>
#include <functional>

namespace
{
    int checks = 0;
    void check(bool condition, const char* description)
    {
        ++checks;
        if (!condition)
        {
            throw std::runtime_error(description);
        }
    }
    void rejects(const std::function<void()>& action, const char* description)
    {
        bool threw = false;
        try
        {
            action();
        }
        catch (const std::exception&)
        {
            threw = true;
        }
        check(threw, description);
    }
}

int main()
try
{
    using namespace TmuxWorkspace;
    for (const std::wstring value : { L"", L"server", L"two words", L"a\"b", L"C:\\keys with spaces\\",
                                      L"trailing\\\\", L"$(calc.exe)", L"'; touch /tmp/oops; #", L"中文会话" })
    {
        const auto command = L"ssh.exe " + quoteArgument(value);
        int count{};
        const auto argv = CommandLineToArgvW(command.c_str(), &count);
        check(argv && count == 2, "Windows quoting produced extra arguments");
        check(argv[1] == value, "Windows quoting changed argument contents");
        LocalFree(argv);
    }
    check(quoteShell("a'b") == "'a'\\''b'", "POSIX single quote escaping");
    check(quoteShell("$(touch /tmp/oops);id") == "'$(touch /tmp/oops);id'", "POSIX metacharacters must remain quoted");

    Server server{ L"my-server", L"", L"", L"" };
    check(validate(server).empty(), "SSH config alias");
    check(sshArguments(server, false).find(L" -p ") == std::wstring::npos, "Do not override SSH config port by default");
    check(sshArguments(server, false).find(L"BatchMode=yes") != std::wstring::npos, "Background authentication cannot hang on a prompt");
    check(sshArguments(server, true).find(L"BatchMode") == std::wstring::npos, "Terminal authentication must remain interactive");
    server = { L"::1", L"user", L"65535", L"C:\\my keys\\id_ed25519" };
    check(validate(server).empty(), "IPv6, username, port and key");
    check(sshArguments(server, true).find(quoteArgument(server.identity)) != std::wstring::npos, "Private key path quoting");
    for (const auto target : { L"-oProxyCommand=calc", L"ssh host", L"host\nother", L"" })
    {
        server.target = target;
        check(!validate(server).empty(), "Reject SSH option injection or command lines in target field");
    }
    server.target = L"host";
    for (const auto port : { L"0", L"65536", L"22 -o X", L"9999999999999999999" })
    {
        server.port = port;
        check(!validate(server).empty(), "Reject invalid SSH ports");
    }
    for (const auto name : { L"dev", L"训练任务", L"dev shell", L"$(touch /tmp/oops)", L"a'b" })
    {
        check(validateSessionName(name).empty(), "Valid names must be passed as data");
    }
    for (const auto name : { L"", L"a:b", L"a.b", L"a\nb", L" name", L"name " })
    {
        check(!validateSessionName(name).empty(), "Reject unsupported tmux names");
    }

    const auto sessions = parseSessions("123\t$0\t1800000000\t2\t1\tdev\r\n123\t$8\t1800000002\t1\t0\t训练 'work'\n");
    check(sessions.size() == 2, "Parse multiple sessions");
    check(sessions[0].windows == 2 && sessions[0].clients == 1, "Window and client counts");
    check(sessions[1].name == "训练 'work'", "UTF-8 session names");
    check(sessions[0].key() != sessions[1].key(), "Session identities");
    auto renamed = sessions[0];
    renamed.name = "renamed";
    check(renamed.key() == sessions[0].key(), "A rename must not lose the existing tab");
    auto restarted = sessions[0];
    restarted.serverPid = "999";
    check(restarted.key() != sessions[0].key(), "A restarted tmux server must not focus an old tab");
    restarted = sessions[0];
    restarted.created = "1800000099";
    check(restarted.key() != sessions[0].key(), "Reused session IDs must not focus an old tab");
    check(parseSessions("").empty(), "Empty server");
    check(parseSessions("1\t$2\t3\t1\t0\ta\tb\n")[0].name == "a\tb", "Tabs in names cannot shift metadata fields");
    rejects([] { parseSessions("login banner\n"); }, "Shell banners must not become fake sessions");
    rejects([] { parseSessions("1\tbad-id\t3\t1\t0\tname\n"); }, "Reject invalid IDs");
    rejects([] { parseSessions("1\t$0\t3\tmany\t0\tname\n"); }, "Reject invalid counts");
    rejects([] { parseSessions("1\t$0\t3\t1\t0\tname\n1\t$0\t3\t1\t0\tname\n"); }, "Reject duplicate IDs");
    check(isEmptyServerError("no server running on /tmp/tmux-1000/default"), "Recognize empty tmux server");
    check(isEmptyServerError("error connecting to /tmp/tmux-1000/default (No such file or directory)"), "Recognize absent socket");
    check(!isEmptyServerError("error connecting to /tmp/tmux-1000/default (Permission denied)"), "Permission errors cannot masquerade as an empty server");
    check(!isEmptyServerError("Permission denied (publickey)."), "Authentication errors cannot masquerade as an empty server");
    const auto create = createCommand("a'; touch /tmp/oops; #");
    check(create.find("-s 'a'\\''; touch /tmp/oops; #'") != std::string::npos, "Create-session shell injection protection");
    const auto attach = attachCommand(sessions[0]);
    check(attach.find("'=$0'") != std::string::npos, "Attach by exact session ID rather than prefix");
    check(attach.find("'123/$0/1800000000'") != std::string::npos, "Verify session generation before attaching");
    auto parsed = parseSshCommand(L"ssh user@example.com");
    check(parsed.user == L"user" && parsed.target == L"example.com", "Parse username and host");
    parsed = parseSshCommand(L"ssh -p2222 -l alice -i \"C:\\my keys\\id_ed25519\" server");
    check(parsed.port == L"2222" && parsed.user == L"alice", "Parse joined and separate SSH options");
    check(parsed.identity == L"C:\\my keys\\id_ed25519", "Parse quoted Windows key paths without losing backslashes");
    parsed = parseSshCommand(L"ssh -J jump1,user@jump2:2222 -F 'C:\\my config\\config' target");
    check(parsed.jumpHost == L"jump1,user@jump2:2222", "Parse multiple jump hosts");
    check(parsed.configFile == L"C:\\my config\\config", "Parse a custom SSH config file");
    check(sshArguments(parsed, false).find(L" -J ") != std::wstring::npos, "Retain jump hosts in background SSH queries");
    check(sshArguments(parsed, true).find(L" -F ") != std::wstring::npos, "Retain custom config in terminal connections");
    parsed = parseSshCommand(L"ssh://alice@[2001:db8::1]:2222");
    check(parsed.target == L"2001:db8::1" && parsed.user == L"alice" && parsed.port == L"2222", "Parse IPv6 SSH URLs");
    parsed = parseSshCommand(L"ssh://bob@server:2200/");
    check(parsed.target == L"server" && parsed.user == L"bob" && parsed.port == L"2200", "Parse SSH URLs with a trailing slash");
    check(parseSshCommand(L"my-server").target == L"my-server", "A saved SSH alias can be entered alone");
    check(parseSshCommand(L"ssh -o Port=2222 -o User=bob -o IdentityFile=C:\\keys\\id host").identity == L"C:\\keys\\id", "Parse supported -o settings");
    check(parseSshCommand(L"\"C:\\Windows\\System32\\OpenSSH\\ssh.exe\" host").target == L"host", "Parse a fully qualified SSH executable");
    check(parseSshCommand(L"ssh -tt -vvv user@host").target == L"host", "Accept non-persistent TTY and verbosity flags");
    check(parseSshCommand(L"ssh host -p 2222").port == L"2222", "Accept connection options after the destination");
    parsed = parseSshCommand(L"ssh host");
    auto labelled = parsed;
    labelled.label = L"开发服务器";
    check(sameConnection(parsed, labelled), "Display-name changes cannot duplicate saved servers");
    for (const auto invalid : {
        L"", L"ssh", L"ssh -p", L"ssh -p 0 host", L"ssh -p 65536 host",
        L"ssh -i \"unclosed host", L"ssh host bash", L"ssh host; calc",
        L"ssh host && calc", L"ssh host | other", L"ssh host\nwhoami",
        L"ssh -o ProxyCommand=calc host", L"ssh -o StrictHostKeyChecking=no host",
        L"ssh -L 8080:localhost:80 host", L"sshpass -p password ssh host",
        L"ssh://user@[2001:db8::1", L"ssh://host:99999", L"ssh://host/path",
        L"ssh -- -oProxyCommand=calc"
    })
    {
        rejects([&] { parseSshCommand(invalid); }, "Reject non-connection commands and malformed SSH input");
    }
    check(tokenizeSsh(L"Host alpha beta # comment", true).size() == 3, "SSH config comments");
    check(tokenizeSsh(L"IdentityFile \"C:\\my keys\\id\"", true).at(1) == L"C:\\my keys\\id", "SSH config quoted paths");
    std::cout << "PASS: " << checks << " tmux workspace checks\n";
    return 0;
}
catch (const std::exception& error)
{
    std::cerr << "FAIL: " << error.what() << "\n";
    return 1;
}
