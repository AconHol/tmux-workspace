// Licensed under the MIT license.
// Included only by a dedicated TmuxWorkflowTests=true build. Not present in
// the installed release. Runs the real page, SSH transport and TermControl.
#include <fstream>
#include <functional>

// The production PCH uses WINRT_LEAN_AND_MEAN, which omits producer
// implementations for platform event-argument interfaces.
namespace winrt::impl
{
    template<typename D>
    struct produce<D, winrt::Windows::UI::Xaml::Controls::IItemClickEventArgs> :
        produce_base<D, winrt::Windows::UI::Xaml::Controls::IItemClickEventArgs>
    {
        int32_t __stdcall get_ClickedItem(void** value) noexcept final
        {
            *value = nullptr;
            try { *value = winrt::detach_abi(this->shim().ClickedItem()); return 0; }
            catch (...) { return winrt::to_hresult(); }
        }
    };
}

namespace TerminalAppLocalTests
{
    struct WorkflowClickArgs : winrt::implements<WorkflowClickArgs, winrt::Windows::UI::Xaml::Controls::IItemClickEventArgs>
    {
        winrt::Windows::Foundation::IInspectable item;
        explicit WorkflowClickArgs(winrt::Windows::Foundation::IInspectable value) : item(std::move(value)) {}
        winrt::Windows::Foundation::IInspectable ClickedItem() { return item; }
    };

    struct WorkflowDialogs : winrt::implements<WorkflowDialogs, winrt::TerminalApp::IDialogPresenter>
    {
        std::function<void(winrt::Windows::UI::Xaml::Controls::ContentDialog)> prepare;
        winrt::Windows::Foundation::IAsyncOperation<winrt::Windows::UI::Xaml::Controls::ContentDialogResult> ShowDialog(
            winrt::Windows::UI::Xaml::Controls::ContentDialog dialog)
        {
            if (prepare) { prepare(dialog); }
            co_return winrt::Windows::UI::Xaml::Controls::ContentDialogResult::Primary;
        }
    };

    struct WorkflowLiveDialog : winrt::implements<WorkflowLiveDialog, winrt::TerminalApp::IDialogPresenter>
    {
        winrt::com_ptr<winrt::TerminalApp::implementation::TerminalPage> page;
        std::function<void(winrt::Windows::UI::Xaml::Controls::ContentDialog)> opened;
        winrt::Windows::Foundation::IAsyncOperation<winrt::Windows::UI::Xaml::Controls::ContentDialogResult> ShowDialog(
            winrt::Windows::UI::Xaml::Controls::ContentDialog dialog)
        {
            const auto dispatcher = page->Dispatcher();
            const auto showing = page->ShowTmuxInputDialog(dialog);
            co_await winrt::resume_after(std::chrono::milliseconds{ 250 });
            co_await winrt::resume_foreground(dispatcher);
            opened(dialog);
            co_return co_await showing;
        }
    };

