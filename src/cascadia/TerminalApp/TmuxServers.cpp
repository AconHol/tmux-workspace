// Licensed under the MIT license.
#include "pch.h"
#include "TerminalPage.h"
#include "TerminalPaneContent.h"
#include "SshCommandParser.h"
#include "TmuxUiHelpers.h"
#include <cmath>

using namespace winrt;
using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Xaml::Controls;
using namespace winrt::Microsoft::Terminal::Settings::Model;
using namespace winrt::Microsoft::Terminal::TerminalConnection;

namespace
{
    std::wstring connectionLabel(const ::TmuxWorkspace::Server& server)
    {
        auto text = server.user.empty() ? server.target : server.user + L"@" + server.target;
        if (!server.port.empty()) { text += L":" + server.port; }
        return text;
    }

    void appendCombo(ComboBox combo, const hstring& text, const hstring& tag, bool selected)
    {
        ComboBoxItem item;
        item.Content(box_value(text));
        item.Tag(box_value(tag));
        combo.Items().Append(item);
        if (selected) { combo.SelectedItem(item); }
    }

    hstring selectedTag(ComboBox combo)
    {
        if (const auto item = combo.SelectedItem().try_as<ComboBoxItem>())
        {
            return unbox_value<hstring>(item.Tag());
        }
        return {};
    }
}

namespace winrt::TerminalApp::implementation
{
    Windows::Foundation::IAsyncOperation<ContentDialogResult> TerminalPage::ShowTmuxInputDialog(ContentDialog dialog)
    {
        // https://learn.microsoft.com/windows/apps/desktop/modernize/xaml-islands/xaml-islands
        // Input controls must remain in the island's normal visual tree.
        const auto lifetime = get_strong();
        const auto dispatcher = Dispatcher();
        if (_tmuxDismissInput || !dialog) { co_return ContentDialogResult::None; }
        const auto body = dialog.Content();
        const auto previousFocus = Input::FocusManager::GetFocusedElement(XamlRoot()).try_as<Control>();
        wil::unique_handle completed{ CreateEventW(nullptr, TRUE, FALSE, nullptr) };
        THROW_LAST_ERROR_IF(!completed);
        auto result = ContentDialogResult::None;
        bool finished{};
        const auto finish = [&](ContentDialogResult answer) {
            if (!finished) { finished = true; result = answer; SetEvent(completed.get()); }
        };
        _tmuxDismissInput = [&]() { finish(ContentDialogResult::None); };
        const auto sync = [this, dialog]() {
            TmuxInputPrimary().Content(box_value(dialog.PrimaryButtonText()));
            TmuxInputPrimary().IsEnabled(dialog.IsPrimaryButtonEnabled());
        };
        dialog.Content(nullptr);
        auto restore = wil::scope_exit([&]() noexcept {
            _tmuxDismissInput = {};
            try
            {
                TmuxInputLayer().Visibility(Visibility::Collapsed);
                TmuxInputBody().Content(nullptr);
                dialog.Content(body);
                if (previousFocus) { previousFocus.Focus(FocusState::Programmatic); }
            }
            CATCH_LOG();
        });
        TmuxInputLayer().RequestedTheme(_settings.GlobalSettings().CurrentTheme().RequestedTheme());
        TmuxInputTitle().Text(unbox_value_or<hstring>(dialog.Title(), hstring{}));
        Automation::AutomationProperties::SetName(TmuxInputCard(), TmuxInputTitle().Text());
        TmuxInputClose().Content(box_value(dialog.CloseButtonText()));
        TmuxInputBody().Content(body);
        sync();
        const auto enabledToken = dialog.RegisterPropertyChangedCallback(ContentDialog::IsPrimaryButtonEnabledProperty(), [sync](auto&&, auto&&) { sync(); });
        const auto textToken = dialog.RegisterPropertyChangedCallback(ContentDialog::PrimaryButtonTextProperty(), [sync](auto&&, auto&&) { sync(); });
        auto detachBindings = wil::scope_exit([&]() noexcept {
            dialog.UnregisterPropertyChangedCallback(ContentDialog::IsPrimaryButtonEnabledProperty(), enabledToken);
            dialog.UnregisterPropertyChangedCallback(ContentDialog::PrimaryButtonTextProperty(), textToken);
        });
        const auto fit = [this]() { TmuxInputCard().MaxHeight(std::max(160.0, Root().ActualHeight() - 48.0)); };
        fit();
        auto resize = Root().SizeChanged(auto_revoke, [fit](auto&&, auto&&) { fit(); });
        const auto submit = [&]() {
            if (!dialog.IsPrimaryButtonEnabled()) { return; }
            if (dialog.Name() == L"TmuxTranslationDialog") { _TmuxTranslateSubmit(); }
            else { finish(ContentDialogResult::Primary); }
        };
        auto accept = TmuxInputPrimary().Click(auto_revoke, [&](auto&&, auto&&) { submit(); });
        auto cancel = TmuxInputClose().Click(auto_revoke, [&](auto&&, auto&&) { finish(ContentDialogResult::None); });
        auto keyboard = TmuxInputLayer().KeyDown(auto_revoke, [&](auto&&, const Input::KeyRoutedEventArgs& event) {
            if (event.Key() == Windows::System::VirtualKey::Escape) { finish(ContentDialogResult::None); event.Handled(true); }
            else if (event.Key() == Windows::System::VirtualKey::Enter && dialog.IsPrimaryButtonEnabled() &&
                     (dialog.Name() != L"TmuxTranslationDialog" || (GetKeyState(VK_CONTROL) & 0x8000))) { submit(); event.Handled(true); }
        });
        auto translationKeyboard = TmuxInputLayer().PreviewKeyDown(auto_revoke, [&](auto&&, const Input::KeyRoutedEventArgs& event) {
            if (dialog.Name() == L"TmuxTranslationDialog" && event.Key() == Windows::System::VirtualKey::Enter && (GetKeyState(VK_CONTROL) & 0x8000))
            { submit(); event.Handled(true); }
        });
        TmuxInputLayer().Visibility(Visibility::Visible);
        co_await resume_foreground(dispatcher, Windows::UI::Core::CoreDispatcherPriority::Low);
        if (dialog.Name() == L"TmuxNewSessionDialog") { TmuxNewSessionName().Focus(FocusState::Programmatic); }
        else if (dialog.Name() == L"TmuxAddServerDialog") { TmuxSshCommand().Focus(FocusState::Programmatic); }
        else if (dialog.Name() == L"TmuxTranslationDialog") { TmuxTranslationText().Focus(FocusState::Programmatic); }
        else { TmuxTheme().Focus(FocusState::Programmatic); }
        co_await resume_on_signal(completed.get());
        co_await resume_foreground(dispatcher);
        co_return result;
    }

