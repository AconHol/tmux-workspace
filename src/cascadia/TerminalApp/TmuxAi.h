// Licensed under the MIT license.
#pragma once

#include <atomic>
#include <filesystem>
#include <memory>
#include <string>

namespace TmuxWorkspace
{
    struct AiSettings
    {
        std::wstring endpoint;
        std::wstring model;
        std::wstring key;
        std::wstring language{ L"英语" };
    };

    // The API secret is DPAPI-encrypted for the current Windows user.
    AiSettings loadAiSettings(const std::filesystem::path& path);
    void saveAiSettings(const std::filesystem::path& path, const AiSettings& settings);
    std::wstring validateAiSettings(const AiSettings& settings);
    std::wstring translate(const AiSettings& settings, std::wstring_view text,
                           const std::shared_ptr<std::atomic_bool>& cancelled);
}