    class TmuxWorkflowTests
    {
        using Page = winrt::TerminalApp::implementation::TerminalPage;
        struct PageSlot
        {
            winrt::weak_ref<Page> page;
            winrt::Windows::UI::Core::CoreDispatcher dispatcher{ nullptr };
        };
        inline static std::mutex _slotMutex;
        inline static std::vector<PageSlot> _slots;
        inline static std::atomic_bool _started{};
        static winrt::Windows::Foundation::IAsyncAction pause(winrt::Windows::UI::Core::CoreDispatcher dispatcher)
        {
            co_await winrt::resume_after(std::chrono::milliseconds{ 200 });
            co_await winrt::resume_foreground(dispatcher);
        }
        static void require(bool condition, const char* message)
        {
            if (!condition) { throw std::runtime_error(message); }
        }
        static winrt::Windows::UI::Xaml::Controls::ItemClickEventArgs clickArgs(winrt::Windows::Foundation::IInspectable item)
        {
            return winrt::make<WorkflowClickArgs>(std::move(item)).as<winrt::Windows::UI::Xaml::Controls::ItemClickEventArgs>();
        }
        static winrt::Windows::Foundation::IAsyncAction ProbeNameInput(winrt::com_ptr<Page> page,
            winrt::Windows::UI::Xaml::Controls::ContentDialog dialog, winrt::hstring existing, std::shared_ptr<std::string> failure)
        {
            try
            {
                page->TmuxNewSessionName().Focus(winrt::Windows::UI::Xaml::FocusState::Programmatic);
                co_await pause(page->Dispatcher());
                require(page->TmuxInputLayer().Visibility() == winrt::Windows::UI::Xaml::Visibility::Visible &&
                        page->TmuxInputLayer().ActualWidth() == page->Root().ActualWidth() &&
                        page->TmuxInputLayer().ActualHeight() == page->Root().ActualHeight(), "Inline modal layer does not cover the actual app bounds");
                const auto nativeInput = GetFocus();
                require(nativeInput != nullptr, "Native keyboard focus is missing");
                for (const auto ch : std::wstring_view{ L"key测试" })
                {
                    require(PostMessageW(nativeInput, WM_CHAR, ch, 1) != FALSE, "Cannot queue a character to the XAML Island input window");
                    co_await pause(page->Dispatcher());
                }
                require(page->TmuxNewSessionName().Text() == L"key测试", "Queued Latin and Unicode characters did not reach the editable form");
                page->TmuxNewSessionName().Text(L"");
                std::wstring typed;
                for (const auto ch : std::wstring_view(existing))
                {
                    typed += ch;
                    page->TmuxNewSessionName().Text(typed);
                    page->_FocusCurrentTab(false);
                    co_await pause(page->Dispatcher());
                    require(page->TmuxNewSessionName().FocusState() != winrt::Windows::UI::Xaml::FocusState::Unfocused, "Typing validation stole input focus");
                }
                require(dialog.IsPrimaryButtonEnabled() && dialog.PrimaryButtonText() == L"打开已有会话", "Duplicate name does not offer the existing session");
                typed += L"-new";
                page->TmuxNewSessionName().Text(typed);
                page->_FocusCurrentTab(false);
                co_await pause(page->Dispatcher());
                require(page->TmuxNewSessionName().Text() == typed && page->TmuxNewSessionName().FocusState() != winrt::Windows::UI::Xaml::FocusState::Unfocused,
                        "Cannot continue editing after a duplicate name");
                require(dialog.PrimaryButtonText() == L"新建并打开", "Edited name did not restore the create action");
                const auto shade = page->TmuxInputLayer().Background().as<winrt::Windows::UI::Xaml::Media::SolidColorBrush>().Color();
                require(shade.R == 0 && shade.G == 0 && shade.B == 0 && shade.A == 102, "The overlay must use the bounded dark shade rather than popup smoke");
            }
            catch (const std::exception& error) { *failure = error.what(); }
            catch (...) { *failure = "Native dialog input probe failed"; }
            if (page->_tmuxDismissInput) { page->_tmuxDismissInput(); }
        }
        static winrt::Windows::Foundation::IAsyncAction ProbeOtherForm(winrt::com_ptr<Page> page, winrt::Windows::UI::Xaml::Controls::TextBox field, std::shared_ptr<std::string> failure)
        {
            try
            {
                field.Text(L"");
                field.Focus(winrt::Windows::UI::Xaml::FocusState::Programmatic);
                co_await pause(page->Dispatcher());
                const auto input = GetFocus();
                require(input != nullptr, "Native form input has no focused host window");
                for (const auto ch : std::wstring_view{ L"key测试" })
                {
                    require(PostMessageW(input, WM_CHAR, ch, 1) != FALSE, "Cannot queue native form input");
                    co_await pause(page->Dispatcher());
                }
                require(field.Text() == L"key测试", "Native input did not reach the server or appearance form");
                PostMessageW(input, WM_KEYDOWN, VK_ESCAPE, 1);
                PostMessageW(input, WM_KEYUP, VK_ESCAPE, 0xc0000001);
                co_await pause(page->Dispatcher());
                require(page->TmuxInputLayer().Visibility() == winrt::Windows::UI::Xaml::Visibility::Collapsed, "Escape did not dismiss the inline form");
            }
            catch (const std::exception& error) { *failure = error.what(); }
            catch (...) { *failure = "Server or appearance native-input probe failed"; }
            if (page->_tmuxDismissInput) { page->_tmuxDismissInput(); }
        }
        static winrt::Windows::Foundation::IAsyncAction TypeAndCreate(winrt::com_ptr<Page> page, winrt::hstring name, std::shared_ptr<std::string> failure)
        {
            try
            {
                page->TmuxNewSessionName().Focus(winrt::Windows::UI::Xaml::FocusState::Programmatic);
                co_await pause(page->Dispatcher());
                const auto input = GetFocus();
                require(input != nullptr, "Session creation has no native input focus");
                for (const auto ch : std::wstring_view(name)) { PostMessageW(input, WM_CHAR, ch, 1); }
                co_await pause(page->Dispatcher());
                require(page->TmuxNewSessionName().Text() == name && page->TmuxInputPrimary().IsEnabled(), "Typing did not enable the visible create button");
                PostMessageW(input, WM_KEYDOWN, VK_RETURN, 1);
                PostMessageW(input, WM_KEYUP, VK_RETURN, 0xc0000001);
            }
            catch (const std::exception& error) { *failure = error.what(); }
            catch (...) { *failure = "Native Enter submission failed"; }
            if (!failure->empty() && page->_tmuxDismissInput) { page->_tmuxDismissInput(); }
        }
    public:
        static void Start(winrt::com_ptr<Page> page)
        {
            {
                const std::scoped_lock lock{ _slotMutex };
                _slots.push_back({ page->get_weak(), page->Dispatcher() });
            }
            if (!_started.exchange(true)) { Run(std::move(page)); }
        }
        static safe_void_coroutine Run(winrt::com_ptr<Page> page)
        {
            using namespace winrt;
            using namespace winrt::Windows::UI::Xaml;
            using namespace winrt::Windows::UI::Xaml::Controls;
            using namespace winrt::Microsoft::Terminal::Settings::Model;
            const auto dispatcher = page->Dispatcher();
            wchar_t command[4096]{};
            wchar_t report[32768]{};
            if (!GetEnvironmentVariableW(L"TMUX_WORKFLOW_SERVER", command, ARRAYSIZE(command)) ||
                !GetEnvironmentVariableW(L"TMUX_WORKFLOW_REPORT", report, ARRAYSIZE(report))) { co_return; }
            std::ofstream log{ std::filesystem::path{ report }, std::ios::trunc };
            const auto note = [&](const char* message) { log << message << std::endl; };
            std::optional<::TmuxWorkspace::Server> server;
            std::optional<::TmuxWorkspace::Session> ownedSession;
            std::optional<::TmuxWorkspace::Session> baselineSession;
            bool passed{};
            try
            {
                for (unsigned i = 0; i < 5; ++i) { co_await pause(dispatcher); }
                require(page->TmuxServerPicker().Visibility() == Visibility::Visible && page->_tabs.Size() == 0, "Initial server picker is unavailable");
                note("OPEN: actual workspace initialized without local shell");
                server = ::TmuxWorkspace::parseSshCommand(command);
                // This portable test instance has its own settings directory.
                page->_tmuxSavedServers = { *server };
                page->_tmuxImportedServers.clear();
                page->_TmuxRenderServers();
                const auto row = page->TmuxServerList().Items().GetAt(0).as<ListViewItem>();
                page->_TmuxServerClicked(nullptr, clickArgs(row.Content()));
                for (unsigned i = 0; page->_tmuxBusy && i < 100; ++i) { co_await pause(dispatcher); }
                if (!page->_tmuxBusy && !page->TmuxProblem().IsOpen() && page->_tmuxSessions.empty())
                {
                    const auto baselineName = "codex-check-base-" + std::to_string(GetTickCount64());
                    co_await resume_background();
                    const auto created = ::TmuxWorkspace::runSsh(*server, ::TmuxWorkspace::createCommand(baselineName), std::make_shared<std::atomic_bool>(false));
                    const auto sessions = created.exitCode == 0 ? ::TmuxWorkspace::parseSessions(created.output) : std::vector<::TmuxWorkspace::Session>{};
                    co_await resume_foreground(dispatcher);
                    require(sessions.size() == 1, "Cannot create an isolated baseline on the empty tmux server");
                    baselineSession = sessions.front();
                    page->_TmuxRefresh(true);
                    for (unsigned i = 0; page->_tmuxBusy && i < 100; ++i) { co_await pause(dispatcher); }
                    note("BASELINE: created a temporary session because the server initially had no sessions");
                }
                if (page->_tmuxBusy || page->TmuxProblem().IsOpen() || page->_tmuxSessions.empty())
                {
                    log << "CONNECT STATE: selected=" << page->_tmuxServer.has_value() << " busy=" << page->_tmuxBusy
                        << " alive=" << page->_tmuxAlive << " sessions=" << page->_tmuxSessions.size()
                        << " home=" << to_string(page->TmuxHomeStatus().Text())
                        << " status=" << to_string(page->TmuxStatus().Text())
                        << " error=" << to_string(page->TmuxProblem().Message()) << std::endl;
                }
                require(!page->_tmuxBusy && !page->TmuxProblem().IsOpen() && !page->_tmuxSessions.empty(), "Real SSH server did not return sessions");
                note("CONNECT: real Linux tmux sessions rendered in the sidebar");
                const auto first = page->_tmuxSessions.front();
                page->_TmuxSessionClicked(nullptr, clickArgs(page->TmuxSessionList().Items().GetAt(0).as<ListViewItem>().Content()));
                for (unsigned i = 0; i < 15; ++i) { co_await pause(dispatcher); }
                require(page->_tabs.Size() == 1, "First session did not create a terminal tab");
                page->_FocusCurrentTab(false);
                require(page->_GetActiveControl().ConnectionState() == winrt::Microsoft::Terminal::TerminalConnection::ConnectionState::Connected, "SSH terminal disconnected");
                note("FOCUS: original crashing focus path completed in the real terminal tab");
                page->_TmuxOpenSession(first);
                require(page->_tabs.Size() == 1, "Repeated session selection duplicated the tab");
                note("REUSE: selecting the same session returned to its existing tab");

                const auto expandedWidth = page->_tabContent.ActualWidth();
                page->_TmuxToggleSidebarClicked(nullptr, nullptr);
                for (unsigned i = 0; i < 3; ++i) { co_await pause(dispatcher); }
                require(!page->TmuxWorkspaceView().IsPaneOpen() && page->TmuxSidebarRail().Visibility() == Visibility::Visible && page->_tabContent.ActualWidth() >= expandedWidth + 220,
                        "Compact sidebar did not collapse to the native rail");
                page->_TmuxToggleSidebarClicked(nullptr, nullptr);
                for (unsigned i = 0; i < 3; ++i) { co_await pause(dispatcher); }
                require(page->TmuxWorkspaceView().IsPaneOpen() && page->TmuxSidebarBody().Visibility() == Visibility::Visible && page->_tabContent.ActualWidth() <= expandedWidth + 1,
                        "Expanded sidebar did not restore its content");
                require(std::wstring_view(page->TmuxStatus().Text()).find(L"5 秒") == std::wstring_view::npos, "Polling text is still visible");
                note("SIDEBAR: native compact rail collapsed and expanded while the terminal remained connected");

                const auto inputFailure = std::make_shared<std::string>();
                auto liveDialog = make_self<WorkflowLiveDialog>();
                liveDialog->page = page;
                liveDialog->opened = [page, first, inputFailure](ContentDialog dialog) { ProbeNameInput(page, dialog, to_hstring(first.name), inputFailure); };
                page->DialogPresenter(liveDialog.as<winrt::TerminalApp::IDialogPresenter>());
                page->_TmuxNewSessionClicked(nullptr, nullptr);
                for (unsigned i = 0; page->_tmuxDialogOpen && i < 100; ++i) { co_await pause(dispatcher); }
                require(!page->_tmuxDialogOpen && inputFailure->empty(), inputFailure->empty() ? "Live name dialog did not close" : inputFailure->c_str());
                for (unsigned i = 0; page->_tmuxBusy && i < 100; ++i) { co_await pause(dispatcher); }
                note("INPUT: native WM_CHAR messages reached the in-island textbox; editing a duplicate retained input focus");

                ShowWindow(*page->_hostingHwnd, SW_MAXIMIZE);
                const auto translucent = page->_settings.Copy();
                const auto translucentAppearance = translucent.GetProfileForArgs(NewTerminalArgs{}).DefaultAppearance();
                translucentAppearance.Opacity(0.2f);
                translucentAppearance.UseAcrylic(true);
                page->SetSettings(translucent, true);
                co_await pause(dispatcher);
                for (const auto serverForm : { true, false })
                {
                    inputFailure->clear();
                    liveDialog->opened = [page, serverForm, inputFailure](ContentDialog) {
                        ProbeOtherForm(page, serverForm ? page->TmuxSshCommand() : page->TmuxFontFace(), inputFailure);
                    };
                    if (serverForm) { page->_TmuxAddServerClicked(nullptr, nullptr); }
                    else { page->_TmuxPersonalizationClicked(nullptr, nullptr); }
                    for (unsigned i = 0; page->_tmuxDialogOpen && i < 60; ++i) { co_await pause(dispatcher); }
                    co_await pause(dispatcher);
                    require(!page->_tmuxDialogOpen && inputFailure->empty(), inputFailure->empty() ? "Native input form stayed open" : inputFailure->c_str());
                    require(!page->_tmuxDismissInput, "Input form left a pending modal operation");
                }
                note("INPUT: server and appearance forms received native Latin/Unicode characters and dismissed with Escape at 20-percent acrylic opacity in a maximized window");

                const auto name = L"codex-check-" + std::to_wstring(GetTickCount64());
                auto dialogs = make_self<WorkflowDialogs>();
                dialogs->prepare = [&](ContentDialog) { page->TmuxNewSessionName().Text(to_hstring(first.name)); };
                page->DialogPresenter(dialogs.as<winrt::TerminalApp::IDialogPresenter>());
                page->_TmuxNewSessionClicked(nullptr, nullptr);
                for (unsigned i = 0; (page->_tmuxDialogOpen || page->_tmuxBusy) && i < 100; ++i) { co_await pause(dispatcher); }
                require(page->_tabs.Size() == 1, "Accepting a duplicate name created another tab");
                note("DUPLICATE: accepting an existing name selected its original tab");
                inputFailure->clear();
                liveDialog->opened = [page, name, inputFailure](ContentDialog) { TypeAndCreate(page, hstring{ name }, inputFailure); };
                page->DialogPresenter(liveDialog.as<winrt::TerminalApp::IDialogPresenter>());
                page->_TmuxNewSessionClicked(nullptr, nullptr);
                for (unsigned i = 0; (page->_tmuxDialogOpen || page->_tmuxBusy) && i < 100; ++i) { co_await pause(dispatcher); }
                require(inputFailure->empty(), inputFailure->empty() ? "Native submit failed" : inputFailure->c_str());
                const auto found = std::find_if(page->_tmuxSessions.begin(), page->_tmuxSessions.end(), [&](const auto& session) { return session.name == to_string(name); });
                require(found != page->_tmuxSessions.end(), "New-session button did not create a remote session");
                ownedSession = *found;
                require(page->_tabs.Size() == 2, "New session did not open its own tab");
                for (unsigned i = 0; i < 10; ++i) { co_await pause(dispatcher); }
                page->_FocusCurrentTab(false);
                // Send a marker only to the shell in the test-owned session,
                // through SSH/tmux. Never send input to an existing user pane.
                const auto markerCommand = "tmux send-keys -t " + ::TmuxWorkspace::quoteShell(ownedSession->id) +
                    " -l " + ::TmuxWorkspace::quoteShell("printf '\\103\\117\\104\\105\\130_WORKFLOW_OK\\n'") +
                    "; tmux send-keys -t " + ::TmuxWorkspace::quoteShell(ownedSession->id) + " Enter";
                co_await resume_background();
                const auto sent = ::TmuxWorkspace::runSsh(*server, markerCommand, std::make_shared<std::atomic_bool>(false));
                co_await resume_foreground(dispatcher);
                require(sent.exitCode == 0, "Cannot send marker to test-owned remote pane");
                bool rendered{};
                for (unsigned i = 0; i < 40; ++i)
                {
                    if (std::wstring_view(page->_GetActiveControl().ReadEntireBuffer()).find(L"CODEX_WORKFLOW_OK") != std::wstring_view::npos) { rendered = true; break; }
                    co_await pause(dispatcher);
                }
                require(rendered, "Actual terminal did not display the output of the remote tmux shell");
                note("CREATE: queued native text and Enter created a real tmux shell and its output reached TermControl");

                dialogs->prepare = [&](ContentDialog dialog) {
                    require(dialog == page->TmuxPersonalizationDialog(), "Unexpected personalization dialog");
                    page->TmuxAcrylic().IsOn(false);
                    page->TmuxOpacity().Value(60);
                };
                page->DialogPresenter(dialogs.as<winrt::TerminalApp::IDialogPresenter>());
                page->_TmuxPersonalizationClicked(nullptr, nullptr);
                for (unsigned i = 0; page->_tmuxDialogOpen && i < 30; ++i) { co_await pause(dispatcher); }
                for (unsigned i = 0; i < 5; ++i) { co_await pause(dispatcher); }
                const auto opacity = page->_GetActiveControl().BackgroundOpacity();
                require(opacity > 0.59f && opacity < 0.61f, "Personalization did not reach the running SSH terminal");
                const auto sideOpacity = page->TmuxSidebar().Background().as<Media::SolidColorBrush>().Opacity();
                require(sideOpacity > 0.59 && sideOpacity < 0.61, "Sidebar opacity did not update");
                note("APPEARANCE: saved opacity reached the running terminal and sidebar");

                page->_TmuxOpenSession(first);
                page->_FocusCurrentTab(false);
                require(page->_tabs.Size() == 2 && page->_GetFocusedTab() == page->_tmuxTabs.at(first.key()).get(), "Existing-session tab switch failed");
                note("SWITCH: returned to the original session tab without duplication");
                page->_TmuxOpenSession(*ownedSession);
                page->_RemoveTab(page->_GetFocusedTab());
                require(page->_tabs.Size() == 1, "Closing a session tab failed");
                require(page->_GetFocusedTab() == page->_tmuxTabs.at(first.key()).get(), "Closing the active tab did not return to the remaining session");
                page->_TmuxOpenSession(*ownedSession);
                for (unsigned i = 0; i < 5; ++i) { co_await pause(dispatcher); }
                page->_FocusCurrentTab(false);
                require(page->_tabs.Size() == 2, "Reopening the detached session failed");

                const auto movedId = page->_GetActiveControl().ContentId();
                page->_stashed.draggedTab = page->_GetFocusedTabImpl();
                page->_onTabDroppedOutside(nullptr, nullptr);
                require(page->_tabs.Size() == 1, "Moving a tab did not remove its source tab");
                PageSlot destination;
                bool adopted{};
                for (unsigned i = 0; i < 100 && !adopted; ++i)
                {
                    {
                        const std::scoped_lock lock{ _slotMutex };
                        if (_slots.size() > 1) { destination = _slots.back(); }
                    }
                    if (destination.dispatcher)
                    {
                        co_await destination.dispatcher.RunAsync(winrt::Windows::UI::Core::CoreDispatcherPriority::Normal, [&]() {
                            if (const auto target = destination.page.get())
                            {
                                const auto control = target->_GetActiveControl();
                                adopted = target->_tmuxServer && ::TmuxWorkspace::sameConnection(*target->_tmuxServer, *server) &&
                                    target->_tabs.Size() == 1 && control && control.ContentId() == movedId &&
                                    target->_tmuxTabs.contains(ownedSession->key()) && target->TmuxWorkspaceView().Visibility() == Visibility::Visible &&
                                    std::wstring_view(control.ReadEntireBuffer()).find(L"CODEX_WORKFLOW_OK") != std::wstring_view::npos;
                                if (adopted) { target->_FocusCurrentTab(false); }
                            }
                        });
                    }
                    if (!adopted) { co_await pause(dispatcher); }
                }
                require(adopted, "New window did not preserve the SSH content, server, session and terminal buffer");
                note("WINDOW: tab tear-out created a real new window retaining the same SSH connection, buffer, tmux session and sidebar");
                uint64_t destinationSummons{};
                uint64_t sourceSummons{};
                page->SummonWindowRequested([&](auto&&, auto&&) { ++sourceSummons; });
                co_await destination.dispatcher.RunAsync(winrt::Windows::UI::Core::CoreDispatcherPriority::Normal, [&]() {
                    if (const auto target = destination.page.get())
                    {
                        target->SummonWindowRequested([&](auto&&, auto&&) { ++destinationSummons; });
                        target->_TmuxVerifySshClicked(nullptr, nullptr);
                        ShowWindow(*target->_hostingHwnd, SW_MINIMIZE);
                    }
                });
                page->SummonWindowRequested.raise(*page, nullptr);
                co_await pause(dispatcher);
                page->_TmuxOpenSession(*ownedSession);
                require(page->_tabs.Size() == 1 && !page->_tmuxTabs.contains(ownedSession->key()), "A cross-window click created another source tab");
                bool jumped{};
                for (unsigned i = 0; i < 30 && !jumped; ++i)
                {
                    co_await destination.dispatcher.RunAsync(winrt::Windows::UI::Core::CoreDispatcherPriority::Normal, [&]() {
                        if (const auto target = destination.page.get())
                        {
                            const auto control = target->_GetActiveControl();
                            jumped = destinationSummons > 0 && target->_tabs.Size() == 2 && control && control.ContentId() == movedId && !IsIconic(*target->_hostingHwnd);
                        }
                    });
                    if (!jumped) { co_await pause(dispatcher); }
                }
                require(jumped, "Cross-window selection did not restore the owner window and select its existing session tab");
                note("GLOBAL: another window selected the existing session tab, restored its minimized window and created no duplicate");
                const auto summonsBefore = sourceSummons;
                bool destinationDuplicated{};
                co_await destination.dispatcher.RunAsync(winrt::Windows::UI::Core::CoreDispatcherPriority::Normal, [&]() {
                    if (const auto target = destination.page.get())
                    {
                        target->_TmuxOpenSession(first);
                        destinationDuplicated = target->_tabs.Size() != 2 || target->_tmuxTabs.contains(first.key());
                    }
                });
                for (unsigned i = 0; sourceSummons == summonsBefore && i < 30; ++i) { co_await pause(dispatcher); }
                require(!destinationDuplicated && sourceSummons > summonsBefore && page->_GetFocusedTab() == page->_tmuxTabs.at(first.key()).get(),
                        "Reverse cross-window session selection failed");
                note("GLOBAL: selecting a session in the second window activated its original tab in the first window");
                bool destinationReturned{};
                co_await destination.dispatcher.RunAsync(winrt::Windows::UI::Core::CoreDispatcherPriority::Normal, [&]() {
                    if (const auto target = destination.page.get())
                    {
                        while (target->_tabs.Size()) { target->_RemoveTab(target->_tabs.GetAt(0)); }
                        destinationReturned = target->_tabs.Size() == 0 && target->TmuxEmptyWorkspace().Visibility() == Visibility::Visible && target->TmuxWorkspaceView().IsPaneOpen();
                    }
                });
                require(destinationReturned, "Closing the last tab did not return to the expanded session picker");
                // Issue opening requests on both UI threads without waiting
                // for either one to finish constructing its terminal control.
                const auto competing = destination.dispatcher.RunAsync(winrt::Windows::UI::Core::CoreDispatcherPriority::Normal, [&]() {
                    if (const auto target = destination.page.get()) { target->_TmuxOpenSession(*ownedSession); }
                });
                page->_TmuxOpenSession(*ownedSession);
                co_await competing;
                for (unsigned i = 0; i < 5; ++i) { co_await pause(dispatcher); }
                bool destinationOwns{};
                uint32_t destinationTabCount{};
                co_await destination.dispatcher.RunAsync(winrt::Windows::UI::Core::CoreDispatcherPriority::Normal, [&]() {
                    if (const auto target = destination.page.get())
                    {
                        destinationOwns = target->_tmuxTabs.contains(ownedSession->key());
                        destinationTabCount = target->_tabs.Size();
                    }
                });
                require(static_cast<unsigned>(page->_tmuxTabs.contains(ownedSession->key())) + static_cast<unsigned>(destinationOwns) == 1 &&
                        page->_tabs.Size() + destinationTabCount == 2, "Simultaneous clicks opened the session more than once");
                note("RACE: simultaneous requests from two UI threads produced exactly one session tab");
                if (const auto owned = page->_tmuxTabs.find(ownedSession->key()); owned != page->_tmuxTabs.end())
                {
                    if (const auto tab = owned->second.get()) { page->_RemoveTab(tab); }
                }
                co_await destination.dispatcher.RunAsync(winrt::Windows::UI::Core::CoreDispatcherPriority::Normal, [&]() {
                    if (const auto target = destination.page.get())
                    {
                        while (target->_tabs.Size()) { target->_RemoveTab(target->_tabs.GetAt(0)); }
                        target->CloseWindowRequested.raise(*target, nullptr);
                    }
                });
                page->_TmuxOpenSession(*ownedSession);
                for (unsigned i = 0; i < 5; ++i) { co_await pause(dispatcher); }
                require(page->_tabs.Size() == 2 && page->_tmuxTabs.contains(ownedSession->key()), "Closing an owner window left a stale global session reservation");
                note("REOPEN: closing the owner released the global reservation and allowed the session to open again");
                const auto endedKey = ownedSession->key();
                const auto endCommand = "tmux kill-session -t " + ::TmuxWorkspace::quoteShell("=" + ownedSession->id);
                co_await resume_background();
                const auto ended = ::TmuxWorkspace::runSsh(*server, endCommand, std::make_shared<std::atomic_bool>(false));
                co_await resume_foreground(dispatcher);
                require(ended.exitCode == 0, "Cannot end the test-owned remote session");
                ownedSession.reset();
                for (unsigned i = 0; page->_tabs.Size() != 1 && i < 60; ++i) { co_await pause(dispatcher); }
                require(page->_tabs.Size() == 1 && !page->_tmuxTabs.contains(endedKey) && page->_GetFocusedTab() == page->_tmuxTabs.at(first.key()).get(),
                        "A clean remote session exit did not close its tab and return to the remaining session");
                note("EXIT: ending the remote test session automatically closed its tab and returned to the remaining session");
                while (page->_tabs.Size()) { page->_RemoveTab(page->_tabs.GetAt(0)); }
                require(page->TmuxEmptyWorkspace().Visibility() == Visibility::Visible && page->TmuxWorkspaceView().IsPaneOpen(), "Closing the final tab lost the expanded session picker");
                page->_FocusCurrentTab(false);
                note("CLOSE: detached, reopened, then closed all tabs while retaining the workspace");
                passed = true;
            }
            catch (const hresult_error& error) { log << "FAIL HRESULT: " << std::hex << static_cast<uint32_t>(error.code()) << " " << to_string(error.message()) << std::endl; }
            catch (const std::exception& error) { log << "FAIL: " << error.what() << std::endl; }
            catch (...) { note("FAIL: unknown exception"); }
            while (page->_tabs.Size()) { page->_RemoveTab(page->_tabs.GetAt(0)); }
            if (server && ownedSession)
            {
                co_await resume_background();
                const auto cleaned = ::TmuxWorkspace::runSsh(*server, "tmux kill-session -t " + ::TmuxWorkspace::quoteShell("=" + ownedSession->id), std::make_shared<std::atomic_bool>(false));
                co_await resume_foreground(dispatcher);
                if (cleaned.exitCode != 0) { passed = false; note("FAIL: test session cleanup failed"); }
                else { note("CLEANUP: removed only the session created by this test"); }
            }
            if (server && baselineSession)
            {
                co_await resume_background();
                const auto cleaned = ::TmuxWorkspace::runSsh(*server, "tmux kill-session -t " + ::TmuxWorkspace::quoteShell("=" + baselineSession->id), std::make_shared<std::atomic_bool>(false));
                co_await resume_foreground(dispatcher);
                if (cleaned.exitCode != 0) { passed = false; note("FAIL: baseline cleanup failed"); }
                else { note("CLEANUP: removed the temporary baseline session"); }
            }
            note(passed ? "PASS: real workspace workflow completed" : "FAIL: workflow incomplete");
            log.close();
            ExitProcess(passed ? 0 : 1);
        }
    };
}
