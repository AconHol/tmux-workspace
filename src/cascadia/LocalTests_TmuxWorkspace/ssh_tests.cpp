// Licensed under the MIT license.
#include "../TerminalApp/TmuxWorkspace.h"
#include <chrono>
#include <iostream>
#include <thread>
#include <filesystem>

int wmain(int argc, wchar_t** argv)
try
{
    using namespace TmuxWorkspace;
    if (argc != 3)
    {
        throw std::runtime_error("Pass the isolated fixture SSH config path");
    }
    Server server{ L"fixture", L"", L"", L"" };
    auto token = std::make_shared<std::atomic_bool>(false);
    int checks{};
    const auto check = [&](bool value, const char* description) {
        ++checks;
        if (!value)
        {
            throw std::runtime_error(description);
        }
    };
    const auto run = [&](const std::string& command) { return runSsh(server, command, token, argv[1]); };
    const auto discovery = std::filesystem::path(argv[2]);
    const auto hosts = discoverSshServers((discovery / L"config").wstring(), (discovery / L"ssh_config").wstring());
    check(hosts.size() == 8, "Discover hosts in user, system and Include files, while excluding wildcard patterns");
    for (const auto alias : { L"alpha", L"beta", L"gamma", L"delta", L"epsilon", L"zeta", L"eta", L"theta" })
    {
        check(std::any_of(hosts.begin(), hosts.end(), [&](const auto& host) { return host.target == alias; }),
              "Discover each concrete SSH config alias, including ones without HostName");
    }
    auto result = run(listCommand());
    if (result.exitCode != 0)
    {
        std::cerr << "Initial SSH exit " << result.exitCode << ": " << result.error << "\n";
    }
    check(result.exitCode == 0, "SSH public-key authentication and remote command execution");
    check(result.error.find("fixture banner") != std::string::npos, "SSH stderr must be captured separately");
    auto sessions = parseSessions(result.output);
    check(sessions.size() == 2, "A stderr banner must not corrupt the session list");
    const std::string name = "native '测试' $(ignored)";
    result = run(createCommand(name));
    if (result.exitCode != 0)
    {
        std::cerr << "Create SSH exit " << result.exitCode << ": " << result.error << "\n";
    }
    check(result.exitCode == 0, "Create a remote session");
    check(parseSessions(result.output).at(0).name == name, "Unicode and shell metacharacters are data");
    result = run(createCommand(name));
    check(result.exitCode != 0 && result.error.find("duplicate session") != std::string::npos, "Duplicate names must not create or attach a different session");
    check(parseSessions(run(listCommand()).output).size() == 3, "Creation must be visible in subsequent queries");
    result = run("fixture:exit-seven");
    check(result.exitCode == 7 && result.error.find("expected error") != std::string::npos && result.output.empty(), "Remote exit codes and error output");
    result = run("fixture:no-server");
    check(result.exitCode == 1 && isEmptyServerError(result.error), "Empty tmux server");
    result = run("fixture:permission-denied");
    check(result.exitCode == 1 && !isEmptyServerError(result.error), "Socket permissions are errors, not empty lists");
    server.user = L"wrong-user";
    result = run("fixture:exit-seven");
    check(result.exitCode == 255 && result.error.find("Permission denied") != std::string::npos, "Authentication failures are reported");
    server.user.clear();
    bool bounded = false;
    try
    {
        run("fixture:flood");
    }
    catch (const std::exception&)
    {
        bounded = true;
    }
    check(bounded, "Output is bounded to avoid unbounded memory consumption");
    bool cancelled = false;
    const auto start = std::chrono::steady_clock::now();
    std::jthread cancel([token]() {
        std::this_thread::sleep_for(std::chrono::milliseconds{ 300 });
        token->store(true);
    });
    try
    {
        run("fixture:hang");
    }
    catch (const std::exception&)
    {
        cancelled = true;
    }
    check(cancelled && std::chrono::steady_clock::now() - start < std::chrono::seconds{ 4 },
          "Switching server or closing the app can cancel an in-flight SSH request");
    std::cout << "PASS: " << checks << " real OpenSSH transport checks (local simulated tmux server)\n";
    return 0;
}
catch (const std::exception& error)
{
    std::cerr << "FAIL: " << error.what() << "\n";
    return 1;
}