    safe_void_coroutine TerminalPage::_TmuxDiscoverServers()
    {
        const auto lifetime = get_strong();
        const auto dispatcher = Dispatcher();
        std::vector<::TmuxWorkspace::Server> imported;
        // Reuse upstream's dynamic SSH profiles before filling in Include
        // files and aliases without HostName that the old generator omits.
        for (const auto& profile : _settings.AllProfiles())
        {
            if (profile.Source() == L"Windows.Terminal.SSH" && !profile.Deleted())
            {
                try { imported.emplace_back(::TmuxWorkspace::parseSshCommand(profile.Commandline())); }
                catch (...) {}
            }
        }
        co_await resume_background();
        try
        {
            for (auto&& server : ::TmuxWorkspace::discoverSshServers())
            {
                if (std::none_of(imported.begin(), imported.end(), [&](const auto& existing) {
                    return ::TmuxWorkspace::sameConnection(existing, server);
                }))
                {
                    imported.emplace_back(std::move(server));
                }
            }
        }
        catch (...) {}
        co_await resume_foreground(dispatcher);
        if (!_tmuxAlive) { co_return; }
        _tmuxImportedServers = std::move(imported);
        _TmuxRenderServers();
    }

    void TerminalPage::_TmuxRenderServers()
    {
        // Do not invalidate the item that ListView is still processing when
        // a fast SSH response saves a server during navigation.
        if (TmuxServerPicker().Visibility() != Visibility::Visible) { return; }
        _tmuxServerChoices = _tmuxSavedServers;
        const auto savedCount = _tmuxServerChoices.size();
        for (const auto& server : _tmuxImportedServers)
        {
            if (std::none_of(_tmuxServerChoices.begin(), _tmuxServerChoices.end(), [&](const auto& existing) {
                return ::TmuxWorkspace::sameConnection(existing, server);
            }))
            {
                _tmuxServerChoices.emplace_back(server);
            }
        }
        TmuxServerList().Items().Clear();
        for (size_t i = 0; i < _tmuxServerChoices.size(); ++i)
        {
            const auto& server = _tmuxServerChoices[i];
            ListViewItem item;
            item.Tag(box_value(static_cast<uint32_t>(i)));
            item.HorizontalContentAlignment(HorizontalAlignment::Stretch);
            item.Padding(Thickness{ 16, 16, 16, 16 });
            item.Margin(Thickness{ 0, 0, 0, 8 });
            Grid content;
            content.Tag(item.Tag());
            ColumnDefinition iconColumn;
            iconColumn.Width(GridLength{ 40, GridUnitType::Pixel });
            ColumnDefinition nameColumn;
            nameColumn.Width(GridLength{ 1, GridUnitType::Star });
            ColumnDefinition arrowColumn;
            arrowColumn.Width(GridLength{ 24, GridUnitType::Pixel });
            content.ColumnDefinitions().Append(iconColumn);
            content.ColumnDefinitions().Append(nameColumn);
            content.ColumnDefinitions().Append(arrowColumn);
            FontIcon icon;
            icon.Glyph(L"\xE968");
            icon.FontSize(20);
            icon.VerticalAlignment(VerticalAlignment::Center);
            content.Children().Append(icon);
            StackPanel text;
            Grid::SetColumn(text, 1);
            text.Spacing(4);
            TextBlock title;
            title.Text(server.label.empty() ? server.target : server.label);
            title.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
            title.TextTrimming(TextTrimming::CharacterEllipsis);
            text.Children().Append(title);
            TextBlock detail;
            detail.Text(connectionLabel(server) + (i >= savedCount ? L"  ·  本机 SSH 配置" : L""));
            detail.FontSize(12);
            detail.Opacity(0.65);
            detail.TextTrimming(TextTrimming::CharacterEllipsis);
            text.Children().Append(detail);
            content.Children().Append(text);
            FontIcon arrow;
            arrow.Glyph(L"\xE76C");
            arrow.FontSize(12);
            arrow.Opacity(0.6);
            Grid::SetColumn(arrow, 2);
            content.Children().Append(arrow);
            item.Content(content);
            TmuxServerList().Items().Append(item);
        }
        TmuxNoServers().Visibility(_tmuxServerChoices.empty() ? Visibility::Visible : Visibility::Collapsed);
        if (!_tmuxServerChoices.empty())
        {
            TmuxHomeStatus().Text(L"自动使用本机 SSH 配置和可用密钥。");
        }
    }

