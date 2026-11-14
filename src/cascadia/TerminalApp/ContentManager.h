/*++
Copyright (c) Microsoft Corporation
Licensed under the MIT license.

Class Name:
- ContentManager.h

Abstract:
- This is a helper class for tracking all of the terminal "content" instances of
  the Terminal. These are all the ControlInteractivity & ControlCore's of each
  of our TermControls. These are each assigned a GUID on creation, and stored in
  a map for later lookup.
- This is used to enable moving panes between windows. TermControl's are not
  thread-agile, so they cannot be reused on other threads. However, the content
  is. This helper, which exists as a singleton across all the threads in the
  Terminal app, allows each thread to create content, assign it to a
  TermControl, detach it from that control, and reattach to new controls on
  other threads.
- When you want to create a new TermControl, call CreateCore to instantiate a
  new content with a GUID for later reparenting.
- Detach can be used to temporarily remove a content from its hosted
  TermControl. After detaching, you can still use LookupCore &
  TermControl::AttachContent to re-attach to the content.
--*/
#pragma once

#include "ContentManager.g.h"
#include "TmuxWorkspace.h"
#include <mutex>
#include <functional>
#include <winrt/Windows.UI.Core.h>

#include <inc/cppwinrt_utils.h>
namespace winrt::TerminalApp::implementation
{
    struct ContentManager : ContentManagerT<ContentManager>
    {
    public:
        ContentManager() = default;
        Microsoft::Terminal::Control::ControlInteractivity CreateCore(const Microsoft::Terminal::Control::IControlSettings& settings,
                                                                      const Microsoft::Terminal::Control::IControlAppearance& unfocusedAppearance,
                                                                      const Microsoft::Terminal::TerminalConnection::ITerminalConnection& connection);
        Microsoft::Terminal::Control::ControlInteractivity TryLookupCore(uint64_t id);

        void Detach(const Microsoft::Terminal::Control::TermControl& control);
        void SetTmuxContext(uint64_t id, ::TmuxWorkspace::TabContext context);
        std::optional<::TmuxWorkspace::TabContext> GetTmuxContext(uint64_t id);
        uint64_t ClaimTmuxSession(const ::TmuxWorkspace::Server& server, const ::TmuxWorkspace::Session& session);
        void ReleaseTmuxClaim(uint64_t claim);
        void BindTmuxSession(uint64_t id, uintptr_t owner, Windows::UI::Core::CoreDispatcher dispatcher, std::function<void()> activate);
        void BeginTmuxMove(uint64_t id);
        void ReleaseTmuxSession(uintptr_t owner, std::string_view sessionKey);
        bool HasTmuxSession(const ::TmuxWorkspace::Server& server, const ::TmuxWorkspace::Session& session);
        void RegisterTmuxWindow(uintptr_t owner, Windows::UI::Core::CoreDispatcher dispatcher, std::function<void()> changed);
        void UnregisterTmuxWindow(uintptr_t owner);

    private:
        std::unordered_map<uint64_t, Microsoft::Terminal::Control::ControlInteractivity> _content;
        std::mutex _tmuxContextMutex;
        std::unordered_map<uint64_t, ::TmuxWorkspace::TabContext> _tmuxContexts;
        struct TmuxOwner
        {
            uint64_t claim{};
            ::TmuxWorkspace::Server server;
            std::string key;
            uint64_t contentId{};
            uintptr_t owner{};
            uint64_t revision{};
            bool moving{};
            bool activateWhenReady{};
            Windows::UI::Core::CoreDispatcher dispatcher{ nullptr };
            std::function<void()> activate;
        };
        struct TmuxObserver
        {
            Windows::UI::Core::CoreDispatcher dispatcher{ nullptr };
            std::function<void()> changed;
        };
        std::vector<TmuxOwner> _tmuxOwners;
        std::unordered_map<uintptr_t, TmuxObserver> _tmuxObservers;
        uint64_t _nextTmuxClaim{ 1 };
        void _notifyTmuxWindows();
        void _queueTmuxActivation(uint64_t claim);
        void _deliverTmuxActivation(uint64_t claim, uint64_t revision);

        void _closedHandler(const winrt::Windows::Foundation::IInspectable& sender,
                            const winrt::Windows::Foundation::IInspectable& e);
    };
}
