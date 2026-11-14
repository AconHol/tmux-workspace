// Licensed under the MIT license.
#include "pch.h"
#include "TerminalPage.h"
#include "TerminalPaneContent.h"
#include "SshCommandParser.h"
#include "TmuxUiHelpers.h"
#include "ContentManager.h"
#include <til/io.h>
#include <cwctype>
#include <json/json.h>

using namespace winrt;
using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Xaml::Controls;
using namespace winrt::Microsoft::Terminal::Settings::Model;
using namespace winrt::Microsoft::Terminal::TerminalConnection;

namespace
{
    std::filesystem::path serversPath()
    {
        return std::filesystem::path{ std::wstring_view{ CascadiaSettings::SettingsPath() } }.parent_path() / L"tmux-servers.json";
    }

    std::wstring trim(std::wstring value)
    {
        const auto first = value.find_first_not_of(L" \t\r\n");
        return first == std::wstring::npos ? std::wstring{} : value.substr(first, value.find_last_not_of(L" \t\r\n") - first + 1);
    }

    std::wstring folded(std::wstring value)
    {
        std::transform(value.begin(), value.end(), value.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
        return value;
    }

    std::wstring serverLabel(const ::TmuxWorkspace::Server& server)
    {
        if (!server.label.empty()) { return server.label; }
        auto label = server.user.empty() ? server.target : server.user + L"@" + server.target;
        if (!server.port.empty())
        {
            label += L":" + server.port;
        }
        return label;
    }

    std::wstring commandError(const ::TmuxWorkspace::CommandResult& result)
    {
        if (result.exitCode == 127 || result.error.find("TMUX_NOT_INSTALLED") != std::string::npos)
        {
            return L"需要安装 tmux，才能管理和保留服务器上的会话。也可以先打开 SSH 终端。";
        }
        const auto detail = to_hstring(result.error.empty() ? result.output : result.error);
        return std::wstring(detail).substr(0, 240) +
               L"\n本机可用密钥会自动用于连接。首次登录或需要输入口令时，可打开 SSH 终端完成登录。";
    }
}

#ifdef TMUX_WORKFLOW_TESTS
#include "../LocalTests_TmuxWorkspace/workflow_tests.inc"
#endif

namespace winrt::TerminalApp::implementation
{
    void TerminalPage::_TmuxInitialize()
    {
        try
        {
            const auto contents = til::io::read_file_as_utf8_string_if_exists(serversPath());
            if (!contents.empty())
            {
                Json::Value root;
                Json::CharReaderBuilder builder;
                const auto reader = std::unique_ptr<Json::CharReader>{ builder.newCharReader() };
                std::string errors;
                if (!reader->parse(contents.data(), contents.data() + contents.size(), &root, &errors) || !root.isArray())
                {
                    throw std::runtime_error("Invalid tmux-servers.json");
                }
                for (const auto& entry : root)
                {
                    ::TmuxWorkspace::Server server{
                        std::wstring(to_hstring(entry.get("target", "").asString())),
                        std::wstring(to_hstring(entry.get("user", "").asString())),
                        std::wstring(to_hstring(entry.get("port", "").asString())),
                        std::wstring(to_hstring(entry.get("identity", "").asString())),
                        std::wstring(to_hstring(entry.get("jumpHost", "").asString())),
                        std::wstring(to_hstring(entry.get("configFile", "").asString())),
                        std::wstring(to_hstring(entry.get("label", "").asString()))
                    };
                    if (::TmuxWorkspace::validate(server).empty())
                    {
                        _tmuxSavedServers.emplace_back(std::move(server));
                    }
                }
            }
        }
        catch (...)
        {
            TmuxHomeStatus().Text(L"已保存的连接无法读取，仍可从本机 SSH 配置连接或添加服务器。");
        }
        _TmuxRenderServers();
        _TmuxDiscoverServers();
        get_self<ContentManager>(_manager)->RegisterTmuxWindow(reinterpret_cast<uintptr_t>(this), Dispatcher(), [weak = get_weak()]() {
            if (const auto page = weak.get(); page && page->_tmuxAlive) { page->_TmuxRenderSessions(); }
        });
#ifdef TMUX_WORKFLOW_TESTS
        Initialized([weak = get_weak()](auto&&, auto&&) {
            if (auto page = weak.get()) { ::TerminalAppLocalTests::TmuxWorkflowTests::Start(std::move(page)); }
        });
#endif
        TmuxWorkspaceView().RegisterPropertyChangedCallback(SplitView::IsPaneOpenProperty(), [weak = get_weak()](auto&&, auto&&) {
            if (const auto page = weak.get()) { page->_TmuxApplySidebarState(); }
        });
        _TmuxApplySidebarState();
        Unloaded([weak = get_weak()](auto&&, auto&&) {
            if (const auto page = weak.get())
            {
                page->_tmuxAlive = false;
                page->_tmuxCancellation->store(true);
                get_self<ContentManager>(page->_manager)->UnregisterTmuxWindow(reinterpret_cast<uintptr_t>(page.get()));
            }
        });
    }