    void TerminalPage::_TmuxServerClicked(const Windows::Foundation::IInspectable&, const ItemClickEventArgs& args)
    {
        const auto index = unbox_value_or<uint32_t>(::TmuxWorkspace::clickedRowTag(args.ClickedItem()), UINT32_MAX);
        if (index < _tmuxServerChoices.size())
        {
            try { _TmuxConnectServer(_tmuxServerChoices[index]); }
            catch (...) { TmuxHomeStatus().Text(L"无法打开服务器，请重新选择或检查连接设置。"); }
        }
    }

    ::TmuxWorkspace::Server TerminalPage::_TmuxServerFromForm()
    {
        return {
            ::TmuxWorkspace::trimSsh(TmuxHost().Text()),
            ::TmuxWorkspace::trimSsh(TmuxUser().Text()),
            ::TmuxWorkspace::trimSsh(TmuxPort().Text()),
            ::TmuxWorkspace::trimSsh(TmuxIdentity().Text()),
            ::TmuxWorkspace::trimSsh(TmuxJumpHost().Text()),
            ::TmuxWorkspace::trimSsh(TmuxConfigFile().Text()),
            ::TmuxWorkspace::trimSsh(TmuxServerLabel().Text())
        };
    }

    void TerminalPage::_TmuxValidateServerForm()
    {
        const auto server = _TmuxServerFromForm();
        const auto error = ::TmuxWorkspace::validate(server);
        TmuxServerError().Text(error);
        TmuxAddServerDialog().IsPrimaryButtonEnabled(error.empty());
        TmuxConnectionPreview().Visibility(error.empty() ? Visibility::Visible : Visibility::Collapsed);
        if (error.empty())
        {
            auto summary = connectionLabel(server);
            summary += server.identity.empty() ? L"\n使用此电脑的可用密钥" : L"\n密钥：" + std::filesystem::path(server.identity).filename().wstring();
            if (!server.jumpHost.empty()) { summary += L"\n通过 " + server.jumpHost; }
            if (!server.configFile.empty()) { summary += L"\n使用指定的 SSH 配置文件"; }
            TmuxCommandSummary().Text(summary);
        }
    }

