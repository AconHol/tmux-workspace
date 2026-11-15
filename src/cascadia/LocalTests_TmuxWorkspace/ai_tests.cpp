// Licensed under the MIT license.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "../TerminalApp/TmuxAi.h"
#include <fstream>
#include <iostream>
#include <thread>
#include <functional>

namespace
{
    void require(bool condition, const char* message) { if (!condition) { throw std::runtime_error(message); } }
    void rejects(const std::function<void()>& operation)
    {
        bool rejected{};
        try { operation(); } catch (...) { rejected = true; }
        require(rejected, "Unsafe/invalid API result was accepted");
    }
}

int wmain(int argc, wchar_t** argv)
try
{
    require(argc == 3, "Expected fixture URL and isolated settings path");
    const std::wstring base = argv[1];
    const std::filesystem::path path{ argv[2] };
    TmuxWorkspace::AiSettings settings{ base + L"/v1", L"fixture-model", L"local-test-token", L"英语" };
    require(TmuxWorkspace::validateAiSettings(settings).empty(), "Local API configuration rejected");
    TmuxWorkspace::saveAiSettings(path, settings);
    const auto loaded = TmuxWorkspace::loadAiSettings(path);
    require(loaded.key == settings.key && loaded.endpoint == settings.endpoint && loaded.language == settings.language, "DPAPI credential roundtrip failed");
    std::ifstream file{ path, std::ios::binary };
    const std::string saved{ std::istreambuf_iterator<char>{ file }, {} };
    require(saved.find("local-test-token") == std::string::npos, "Plaintext credential was persisted");
    const auto cancelled = std::make_shared<std::atomic_bool>(false);
    const auto original = L"请帮我检查这个路径 /tmp/example";
    const auto translated = TmuxWorkspace::translate(settings, original, cancelled);
    require(translated == L"Translation CODEX_TRANSLATION_MARKER 中文", "UTF-8 API translation did not roundtrip");
    settings.endpoint = base + L"/v1/chat/completions";
    require(TmuxWorkspace::translate(settings, original, cancelled) == translated, "Full API endpoint was changed incorrectly");
    settings.endpoint = base + L"/lines";
    require(TmuxWorkspace::translate(settings, original, cancelled) == L"first second third", "Translated newlines could submit terminal commands");
    for (const auto scenario : { L"unauthorized", L"rate-limit", L"malformed", L"empty", L"escape", L"redirect" })
    {
        settings.endpoint = base + L"/" + scenario;
        rejects([&]() { TmuxWorkspace::translate(settings, original, cancelled); });
    }
    settings.endpoint = base + L"/slow";
    std::thread cancel{ [cancelled]() { std::this_thread::sleep_for(std::chrono::milliseconds{ 100 }); cancelled->store(true); } };
    rejects([&]() { TmuxWorkspace::translate(settings, original, cancelled); });
    cancel.join();
    settings.endpoint = L"http://example.com/v1";
    require(!TmuxWorkspace::validateAiSettings(settings).empty(), "Non-local plaintext API credential transport accepted");
    std::cout << "PASS: native HTTP translation, request contract, error responses, cancellation, terminal-control filtering and encrypted credential storage" << std::endl;
    return 0;
}
catch (const std::exception& error) { std::cerr << error.what() << std::endl; return 1; }
