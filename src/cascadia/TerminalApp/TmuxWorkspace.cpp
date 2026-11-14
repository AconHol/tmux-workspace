// Licensed under the MIT license.
#include "pch.h"
#include "TmuxWorkspace.h"
#include "SshCommandParser.h"
#include <til/io.h>
#include <set>
#include <sstream>

namespace TmuxWorkspace
{
    std::wstring sshExecutable()
    {
        wchar_t directory[MAX_PATH]{};
        THROW_LAST_ERROR_IF(GetSystemDirectoryW(directory, ARRAYSIZE(directory)) == 0);
        const auto systemSsh = std::wstring(directory) + L"\\OpenSSH\\ssh.exe";
        if (std::filesystem::is_regular_file(systemSsh)) { return systemSsh; }
        for (const auto path : { L"%ProgramFiles%\\OpenSSH\\ssh.exe", L"%ProgramFiles(x86)%\\OpenSSH\\ssh.exe" })
        {
            const auto expanded = wil::ExpandEnvironmentStringsW<std::wstring>(path);
            if (std::filesystem::is_regular_file(expanded)) { return expanded; }
        }
        throw std::runtime_error("Windows OpenSSH client is not installed");
    }

    std::vector<Server> discoverSshServers(const std::wstring& userConfig, const std::wstring& systemConfig)
    {
        std::vector<Server> servers;
        std::set<std::wstring> visited;
        const auto userFile = userConfig.empty() ? std::filesystem::path(wil::ExpandEnvironmentStringsW<std::wstring>(L"%UserProfile%\\.ssh\\config")) : std::filesystem::path(userConfig);
        const auto systemFile = systemConfig.empty() ? std::filesystem::path(wil::ExpandEnvironmentStringsW<std::wstring>(L"%ProgramData%\\ssh\\ssh_config")) : std::filesystem::path(systemConfig);
        const auto userRoot = userFile.parent_path();
        const auto systemRoot = systemFile.parent_path();
        std::function<void(std::filesystem::path, const std::filesystem::path&, unsigned)> scan;
        scan = [&](std::filesystem::path path, const std::filesystem::path& base, unsigned depth) {
            try
            {
                path = std::filesystem::absolute(path).lexically_normal();
                if (depth > 8 || visited.size() >= 128 || !visited.insert(lowerSsh(path.wstring())).second ||
                    !std::filesystem::is_regular_file(path) || std::filesystem::file_size(path) > 1024 * 1024)
                {
                    return;
                }
                const auto data = til::io::read_file_as_utf8_string_if_exists(path);
                std::wstringstream stream{ std::wstring(winrt::to_hstring(data)) };
                std::wstring line;
                while (std::getline(stream, line))
                {
                    if (!line.empty() && line.back() == L'\r') { line.pop_back(); }
                    line = trimSsh(line);
                    auto firstSeparator = line.find_first_of(L"= \t");
                    if (firstSeparator != std::wstring::npos)
                    {
                        const auto valueStart = line.find_first_not_of(L" \t", firstSeparator);
                        if (valueStart != std::wstring::npos && line[valueStart] == L'=') { line[valueStart] = L' '; }
                    }
                    std::vector<std::wstring> tokens;
                    try { tokens = tokenizeSsh(line, true); }
                    catch (...) { continue; }
                    if (tokens.empty()) { continue; }
                    const auto key = lowerSsh(tokens.front());
                    if (key == L"host")
                    {
                        for (size_t i = 1; i < tokens.size(); ++i)
                        {
                            if (tokens[i].find_first_of(L"*?!") != std::wstring::npos) { continue; }
                            Server server;
                            server.target = tokens[i];
                            if (validate(server).empty() && std::none_of(servers.begin(), servers.end(),
                                [&](const auto& existing) { return sameConnection(existing, server); }))
                            {
                                servers.emplace_back(std::move(server));
                            }
                        }
                    }
                    else if (key == L"include")
                    {
                        for (size_t i = 1; i < tokens.size(); ++i)
                        {
                            auto pattern = tokens[i];
                            if (pattern.starts_with(L"~/") || pattern.starts_with(L"~\\"))
                            {
                                pattern = wil::ExpandEnvironmentStringsW<std::wstring>(L"%UserProfile%") + L"\\" + pattern.substr(2);
                            }
                            auto include = std::filesystem::path(pattern);
                            if (include.is_relative()) { include = base / include; }
                            WIN32_FIND_DATAW entry{};
                            wil::unique_hfind search{ FindFirstFileW(include.c_str(), &entry) };
                            if (search)
                            {
                                do
                                {
                                    if (!(entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                                    {
                                        scan(include.parent_path() / entry.cFileName, base, depth + 1);
                                    }
                                } while (FindNextFileW(search.get(), &entry));
                            }
                        }
                    }
                }
            }
            catch (...) { /* One unreadable include must not hide other saved SSH hosts. */ }
        };
        scan(userFile, userRoot, 0);
        scan(systemFile, systemRoot, 0);
        return servers;
    }

    CommandResult runSsh(const Server& server, const std::string& command,
                         const std::shared_ptr<std::atomic_bool>& cancelled,
                         std::wstring_view configFile)
    {
        if (cancelled->load())
        {
            throw std::runtime_error("SSH request cancelled");
        }
        const auto executable = sshExecutable();
        // Tests supply an isolated SSH configuration and known_hosts file;
        // the application uses the user's existing OpenSSH configuration.
        auto effectiveServer = server;
        if (!configFile.empty()) { effectiveServer.configFile = configFile; }
        const auto remoteCommand = "sh -c " + quoteShell(command);
        auto commandline = quoteArgument(executable) + sshArguments(effectiveServer, false) +
                           L" " + quoteArgument(winrt::to_hstring(remoteCommand));
        SECURITY_ATTRIBUTES security{ sizeof(security), nullptr, TRUE };
        wil::unique_handle stdoutRead, stdoutWrite, stderrRead, stderrWrite;
        THROW_IF_WIN32_BOOL_FALSE(CreatePipe(stdoutRead.put(), stdoutWrite.put(), &security, 0));
        THROW_IF_WIN32_BOOL_FALSE(CreatePipe(stderrRead.put(), stderrWrite.put(), &security, 0));
        THROW_IF_WIN32_BOOL_FALSE(SetHandleInformation(stdoutRead.get(), HANDLE_FLAG_INHERIT, 0));
        THROW_IF_WIN32_BOOL_FALSE(SetHandleInformation(stderrRead.get(), HANDLE_FLAG_INHERIT, 0));
        wil::unique_handle input{ CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                             &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr) };
        THROW_LAST_ERROR_IF(!input);

        // An explicit inheritance allowlist prevents leaking handles from the
        // terminal's other tabs to the background SSH process.
        SIZE_T attributeSize{};
        InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeSize);
        std::vector<std::byte> storage(attributeSize);
        auto attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
        THROW_IF_WIN32_BOOL_FALSE(InitializeProcThreadAttributeList(attributes, 1, 0, &attributeSize));
        auto deleteAttributes = wil::scope_exit([&]() noexcept { DeleteProcThreadAttributeList(attributes); });
        HANDLE handles[]{ input.get(), stdoutWrite.get(), stderrWrite.get() };
        THROW_IF_WIN32_BOOL_FALSE(UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                                           handles, sizeof(handles), nullptr, nullptr));
        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb = sizeof(startup);
        startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        startup.StartupInfo.hStdInput = input.get();
        startup.StartupInfo.hStdOutput = stdoutWrite.get();
        startup.StartupInfo.hStdError = stderrWrite.get();
        startup.lpAttributeList = attributes;
        wil::unique_process_information process;
        THROW_IF_WIN32_BOOL_FALSE(CreateProcessW(executable.c_str(), commandline.data(), nullptr, nullptr, TRUE,
                                                 CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED,
                                                 nullptr, nullptr, &startup.StartupInfo, &process));
        auto terminate = wil::scope_exit([&]() noexcept { TerminateProcess(process.hProcess, ERROR_CANCELLED); });
        wil::unique_handle job{ CreateJobObjectW(nullptr, nullptr) };
        THROW_LAST_ERROR_IF(!job);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        THROW_IF_WIN32_BOOL_FALSE(SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits)));
        THROW_IF_WIN32_BOOL_FALSE(AssignProcessToJobObject(job.get(), process.hProcess));
        THROW_LAST_ERROR_IF(ResumeThread(process.hThread) == static_cast<DWORD>(-1));
        stdoutWrite.reset();
        stderrWrite.reset();

        CommandResult result;
        const auto start = GetTickCount64();
        const auto drain = [](HANDLE pipe, std::string& text) {
            DWORD available{};
            while (PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr) && available)
            {
                char buffer[4096];
                DWORD read{};
                if (!ReadFile(pipe, buffer, std::min<DWORD>(available, sizeof(buffer)), &read, nullptr) || !read)
                {
                    break;
                }
                if (text.size() + read > 1024 * 1024)
                {
                    throw std::runtime_error("SSH response exceeded 1 MiB");
                }
                text.append(buffer, read);
            }
        };
        for (;;)
        {
            drain(stdoutRead.get(), result.output);
            drain(stderrRead.get(), result.error);
            if (WaitForSingleObject(process.hProcess, 30) == WAIT_OBJECT_0)
            {
                drain(stdoutRead.get(), result.output);
                drain(stderrRead.get(), result.error);
                break;
            }
            if (cancelled->load())
            {
                throw std::runtime_error("SSH request cancelled");
            }
            if (GetTickCount64() - start > 20000)
            {
                throw std::runtime_error("SSH request timed out after 20 seconds");
            }
        }
        THROW_IF_WIN32_BOOL_FALSE(GetExitCodeProcess(process.hProcess, &result.exitCode));
        terminate.release();
        return result;
    }
}