    void TerminalPage::_TmuxSshCommandChanged(const Windows::Foundation::IInspectable&, const TextChangedEventArgs&)
    {
        if (_tmuxUpdatingForm || !TmuxAddServerDialog()) { return; }
        _tmuxUpdatingForm = true;
        auto reset = wil::scope_exit([&]() noexcept { _tmuxUpdatingForm = false; });
        try
        {
            const auto server = ::TmuxWorkspace::parseSshCommand(TmuxSshCommand().Text());
            TmuxHost().Text(server.target);
            TmuxUser().Text(server.user);
            TmuxPort().Text(server.port);
            TmuxIdentity().Text(server.identity);
            TmuxJumpHost().Text(server.jumpHost);
            TmuxConfigFile().Text(server.configFile);
            _TmuxValidateServerForm();
        }
        catch (const std::exception& error)
        {
            TmuxConnectionPreview().Visibility(Visibility::Collapsed);
            TmuxAddServerDialog().IsPrimaryButtonEnabled(false);
            if (::TmuxWorkspace::trimSsh(TmuxSshCommand().Text()).empty())
            {
                TmuxServerError().Text(L"");
            }
            else
            {
                const std::string_view message{ error.what() };
                if (message.find("quote") != std::string_view::npos) { TmuxServerError().Text(L"引号没有闭合，请检查指令。"); }
                else if (message.find("Remote commands") != std::string_view::npos || message.find("Shell operators") != std::string_view::npos)
                { TmuxServerError().Text(L"请只填写 SSH 连接指令，不要附加远程命令或命令链。"); }
                else if (message.find("Unsupported SSH option") != std::string_view::npos || message.find("other SSH options") != std::string_view::npos)
                { TmuxServerError().Text(L"支持 -p、-l、-i、-J、-F。其他连接选项请写入 SSH 配置，再使用 Host 别名。"); }
                else { TmuxServerError().Text(L"请检查主机和连接参数。例如：ssh user@host -p 22"); }
            }
        }
    }

    void TerminalPage::_TmuxDetailsChanged(const Windows::Foundation::IInspectable&, const TextChangedEventArgs&)
    {
        if (!_tmuxUpdatingForm && TmuxAddServerDialog())
        {
            _TmuxValidateServerForm();
        }
    }

    safe_void_coroutine TerminalPage::_TmuxAddServerClicked(Windows::Foundation::IInspectable, RoutedEventArgs)
    {
        if (_tmuxDialogOpen) { co_return; }
        const auto lifetime = get_strong();
        _tmuxDialogOpen = true;
        auto resetDialog = wil::scope_exit([&]() noexcept { _tmuxDialogOpen = false; });
        _tmuxUpdatingForm = true;
        TmuxSshCommand().Text(L"");
        for (const auto box : { TmuxHost(), TmuxUser(), TmuxPort(), TmuxIdentity(), TmuxJumpHost(), TmuxConfigFile(), TmuxServerLabel() })
        {
            box.Text(L"");
        }
        _tmuxUpdatingForm = false;
        TmuxServerError().Text(L"");
        TmuxConnectionPreview().Visibility(Visibility::Collapsed);
        TmuxAddServerDialog().IsPrimaryButtonEnabled(false);
        if (co_await _ShowDialogHelper(L"TmuxAddServerDialog") == ContentDialogResult::Primary)
        {
            const auto server = _TmuxServerFromForm();
            if (::TmuxWorkspace::validate(server).empty())
            {
                _TmuxConnectServer(server);
                _TmuxSaveServer();
            }
        }
    }