    void TerminalPage::_TmuxSetBusy(bool busy)
    {
        _tmuxBusy = busy;
        TmuxLoading().Visibility(busy ? Visibility::Visible : Visibility::Collapsed);
        TmuxNewButton().IsEnabled(!busy && !_tmuxMissing);
        TmuxRefreshButton().IsEnabled(!busy);
    }

    void TerminalPage::_TmuxConnectServer(::TmuxWorkspace::Server server)
    {
        const auto error = ::TmuxWorkspace::validate(server);
        if (!error.empty())
        {
            TmuxHomeStatus().Text(error);
            return;
        }
        ++_tmuxGeneration;
        _tmuxCancellation->store(true);
        _tmuxCancellation = std::make_shared<std::atomic_bool>(false);
        _tmuxServer = std::move(server);
        _tmuxMissing = false;
        TmuxProblem().IsOpen(false);
        _tmuxSessions.clear();
        _tmuxRenderSignature.clear();
        TmuxServerError().Text(L"");
        TmuxServerHeading().Text(serverLabel(*_tmuxServer));
        TmuxServerPicker().Visibility(Visibility::Collapsed);
        TmuxWorkspaceView().Visibility(Visibility::Visible);
        TmuxSidebar().Visibility(Visibility::Visible);
        TmuxEmptyWorkspace().Visibility(_tabs.Size() == 0 ? Visibility::Visible : Visibility::Collapsed);
        _TmuxRenderSessions();
        _TmuxRefresh(true);
    }

    void TerminalPage::_TmuxApplySidebarState()
    {
        const auto open = TmuxWorkspaceView().IsPaneOpen();
        TmuxSidebarBody().Visibility(open ? Visibility::Visible : Visibility::Collapsed);
        TmuxSidebarRail().Visibility(open ? Visibility::Collapsed : Visibility::Visible);
        const auto label = open ? L"折叠会话侧栏" : L"展开会话侧栏";
        Automation::AutomationProperties::SetName(TmuxSidebarToggle(), label);
        ToolTipService::SetToolTip(TmuxSidebarToggle(), box_value(label));
    }

    void TerminalPage::_TmuxToggleSidebarClicked(const Windows::Foundation::IInspectable&, const RoutedEventArgs&)
    {
        TmuxWorkspaceView().IsPaneOpen(!TmuxWorkspaceView().IsPaneOpen());
    }

    void TerminalPage::_TmuxSaveServer()
    {
        if (!_tmuxServer || std::find(_tmuxSavedServers.begin(), _tmuxSavedServers.end(), *_tmuxServer) != _tmuxSavedServers.end())
        {
            return;
        }
        auto updated = _tmuxSavedServers;
        std::erase_if(updated, [&](const auto& existing) { return ::TmuxWorkspace::sameConnection(existing, *_tmuxServer); });
        updated.insert(updated.begin(), *_tmuxServer);
        if (updated.size() > 20)
        {
            updated.resize(20);
        }
        Json::Value root{ Json::arrayValue };
        for (const auto& server : updated)
        {
            Json::Value entry;
            entry["target"] = to_string(server.target);
            entry["user"] = to_string(server.user);
            entry["port"] = to_string(server.port);
            entry["identity"] = to_string(server.identity);
            entry["jumpHost"] = to_string(server.jumpHost);
            entry["configFile"] = to_string(server.configFile);
            entry["label"] = to_string(server.label);
            root.append(std::move(entry));
        }
        Json::StreamWriterBuilder builder;
        builder["indentation"] = "  ";
        try
        {
            std::filesystem::create_directories(serversPath().parent_path());
            til::io::write_utf8_string_to_file_atomic(serversPath(), Json::writeString(builder, root));
            _tmuxSavedServers = std::move(updated);
            _TmuxRenderServers();
        }
        catch (...)
        {
            TmuxStatus().Text(L"连接成功，但无法保存连接信息。当前会话仍可正常使用。");
        }
    }

