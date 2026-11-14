// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "ContentManager.h"
#include "ContentManager.g.cpp"
#include "SshCommandParser.h"

#include <wil/token_helpers.h>

#include "../../types/inc/utils.hpp"

using namespace winrt::Windows::ApplicationModel;
using namespace winrt::Windows::ApplicationModel::DataTransfer;
using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Xaml::Controls;
using namespace winrt::Windows::UI::Core;
using namespace winrt::Windows::System;
using namespace winrt::Microsoft::Terminal;
using namespace winrt::Microsoft::Terminal::Control;
using namespace winrt::Microsoft::Terminal::Settings::Model;

namespace winrt::TerminalApp::implementation
{
    ControlInteractivity ContentManager::CreateCore(const Microsoft::Terminal::Control::IControlSettings& settings,
                                                    const IControlAppearance& unfocusedAppearance,
                                                    const TerminalConnection::ITerminalConnection& connection)
    {
        ControlInteractivity content{ settings, unfocusedAppearance, connection };
        content.Closed({ get_weak(), &ContentManager::_closedHandler });

        _content.emplace(content.Id(), content);

        return content;
    }

    ControlInteractivity ContentManager::TryLookupCore(uint64_t id)
    {
        const auto it = _content.find(id);
        return it != _content.end() ? it->second : ControlInteractivity{ nullptr };
    }

    void ContentManager::Detach(const Microsoft::Terminal::Control::TermControl& control)
    {
        const auto contentId{ control.ContentId() };
        if (const auto& content{ TryLookupCore(contentId) })
        {
            control.Detach();
        }
    }

    void ContentManager::SetTmuxContext(uint64_t id, ::TmuxWorkspace::TabContext context)
    {
        const std::scoped_lock lock{ _tmuxContextMutex };
        _tmuxContexts.insert_or_assign(id, std::move(context));
    }

    std::optional<::TmuxWorkspace::TabContext> ContentManager::GetTmuxContext(uint64_t id)
    {
        const std::scoped_lock lock{ _tmuxContextMutex };
        const auto found = _tmuxContexts.find(id);
        return found == _tmuxContexts.end() ? std::nullopt : std::optional{ found->second };
    }

    uint64_t ContentManager::ClaimTmuxSession(const ::TmuxWorkspace::Server& server, const ::TmuxWorkspace::Session& session)
    {
        uint64_t claimed{}, existing{};
        {
            const std::scoped_lock lock{ _tmuxContextMutex };
            const auto found = std::find_if(_tmuxOwners.begin(), _tmuxOwners.end(), [&](const auto& entry) {
                return entry.key == session.key() && ::TmuxWorkspace::sameConnection(entry.server, server);
            });
            if (found != _tmuxOwners.end())
            {
                found->activateWhenReady = true;
                existing = found->claim;
            }
            else
            {
                TmuxOwner entry;
                entry.claim = claimed = _nextTmuxClaim++;
                entry.server = server;
                entry.key = session.key();
                _tmuxOwners.emplace_back(std::move(entry));
            }
        }
        if (existing) { _queueTmuxActivation(existing); }
        else { _notifyTmuxWindows(); }
        return claimed;
    }

    void ContentManager::ReleaseTmuxClaim(uint64_t claim)
    {
        bool removed{};
        {
            const std::scoped_lock lock{ _tmuxContextMutex };
            removed = std::erase_if(_tmuxOwners, [&](const auto& entry) { return entry.claim == claim && entry.contentId == 0; }) != 0;
        }
        if (removed) { _notifyTmuxWindows(); }
    }

    void ContentManager::BindTmuxSession(uint64_t id, uintptr_t owner, Windows::UI::Core::CoreDispatcher dispatcher, std::function<void()> activate)
    {
        uint64_t claim{};
        {
            const std::scoped_lock lock{ _tmuxContextMutex };
            const auto context = _tmuxContexts.find(id);
            if (context == _tmuxContexts.end() || !context->second.session) { return; }
            auto found = std::find_if(_tmuxOwners.begin(), _tmuxOwners.end(), [&](const auto& entry) {
                return entry.key == context->second.session->key() && ::TmuxWorkspace::sameConnection(entry.server, context->second.server);
            });
            if (found == _tmuxOwners.end())
            {
                TmuxOwner entry;
                entry.claim = _nextTmuxClaim++;
                entry.server = context->second.server;
                entry.key = context->second.session->key();
                _tmuxOwners.emplace_back(std::move(entry));
                found = std::prev(_tmuxOwners.end());
            }
            found->contentId = id;
            found->owner = owner;
            found->dispatcher = std::move(dispatcher);
            found->activate = std::move(activate);
            found->moving = false;
            ++found->revision;
            claim = found->claim;
        }
        _notifyTmuxWindows();
        _queueTmuxActivation(claim);
    }