    void TerminalPage::_TmuxSetProblem(const std::wstring& message, bool missingTmux)
    {
        _tmuxMissing = missingTmux;
        TmuxProblem().Title(missingTmux ? L"此服务器未安装 tmux" : L"无法完成连接");
        TmuxProblem().Message(message);
        TmuxProblem().IsOpen(!message.empty());
        TmuxProblemAction().Content(box_value(missingTmux ? L"安装 tmux" : L"打开 SSH 终端"));
        TmuxNewButton().IsEnabled(!_tmuxBusy && !missingTmux);
    }

    void TerminalPage::_TmuxProblemActionClicked(const Windows::Foundation::IInspectable&, const RoutedEventArgs&)
    {
        if (_tmuxMissing) { _TmuxInstall(); }
        else { _TmuxVerifySshClicked(nullptr, nullptr); }
    }

    safe_void_coroutine TerminalPage::_TmuxInstall()
    {
        if (!_tmuxServer || _tmuxDialogOpen) { co_return; }
        const auto lifetime = get_strong();
        if (const auto existing = _tmuxInstallTab.get())
        {
            uint32_t index{};
            if (_tabs.IndexOf(existing, index))
            {
                _SelectTab(index);
                if (const auto content = _GetTabImpl(existing)->GetActiveContent().try_as<winrt::TerminalApp::TerminalPaneContent>();
                    content && content.GetTermControl().ConnectionState() >= ConnectionState::Closed)
                { _restartPaneConnection(content, nullptr); }
                co_return;
            }
        }
        ContentDialog dialog;
        dialog.Title(box_value(L"安装 tmux"));
        TextBlock body;
        body.Text(L"将在当前服务器上使用系统包管理器安装 tmux，可能需要在 SSH 终端中输入 sudo 密码。");
        body.TextWrapping(TextWrapping::Wrap);
        dialog.Content(body);
        dialog.PrimaryButtonText(L"安装");
        dialog.CloseButtonText(L"取消");
        dialog.DefaultButton(ContentDialogButton::Primary);
        const auto presenter = _dialogPresenter.get();
        if (!presenter) { co_return; }
        _tmuxDialogOpen = true;
        auto reset = wil::scope_exit([&]() noexcept { _tmuxDialogOpen = false; });
        if (co_await presenter.ShowDialog(dialog) != ContentDialogResult::Primary) { co_return; }
        try
        {
            NewTerminalArgs args;
            args.Profile(to_hstring(_settings.GlobalSettings().DefaultProfile()));
            args.Commandline(::TmuxWorkspace::quoteArgument(::TmuxWorkspace::sshExecutable()) +
                ::TmuxWorkspace::sshArguments(*_tmuxServer, true) + L" " +
                ::TmuxWorkspace::quoteArgument(to_hstring("sh -c " + ::TmuxWorkspace::quoteShell(::TmuxWorkspace::installTmuxCommand()))));
            args.TabTitle(L"安装 tmux");
            args.SuppressApplicationTitle(true);
            args.Elevate(false);
            THROW_IF_FAILED(_OpenNewTab(args));
            if (const auto tab = _GetFocusedTab())
            {
                _tmuxInstallTab = make_weak(tab);
                _TmuxRegisterTab(tab, std::nullopt, true);
                if (const auto content = _GetTabImpl(tab)->GetActiveContent().try_as<winrt::TerminalApp::TerminalPaneContent>())
                { get_self<TerminalPaneContent>(content)->KeepOpenForTmux(); }
            }
        }
        catch (...) { TmuxStatus().Text(L"无法打开安装终端，请从服务器菜单打开 SSH 终端手动安装。"); }
    }