    safe_void_coroutine TerminalPage::_TmuxRefresh(bool manual)
    {
        if (!_tmuxServer || _tmuxBusy || !_tmuxAlive || _tmuxDialogOpen)
        {
            co_return;
        }
        const auto lifetime = get_strong();
        const auto dispatcher = Dispatcher();
        const auto server = *_tmuxServer;
        const auto generation = _tmuxGeneration;
        const auto cancellation = _tmuxCancellation;
        _TmuxSetBusy(true);
        if (manual || _tmuxSessions.empty())
        {
            TmuxStatus().Text(L"正在读取远端会话…");
        }
        co_await resume_background();
        std::vector<::TmuxWorkspace::Session> sessions;
        std::wstring error;
        bool missingTmux{};
        try
        {
            const auto result = ::TmuxWorkspace::runSsh(server, ::TmuxWorkspace::listCommand(), cancellation);
            missingTmux = result.exitCode == 127 || result.error.find("TMUX_NOT_INSTALLED") != std::string::npos;
            if (result.exitCode == 0)
            {
                sessions = ::TmuxWorkspace::parseSessions(result.output);
            }
            else if (result.exitCode != 1 || !::TmuxWorkspace::isEmptyServerError(result.error))
            {
                error = commandError(result);
            }
        }
        catch (const std::exception& e)
        {
            error = L"无法获取会话：" + std::wstring(to_hstring(e.what()));
        }
        catch (...)
        {
            error = L"无法启动 Windows OpenSSH，请确认已安装 OpenSSH 客户端。";
        }
        co_await resume_foreground(dispatcher);
        if (!_tmuxAlive || generation != _tmuxGeneration)
        {
            co_return;
        }
        _TmuxSetBusy(false);
        if (!error.empty())
        {
            // Preserve the previous snapshot rather than reporting an SSH
            // failure as an empty tmux server.
            _TmuxSetProblem(error, missingTmux);
            TmuxStatus().Text(missingTmux ? L"需要安装 tmux" : L"连接未就绪");
            _TmuxRenderSessions();
            co_return;
        }
        _tmuxSessions = std::move(sessions);
        _TmuxSetProblem(L"", false);
        _TmuxRenderSessions();
        TmuxStatus().Text(_tmuxSessions.empty() ? L"服务器上还没有会话，点击「新建会话」开始。"
                                               : std::to_wstring(_tmuxSessions.size()) + L" 个会话");
        _TmuxSaveServer();
    }

    void TerminalPage::_TmuxRefreshClicked(const Windows::Foundation::IInspectable&, const RoutedEventArgs&)
    {
        _TmuxRefresh(true);
    }

    void TerminalPage::_TmuxSearchChanged(const Windows::Foundation::IInspectable&, const TextChangedEventArgs&)
    {
        _TmuxRenderSessions();
    }