    void ContentManager::BeginTmuxMove(uint64_t id)
    {
        {
            const std::scoped_lock lock{ _tmuxContextMutex };
            for (auto& entry : _tmuxOwners)
            {
                if (entry.contentId == id) { entry.moving = true; ++entry.revision; }
            }
        }
        _notifyTmuxWindows();
    }

    void ContentManager::ReleaseTmuxSession(uintptr_t owner, std::string_view sessionKey)
    {
        bool removed{};
        {
            const std::scoped_lock lock{ _tmuxContextMutex };
            removed = std::erase_if(_tmuxOwners, [&](const auto& entry) { return entry.owner == owner && entry.key == sessionKey && !entry.moving; }) != 0;
        }
        if (removed) { _notifyTmuxWindows(); }
    }

    bool ContentManager::HasTmuxSession(const ::TmuxWorkspace::Server& server, const ::TmuxWorkspace::Session& session)
    {
        const std::scoped_lock lock{ _tmuxContextMutex };
        return std::any_of(_tmuxOwners.begin(), _tmuxOwners.end(), [&](const auto& entry) {
            return entry.key == session.key() && ::TmuxWorkspace::sameConnection(entry.server, server);
        });
    }

    void ContentManager::RegisterTmuxWindow(uintptr_t owner, Windows::UI::Core::CoreDispatcher dispatcher, std::function<void()> changed)
    {
        const std::scoped_lock lock{ _tmuxContextMutex };
        _tmuxObservers.insert_or_assign(owner, TmuxObserver{ std::move(dispatcher), std::move(changed) });
    }

    void ContentManager::UnregisterTmuxWindow(uintptr_t owner)
    {
        {
            const std::scoped_lock lock{ _tmuxContextMutex };
            _tmuxObservers.erase(owner);
            std::erase_if(_tmuxOwners, [&](const auto& entry) { return entry.owner == owner && !entry.moving; });
        }
        _notifyTmuxWindows();
    }

    void ContentManager::_notifyTmuxWindows()
    {
        std::vector<TmuxObserver> observers;
        {
            const std::scoped_lock lock{ _tmuxContextMutex };
            for (const auto& [_, observer] : _tmuxObservers) { observers.emplace_back(observer); }
        }
        for (auto& observer : observers)
        {
            try
            {
                observer.dispatcher.RunAsync(Windows::UI::Core::CoreDispatcherPriority::Normal, [changed = std::move(observer.changed)]() {
                    try { changed(); }
                    CATCH_LOG();
                });
            }
            CATCH_LOG();
        }
    }

    void ContentManager::_queueTmuxActivation(uint64_t claim)
    {
        Windows::UI::Core::CoreDispatcher dispatcher{ nullptr };
        uint64_t revision{};
        {
            const std::scoped_lock lock{ _tmuxContextMutex };
            const auto found = std::find_if(_tmuxOwners.begin(), _tmuxOwners.end(), [&](const auto& entry) { return entry.claim == claim; });
            if (found == _tmuxOwners.end() || !found->activateWhenReady || found->moving || !found->activate || !found->dispatcher) { return; }
            dispatcher = found->dispatcher;
            revision = found->revision;
        }
        try
        {
            dispatcher.RunAsync(Windows::UI::Core::CoreDispatcherPriority::Normal, [weak = get_weak(), claim, revision]() {
                try { if (const auto manager = weak.get()) { manager->_deliverTmuxActivation(claim, revision); } }
                CATCH_LOG();
            });
        }
        CATCH_LOG();
    }

    void ContentManager::_deliverTmuxActivation(uint64_t claim, uint64_t revision)
    {
        std::function<void()> activate;
        bool reroute{};
        {
            const std::scoped_lock lock{ _tmuxContextMutex };
            const auto found = std::find_if(_tmuxOwners.begin(), _tmuxOwners.end(), [&](const auto& entry) { return entry.claim == claim; });
            if (found == _tmuxOwners.end() || !found->activateWhenReady || found->moving) { return; }
            if (found->revision != revision) { reroute = true; }
            else { found->activateWhenReady = false; activate = found->activate; }
        }
        if (reroute) { _queueTmuxActivation(claim); }
        else if (activate) { activate(); }
    }

    void ContentManager::_closedHandler(const winrt::Windows::Foundation::IInspectable& sender,
                                        const winrt::Windows::Foundation::IInspectable&)
    {
        if (const auto& content{ sender.try_as<winrt::Microsoft::Terminal::Control::ControlInteractivity>() })
        {
            const auto& contentId{ content.Id() };
            _content.erase(contentId);
            {
                const std::scoped_lock lock{ _tmuxContextMutex };
                _tmuxContexts.erase(contentId);
                std::erase_if(_tmuxOwners, [&](const auto& entry) { return entry.contentId == contentId; });
            }
            _notifyTmuxWindows();
        }
    }
}