    safe_void_coroutine TerminalPage::_TmuxPersonalizationClicked(Windows::Foundation::IInspectable, RoutedEventArgs)
    {
        if (_tmuxDialogOpen) { co_return; }
        const auto lifetime = get_strong();
        _tmuxDialogOpen = true;
        auto reset = wil::scope_exit([&]() noexcept { _tmuxDialogOpen = false; });
        const auto profile = _settings.GetProfileForArgs(NewTerminalArgs{});
        const auto appearance = profile.DefaultAppearance();
        const auto currentTheme = _settings.GlobalSettings().CurrentTheme().Name();
        TmuxTheme().Items().Clear();
        for (const auto& theme : _settings.GlobalSettings().Themes())
        {
            const auto name = theme.Key();
            if (std::wstring_view(name).starts_with(L"legacy") && name != currentTheme) { continue; }
            const hstring label = name == L"system" ? hstring{ L"使用 Windows 设置" } :
                                  name == L"light" ? hstring{ L"浅色" } :
                                  name == L"dark" ? hstring{ L"深色" } : name;
            appendCombo(TmuxTheme(), label, name, name == currentTheme);
        }
        TmuxColorScheme().Items().Clear();
        for (const auto& scheme : _settings.GlobalSettings().ColorSchemes())
        {
            appendCombo(TmuxColorScheme(), scheme.Key(), scheme.Key(), scheme.Key() == appearance.DarkColorSchemeName());
        }
        TmuxFontFace().Text(profile.FontInfo().FontFace());
        TmuxFontSize().Value(profile.FontInfo().FontSize());
        TmuxAcrylic().IsOn(appearance.UseAcrylic());
        TmuxOpacity().Value(appearance.Opacity() * 100.0);
        TmuxBackgroundImage().Text(appearance.BackgroundImagePath().Path());
        TmuxPersonalizationError().Text(L"");
        while (co_await _ShowDialogHelper(L"TmuxPersonalizationDialog") == ContentDialogResult::Primary)
        {
            const auto fontSize = TmuxFontSize().Value();
            if (!std::isfinite(fontSize) || fontSize < 8 || fontSize > 40)
            {
                TmuxPersonalizationError().Text(L"字号应为 8 到 40 之间的数字。");
                continue;
            }
            try
            {
                const auto settings = _settings.Copy();
                const auto target = settings.GetProfileForArgs(NewTerminalArgs{});
                const auto targetAppearance = target.DefaultAppearance();
                const auto theme = selectedTag(TmuxTheme());
                if (!theme.empty()) { settings.GlobalSettings().Theme(ThemePair{ theme }); }
                const auto scheme = selectedTag(TmuxColorScheme());
                if (!scheme.empty())
                {
                    targetAppearance.DarkColorSchemeName(scheme);
                    targetAppearance.LightColorSchemeName(scheme);
                }
                const auto font = ::TmuxWorkspace::trimSsh(TmuxFontFace().Text());
                target.FontInfo().FontFace(font.empty() ? L"Cascadia Mono" : font);
                target.FontInfo().FontSize(static_cast<float>(fontSize));
                targetAppearance.UseAcrylic(TmuxAcrylic().IsOn());
                targetAppearance.Opacity(static_cast<float>(TmuxOpacity().Value() / 100.0));
                targetAppearance.BackgroundImagePath(MediaResourceHelper::FromString(TmuxBackgroundImage().Text()));
                settings.ResolveMediaResources();
                if (!settings.WriteSettingsToDisk()) { throw std::runtime_error("Settings could not be saved"); }
                SetSettings(settings, true);
                co_return;
            }
            catch (...) { TmuxPersonalizationError().Text(L"无法保存个性化设置，请检查程序目录是否可写。"); }
        }
    }

    safe_void_coroutine TerminalPage::_TmuxBrowseBackgroundClicked(Windows::Foundation::IInspectable, RoutedEventArgs)
    {
        const auto lifetime = get_strong();
        Windows::Storage::Pickers::FileOpenPicker picker;
        picker.FileTypeFilter().Append(L".png");
        picker.FileTypeFilter().Append(L".jpg");
        picker.FileTypeFilter().Append(L".jpeg");
        picker.FileTypeFilter().Append(L".bmp");
        picker.FileTypeFilter().Append(L".gif");
        if (_hostingHwnd)
        {
            THROW_IF_FAILED(picker.as<::IInitializeWithWindow>()->Initialize(*_hostingHwnd));
        }
        if (const auto file = co_await picker.PickSingleFileAsync())
        {
            TmuxBackgroundImage().Text(file.Path());
        }
    }
}