    void TerminalPage::_TmuxRenderSessions()
    {
        if (!_tmuxServer || _tmuxDialogOpen)
        {
            return;
        }
        const auto filter = folded(std::wstring(TmuxSessionSearch().Text()));
        const auto focused = _GetFocusedTab();
        std::wstring signature = filter;
        std::vector<ListViewItem> rows;
        Windows::Foundation::IInspectable selected{ nullptr };
        for (const auto& session : _tmuxSessions)
        {
            const auto name = std::wstring(to_hstring(session.name));
            if (folded(name).find(filter) == std::wstring::npos)
            {
                continue;
            }
            winrt::TerminalApp::Tab tab{ nullptr };
            if (const auto found = _tmuxTabs.find(session.key()); found != _tmuxTabs.end())
            {
                tab = found->second.get();
                uint32_t index{};
                if (tab && !_tabs.IndexOf(tab, index))
                {
                    tab = nullptr;
                }
            }
            std::wstring state = L"点击打开";
            if (tab)
            {
                const auto impl = _GetTabImpl(tab);
                impl->SetTabText(to_hstring(session.name));
                if (const auto control = impl->GetActiveTerminalControl())
                {
                    switch (control.ConnectionState())
                    {
                    case ConnectionState::Connected:
                        state = L"已打开";
                        break;
                    case ConnectionState::Connecting:
                    case ConnectionState::NotConnected:
                        state = L"连接中";
                        break;
                    default:
                        state = L"已断开 · 点击重连";
                        break;
                    }
                }
            }
            else if (get_self<ContentManager>(_manager)->HasTmuxSession(*_tmuxServer, session))
            {
                state = L"已在其他窗口打开 · 点击切换";
            }
            signature += L"\n" + std::wstring(to_hstring(session.key())) + L"\t" + name +
                         L"\t" + std::to_wstring(session.windows) + L"\t" + std::to_wstring(session.clients) + state +
                         (tab && tab == focused ? L"*" : L"");
            ListViewItem row;
            row.Tag(box_value(to_hstring(session.key())));
            row.HorizontalContentAlignment(HorizontalAlignment::Stretch);
            row.Padding(Thickness{ 10, 10, 10, 10 });
            StackPanel content;
            content.Tag(row.Tag());
            TextBlock title;
            title.Text(name);
            title.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
            title.TextTrimming(TextTrimming::CharacterEllipsis);
            content.Children().Append(title);
            TextBlock detail;
            detail.Text(std::to_wstring(session.windows) + L" 个窗口 · " + state);
            detail.FontSize(12);
            detail.Opacity(0.65);
            detail.Margin(Thickness{ 0, 5, 0, 0 });
            content.Children().Append(detail);
            if (session.clients > 0)
            {
                TextBlock clients;
                clients.Text(L"远端连接数 " + std::to_wstring(session.clients));
                clients.FontSize(11);
                clients.Opacity(0.5);
                clients.Margin(Thickness{ 0, 3, 0, 0 });
                content.Children().Append(clients);
            }
            row.Content(content);
            if (tab && tab == focused)
            {
                selected = row;
            }
            rows.emplace_back(std::move(row));
        }
        if (signature == _tmuxRenderSignature)
        {
            return;
        }
        _tmuxRenderSignature = std::move(signature);
        TmuxSessionList().Items().Clear();
        for (const auto& row : rows)
        {
            TmuxSessionList().Items().Append(row);
        }
        TmuxSessionList().SelectedItem(selected);
    }

    void TerminalPage::_TmuxSyncSelection()
    {
        _TmuxRenderSessions();
    }

    void TerminalPage::_TmuxTabRemoved(const winrt::TerminalApp::Tab& tab)
    {
        std::erase_if(_tmuxTabs, [&](const auto& entry) {
            const auto existing = entry.second.get();
            const auto removed = !existing || existing == tab;
            if (removed) { get_self<ContentManager>(_manager)->ReleaseTmuxSession(reinterpret_cast<uintptr_t>(this), entry.first); }
            return removed;
        });
        _TmuxRenderSessions();
        _TmuxRefresh();
    }

    void TerminalPage::_TmuxRegisterTab(const winrt::TerminalApp::Tab& tab, const std::optional<::TmuxWorkspace::Session>& session, bool installer)
    {
        if (_tmuxServer)
        {
            if (const auto control = _GetTabImpl(tab)->GetActiveTerminalControl())
            {
                get_self<ContentManager>(_manager)->SetTmuxContext(control.ContentId(), { *_tmuxServer, session, installer });
                if (session) { _TmuxBindGlobalTab(tab, session->key()); }
            }
        }
    }

    void TerminalPage::_TmuxRestoreTab(const winrt::TerminalApp::Tab& tab)
    {
        const auto control = _GetTabImpl(tab)->GetActiveTerminalControl();
        if (!control) { return; }
        if (const auto context = get_self<ContentManager>(_manager)->GetTmuxContext(control.ContentId()))
        {
            if (!_tmuxServer || !::TmuxWorkspace::sameConnection(*_tmuxServer, context->server))
            {
                _TmuxConnectServer(context->server);
            }
            if (context->session)
            {
                _tmuxTabs.insert_or_assign(context->session->key(), make_weak(tab));
                _TmuxBindGlobalTab(tab, context->session->key());
                if (std::none_of(_tmuxSessions.begin(), _tmuxSessions.end(), [&](const auto& existing) { return existing.key() == context->session->key(); }))
                {
                    _tmuxSessions.emplace_back(*context->session);
                }
            }
            else if (context->installer) { _tmuxInstallTab = make_weak(tab); }
            else { _tmuxSshTab = make_weak(tab); }
            _tmuxRenderSignature.clear();
            _TmuxRenderSessions();
        }
    }

    void TerminalPage::_TmuxBindGlobalTab(const winrt::TerminalApp::Tab& tab, const std::string& sessionKey)
    {
        if (const auto control = _GetTabImpl(tab)->GetActiveTerminalControl())
        {
            get_self<ContentManager>(_manager)->BindTmuxSession(control.ContentId(), reinterpret_cast<uintptr_t>(this), Dispatcher(),
                [weak = get_weak(), sessionKey]() {
                    if (const auto page = weak.get(); page && page->_tmuxAlive)
                    {
                        const auto found = page->_tmuxTabs.find(sessionKey);
                        const auto target = found == page->_tmuxTabs.end() ? winrt::TerminalApp::Tab{ nullptr } : found->second.get();
                        uint32_t index{};
                        if (target && page->_tabs.IndexOf(target, index))
                        {
                            page->_SelectTab(index);
                            const auto content = page->_GetTabImpl(target)->GetActiveContent().try_as<winrt::TerminalApp::TerminalPaneContent>();
                            if (content && content.GetTermControl().ConnectionState() >= ConnectionState::Closed)
                            {
                                page->_restartPaneConnection(content, nullptr);
                            }
                            page->SummonWindowRequested.raise(*page, nullptr);
                            page->_FocusCurrentTab(false);
                        }
                    }
                });
        }
    }

    void TerminalPage::_TmuxSessionClicked(const Windows::Foundation::IInspectable&, const ItemClickEventArgs& args)
    {
        const auto key = to_string(unbox_value_or<hstring>(::TmuxWorkspace::clickedRowTag(args.ClickedItem()), hstring{}));
        const auto found = std::find_if(_tmuxSessions.begin(), _tmuxSessions.end(), [&](const auto& session) { return session.key() == key; });
        if (found != _tmuxSessions.end())
        {
            // The vector can be replaced by a later refresh; use a value here.
            _TmuxOpenSession(*found);
        }
    }

    void TerminalPage::_TmuxOpenSession(const ::TmuxWorkspace::Session& session)
    {
        if (!_tmuxServer)
        {
            return;
        }
        try
        {
            if (const auto found = _tmuxTabs.find(session.key()); found != _tmuxTabs.end())
            {
                if (const auto tab = found->second.get())
                {
                    uint32_t index{};
                    if (_tabs.IndexOf(tab, index))
                    {
                        _SelectTab(index);
                        const auto impl = _GetTabImpl(tab);
                        const auto content = impl->GetActiveContent().try_as<winrt::TerminalApp::TerminalPaneContent>();
                        if (content && content.GetTermControl().ConnectionState() >= ConnectionState::Closed)
                        {
                            _restartPaneConnection(content, nullptr);
                        }
                        _TmuxRenderSessions();
                        return;
                    }
                }
                _tmuxTabs.erase(found);
            }
            const auto manager = get_self<ContentManager>(_manager)->get_strong();
            const auto claim = manager->ClaimTmuxSession(*_tmuxServer, session);
            if (!claim) { _TmuxRenderSessions(); return; }
            auto releasePendingClaim = wil::scope_exit([&]() noexcept {
                try { manager->ReleaseTmuxClaim(claim); }
                CATCH_LOG();
            });
            NewTerminalArgs args;
            args.Profile(to_hstring(_settings.GlobalSettings().DefaultProfile()));
            args.Commandline(::TmuxWorkspace::quoteArgument(::TmuxWorkspace::sshExecutable()) +
                             ::TmuxWorkspace::sshArguments(*_tmuxServer, true) + L" " +
                             ::TmuxWorkspace::quoteArgument(to_hstring("sh -c " + ::TmuxWorkspace::quoteShell(::TmuxWorkspace::attachCommand(session)))));
            args.TabTitle(to_hstring(session.name));
            args.SuppressApplicationTitle(true);
            args.Elevate(false);
            const auto pane = _MakePane(args, nullptr);
            if (!pane)
            {
                throw std::runtime_error("Unable to create terminal pane");
            }
            if (const auto content = pane->GetContent().try_as<winrt::TerminalApp::TerminalPaneContent>())
            {
                get_self<TerminalPaneContent>(content)->KeepOpenForTmux();
            }
            const auto tab = _CreateNewTabFromPane(pane);
            if (tab)
            {
                // Installed synchronously on the UI thread, before another
                // click can create a second tab for the same session.
                _tmuxTabs.insert_or_assign(session.key(), make_weak(tab));
                _TmuxRegisterTab(tab, session);
                _TmuxRenderSessions();
            }
        }
        catch (...)
        {
            TmuxStatus().Text(L"无法打开会话。请检查 Windows OpenSSH 是否可用，刷新列表后重试。");
        }
    }

    void TerminalPage::_TmuxVerifySshClicked(const Windows::Foundation::IInspectable&, const RoutedEventArgs&)
    {
        if (!_tmuxServer)
        {
            return;
        }
        try
        {
            if (const auto tab = _tmuxSshTab.get())
            {
                uint32_t index{};
                if (_tabs.IndexOf(tab, index))
                {
                    _SelectTab(index);
                    if (const auto content = _GetTabImpl(tab)->GetActiveContent().try_as<winrt::TerminalApp::TerminalPaneContent>();
                        content && content.GetTermControl().ConnectionState() >= ConnectionState::Closed)
                    {
                        _restartPaneConnection(content, nullptr);
                    }
                    return;
                }
            }
            NewTerminalArgs args;
            args.Profile(to_hstring(_settings.GlobalSettings().DefaultProfile()));
            args.Commandline(::TmuxWorkspace::quoteArgument(::TmuxWorkspace::sshExecutable()) +
                             ::TmuxWorkspace::sshArguments(*_tmuxServer, true));
            args.TabTitle(L"SSH");
            args.SuppressApplicationTitle(true);
            args.Elevate(false);
            THROW_IF_FAILED(_OpenNewTab(args));
            if (const auto tab = _GetFocusedTab()) { _tmuxSshTab = make_weak(tab); _TmuxRegisterTab(tab); }
        }
        catch (...)
        {
            TmuxStatus().Text(L"无法启动 SSH 验证，请检查 Windows OpenSSH 客户端。");
        }
    }

    safe_void_coroutine TerminalPage::_TmuxSwitchServerClicked(Windows::Foundation::IInspectable, RoutedEventArgs)
    {
        const auto lifetime = get_strong();
        if (_tmuxDialogOpen)
        {
            co_return;
        }
        if (_tabs.Size() != 0)
        {
            ContentDialog dialog;
            dialog.Title(box_value(L"切换服务器？"));
            dialog.Content(box_value(L"将关闭当前窗口中的连接标签页。远端 tmux 会话和其中的任务会继续运行。"));
            dialog.PrimaryButtonText(L"断开并切换");
            dialog.CloseButtonText(L"取消");
            dialog.DefaultButton(ContentDialogButton::Close);
            const auto presenter = _dialogPresenter.get();
            if (!presenter)
            {
                co_return;
            }
            _tmuxDialogOpen = true;
            auto resetDialog = wil::scope_exit([&]() noexcept { _tmuxDialogOpen = false; });
            if (co_await presenter.ShowDialog(dialog) != ContentDialogResult::Primary)
            {
                co_return;
            }
        }
        ++_tmuxGeneration;
        _tmuxCancellation->store(true);
        _tmuxServer.reset();
        while (_tabs.Size() != 0)
        {
            _RemoveTab(_tabs.GetAt(0));
        }
        _tmuxTabs.clear();
        _tmuxSessions.clear();
        _TmuxSetBusy(false);
        TmuxSessionSearch().Text(L"");
        TmuxSessionList().Items().Clear();
        TmuxSidebar().Visibility(Visibility::Collapsed);
        TmuxWorkspaceView().Visibility(Visibility::Collapsed);
        TmuxServerPicker().Visibility(Visibility::Visible);
        _tmuxMissing = false;
        TmuxProblem().IsOpen(false);
        _TmuxRenderServers();
        TmuxServerList().Focus(FocusState::Programmatic);
    }

    void TerminalPage::_TmuxNewNameChanged(const Windows::Foundation::IInspectable&, const TextChangedEventArgs&)
    {
        const auto name = std::wstring(TmuxNewSessionName().Text());
        auto error = ::TmuxWorkspace::validateSessionName(name);
        const auto exists = error.empty() && std::any_of(_tmuxSessions.begin(), _tmuxSessions.end(), [&](const auto& session) { return session.name == to_string(name); });
        TmuxNewSessionDialog().PrimaryButtonText(exists ? L"打开已有会话" : L"新建并打开");
        TmuxNewSessionDialog().IsPrimaryButtonEnabled(error.empty());
        TmuxNameValidation().Text(exists ? L"已有同名会话，可以直接打开，也可以继续修改名称。" :
                                  name.empty() ? L"名称支持中文、英文、数字、空格和连字符。" : error);
    }

    safe_void_coroutine TerminalPage::_TmuxNewSessionClicked(Windows::Foundation::IInspectable, RoutedEventArgs)
    {
        if (!_tmuxServer || _tmuxMissing || _tmuxBusy || _tmuxDialogOpen)
        {
            co_return;
        }
        const auto lifetime = get_strong();
        const auto dispatcher = Dispatcher();
        const auto generation = _tmuxGeneration;
        _tmuxDialogOpen = true;
        auto resetDialog = wil::scope_exit([&]() noexcept {
            _tmuxDialogOpen = false;
            try { _TmuxRenderSessions(); _FocusCurrentTab(false); _TmuxRefresh(); }
            CATCH_LOG();
        });
        TmuxNewSessionName().Text(L"");
        TmuxNewSessionDialog().PrimaryButtonText(L"新建并打开");
        TmuxNewSessionDialog().IsPrimaryButtonEnabled(false);
        const auto answer = co_await _ShowDialogHelper(L"TmuxNewSessionDialog");
        if (answer != ContentDialogResult::Primary || !_tmuxServer || generation != _tmuxGeneration)
        {
            co_return;
        }
        const auto name = std::wstring(TmuxNewSessionName().Text());
        if (!::TmuxWorkspace::validateSessionName(name).empty())
        {
            co_return;
        }
        if (const auto existing = std::find_if(_tmuxSessions.begin(), _tmuxSessions.end(), [&](const auto& session) { return session.name == to_string(name); }); existing != _tmuxSessions.end())
        {
            _TmuxOpenSession(*existing);
            co_return;
        }
        const auto server = *_tmuxServer;
        const auto cancellation = _tmuxCancellation;
        _TmuxSetBusy(true);
        TmuxStatus().Text(L"正在创建会话…");
        co_await resume_background();
        std::vector<::TmuxWorkspace::Session> created;
        std::wstring error;
        try
        {
            const auto result = ::TmuxWorkspace::runSsh(server, ::TmuxWorkspace::createCommand(to_string(name)), cancellation);
            if (result.exitCode == 0)
            {
                created = ::TmuxWorkspace::parseSessions(result.output);
                if (created.size() != 1)
                {
                    error = L"服务器没有返回唯一的新会话。请刷新列表检查创建结果。";
                }
            }
            else
            {
                error = commandError(result);
            }
        }
        catch (const std::exception& e)
        {
            error = L"创建结果未能确认，请先刷新列表检查是否已创建：\n" + std::wstring(to_hstring(e.what()));
        }
        catch (...)
        {
            error = L"创建结果未能确认，请刷新列表检查，避免重复创建。";
        }
        co_await resume_foreground(dispatcher);
        if (!_tmuxAlive || generation != _tmuxGeneration)
        {
            co_return;
        }
        _TmuxSetBusy(false);
        if (!error.empty())
        {
            TmuxStatus().Text(error);
            co_return;
        }
        _tmuxSessions.emplace_back(created.front());
        TmuxSessionSearch().Text(L"");
        _TmuxOpenSession(created.front());
        _TmuxRenderSessions();
        TmuxStatus().Text(L"会话已创建并打开。关闭标签页后，远端会话仍会保留。");
        _TmuxRefresh();
    }
}
