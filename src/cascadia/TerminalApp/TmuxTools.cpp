// Licensed under the MIT license.
#include "pch.h"
#include "TerminalPage.h"
#include "ContentManager.h"
#include "TmuxAi.h"
#include "SshCommandParser.h"

using namespace winrt;
using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Xaml::Controls;
using namespace winrt::Microsoft::Terminal::TerminalConnection;

namespace
{
    std::filesystem::path aiSettingsPath()
    {
        return std::filesystem::path{ std::wstring_view{ winrt::Microsoft::Terminal::Settings::Model::CascadiaSettings::SettingsPath() } }.parent_path() / L"tmux-ai.json";
    }
}

namespace winrt::TerminalApp::implementation
{
    void TerminalPage::_TmuxUpdateTools()
    {
        const auto control = _GetActiveControl();
        const auto context = control ? get_self<ContentManager>(_manager)->GetTmuxContext(control.ContentId()) : std::nullopt;
        const auto enabled = !_tmuxActionBusy && control && control.ConnectionState() == ConnectionState::Connected && context && context->session;
        TmuxTools().IsEnabled(enabled);
        TmuxRailTools().IsEnabled(enabled);
        TmuxRailTranslate().IsEnabled(enabled);
    }

    safe_void_coroutine TerminalPage::_TmuxCopyPaneAt(winrt::Microsoft::Terminal::Control::TermControl control, int column, int row)
    {
        if (!_tmuxAlive || _tmuxDialogOpen || _GetActiveControl() != control ||
            control.ConnectionState() != ConnectionState::Connected)
        {
            co_return;
        }
        const auto context = get_self<ContentManager>(_manager)->GetTmuxContext(control.ContentId());
        if (!context || !context->session) { co_return; }

        const auto lifetime = get_strong();
        const auto dispatcher = Dispatcher();
        const auto cancellation = _tmuxCancellation;
        const auto connection = control.Connection();
        const auto generation = _tmuxGeneration;
        const auto request = ++_tmuxPaneCopyRequest;
        ::TmuxWorkspace::CommandResult result;
        co_await resume_background();
        try
        {
            result = ::TmuxWorkspace::runSsh(context->server, ::TmuxWorkspace::capturePaneAtCommand(*context->session, column, row), cancellation);
        }
        catch (...) { result.exitCode = 1; }
        co_await resume_foreground(dispatcher);
        if (!_tmuxAlive || generation != _tmuxGeneration || request != _tmuxPaneCopyRequest ||
            _GetActiveControl() != control || control.Connection() != connection)
        {
            co_return;
        }
        if (result.exitCode != 0)
        {
            TmuxStatus().Text(result.exitCode == 2 ? L"请在窗格文字区域内复制。" : L"无法读取窗格文字，请重试。");
            co_return;
        }
        while (!result.output.empty() && (result.output.back() == '\n' || result.output.back() == '\r'))
        {
            result.output.pop_back();
        }
        if (result.output.empty())
        {
            TmuxStatus().Text(L"这个窗格没有可复制的文字。");
            co_return;
        }
        try
        {
            std::string clipboardText;
            clipboardText.reserve(result.output.size());
            for (const auto ch : result.output)
            {
                if (ch == '\n') { clipboardText += '\r'; }
                clipboardText += ch;
            }
            const auto text = to_hstring(clipboardText);
#ifdef TMUX_WORKFLOW_TESTS
            if (GetEnvironmentVariableW(L"TMUX_WORKFLOW_TOUCH_INPUT", nullptr, 0))
            {
                // The isolated workflow checks the exact payload without
                // disturbing the user's system clipboard.
                TmuxStatus().Text(text);
                co_return;
            }
#endif
            Windows::ApplicationModel::DataTransfer::DataPackage data;
            data.SetText(text);
            Windows::ApplicationModel::DataTransfer::Clipboard::SetContent(data);
            Windows::ApplicationModel::DataTransfer::Clipboard::Flush();
            TmuxStatus().Text(L"已复制此窗格的文字");
        }
        catch (...)
        {
            TmuxStatus().Text(L"剪贴板暂时无法写入，请重试。");
        }
    }

    safe_void_coroutine TerminalPage::_TmuxActionClicked(Windows::Foundation::IInspectable sender, RoutedEventArgs)
    {
        if (_tmuxActionBusy || _tmuxDialogOpen) { co_return; }
        const auto control = _GetActiveControl();
        const auto context = control ? get_self<ContentManager>(_manager)->GetTmuxContext(control.ContentId()) : std::nullopt;
        if (!context || !context->session || control.ConnectionState() != ConnectionState::Connected) { co_return; }
        const auto element = sender.try_as<FrameworkElement>();
        const auto action = element ? unbox_value_or<hstring>(element.Tag(), hstring{}) : hstring{};
        std::string command;
        try { command = ::TmuxWorkspace::actionCommand(*context->session, action) + " && " + ::TmuxWorkspace::listCommand(); }
        catch (...) { co_return; }
        const auto lifetime = get_strong();
        const auto dispatcher = Dispatcher();
        const auto generation = _tmuxGeneration;
        const auto cancellation = _tmuxCancellation;
        _tmuxActionBusy = true;
        _TmuxUpdateTools();
        auto reset = wil::scope_exit([&]() noexcept { _tmuxActionBusy = false; try { if (_tmuxAlive) { _TmuxUpdateTools(); } } CATCH_LOG(); });
        ::TmuxWorkspace::CommandResult result;
        co_await resume_background();
        try { result = ::TmuxWorkspace::runSsh(context->server, command, cancellation); }
        catch (...) { result.exitCode = 1; }
        co_await resume_foreground(dispatcher);
        if (!_tmuxAlive || generation != _tmuxGeneration) { co_return; }
        if (result.exitCode == 0)
        {
            _tmuxSessions = ::TmuxWorkspace::parseSessions(result.output);
            _TmuxRenderSessions();
            TmuxStatus().Text(L"已完成 tmux 操作");
        }
        else { TmuxStatus().Text(L"tmux 操作失败，请检查连接或当前窗格大小。"); }
        if (_GetActiveControl() == control) { control.Focus(FocusState::Programmatic); }
    }

    void TerminalPage::_TmuxValidateTranslation()
    {
        if (_tmuxUpdatingForm || !_tmuxDialogOpen) { return; }
        const ::TmuxWorkspace::AiSettings settings{
            ::TmuxWorkspace::trimSsh(TmuxTranslationEndpoint().Text()),
            ::TmuxWorkspace::trimSsh(TmuxTranslationModel().Text()),
            std::wstring(TmuxTranslationKey().Password()),
            ::TmuxWorkspace::trimSsh(TmuxTranslationLanguage().Text())
        };
        const auto error = ::TmuxWorkspace::validateAiSettings(settings);
        TmuxTranslationDialog().IsPrimaryButtonEnabled(!_tmuxTranslationBusy && error.empty() && !::TmuxWorkspace::trimSsh(TmuxTranslationText().Text()).empty());
        if (!_tmuxTranslationBusy) { TmuxTranslationStatus().Text(error.empty() ? L"Ctrl+Enter 翻译并输入。设置会在翻译时保存。" : error); }
    }

    void TerminalPage::_TmuxTranslationChanged(const Windows::Foundation::IInspectable&, const TextChangedEventArgs&) { _TmuxValidateTranslation(); }
    void TerminalPage::_TmuxTranslationKeyChanged(const Windows::Foundation::IInspectable&, const RoutedEventArgs&) { _TmuxValidateTranslation(); }

    safe_void_coroutine TerminalPage::_TmuxTranslateClicked(Windows::Foundation::IInspectable, RoutedEventArgs)
    {
        if (_tmuxDialogOpen) { co_return; }
        const auto control = _GetActiveControl();
        if (!control || control.ConnectionState() != ConnectionState::Connected) { co_return; }
        const auto lifetime = get_strong();
        _tmuxDialogOpen = true;
        _tmuxTranslationTarget = make_weak(control);
        _tmuxTranslationBusy = false;
        auto reset = wil::scope_exit([&]() noexcept {
            if (_tmuxTranslationCancellation) { _tmuxTranslationCancellation->store(true); }
            _tmuxTranslationBusy = false;
            _tmuxTranslationTarget = {};
            _tmuxDialogOpen = false;
            try
            {
                TmuxTranslationKey().Password(L"");
                if (_tmuxAlive && _GetActiveControl() == control) { control.Focus(FocusState::Programmatic); }
            }
            CATCH_LOG();
        });
        _tmuxUpdatingForm = true;
        auto formReset = wil::scope_exit([&]() noexcept { _tmuxUpdatingForm = false; });
        ::TmuxWorkspace::AiSettings settings;
        try { settings = ::TmuxWorkspace::loadAiSettings(aiSettingsPath()); }
        catch (...) { /* A moved/corrupt encrypted key can be re-entered. */ }
        const auto dialog = TmuxTranslationDialog();
        TmuxTranslationEndpoint().Text(settings.endpoint);
        TmuxTranslationModel().Text(settings.model);
        TmuxTranslationLanguage().Text(settings.language);
        TmuxTranslationKey().Password(settings.key);
        TmuxTranslationApi().IsExpanded(settings.endpoint.empty() || settings.model.empty());
        TmuxTranslationProgress().Visibility(Visibility::Collapsed);
        TmuxTranslationText().IsEnabled(true);
        TmuxTranslationLanguage().IsEnabled(true);
        TmuxTranslationApi().IsEnabled(true);
        _tmuxUpdatingForm = false;
        formReset.release();
        _TmuxValidateTranslation();
        if (const auto presenter = _dialogPresenter.get()) { co_await presenter.ShowDialog(dialog); }
    }

    safe_void_coroutine TerminalPage::_TmuxTranslateSubmit()
    {
        if (_tmuxTranslationBusy || !_tmuxDialogOpen || !_tmuxDismissInput) { co_return; }
        _TmuxValidateTranslation();
        if (!TmuxTranslationDialog().IsPrimaryButtonEnabled()) { co_return; }
        const auto target = _tmuxTranslationTarget.get();
        if (!target || target.ConnectionState() != ConnectionState::Connected) { TmuxTranslationStatus().Text(L"原终端连接已关闭，请重新连接后翻译。"); co_return; }
        const auto originalConnection = target.Connection();
        const auto lifetime = get_strong();
        const auto dispatcher = Dispatcher();
        const ::TmuxWorkspace::AiSettings settings{
            ::TmuxWorkspace::trimSsh(TmuxTranslationEndpoint().Text()), ::TmuxWorkspace::trimSsh(TmuxTranslationModel().Text()),
            std::wstring(TmuxTranslationKey().Password()), ::TmuxWorkspace::trimSsh(TmuxTranslationLanguage().Text())
        };
        const auto text = std::wstring(TmuxTranslationText().Text());
        try { ::TmuxWorkspace::saveAiSettings(aiSettingsPath(), settings); }
        catch (...) { TmuxTranslationStatus().Text(L"无法保存 API 设置，请检查应用配置目录是否可写。"); co_return; }
        _tmuxTranslationBusy = true;
        const auto cancelled = _tmuxTranslationCancellation = std::make_shared<std::atomic_bool>(false);
        TmuxTranslationDialog().IsPrimaryButtonEnabled(false);
        TmuxTranslationProgress().Visibility(Visibility::Visible);
        TmuxTranslationText().IsEnabled(false);
        TmuxTranslationLanguage().IsEnabled(false);
        TmuxTranslationApi().IsEnabled(false);
        TmuxTranslationStatus().Text(L"正在翻译…");
        std::wstring translated;
        std::string error;
        co_await resume_background();
        try { translated = ::TmuxWorkspace::translate(settings, text, cancelled); }
        catch (const std::exception& failure) { error = failure.what(); }
        catch (...) { error = "API request failed"; }
        co_await resume_foreground(dispatcher);
        if (cancelled->load() || !_tmuxAlive || cancelled != _tmuxTranslationCancellation) { co_return; }
        _tmuxTranslationBusy = false;
        TmuxTranslationProgress().Visibility(Visibility::Collapsed);
        TmuxTranslationText().IsEnabled(true);
        TmuxTranslationLanguage().IsEnabled(true);
        TmuxTranslationApi().IsEnabled(true);
        _TmuxValidateTranslation();
        if (!error.empty())
        {
            TmuxTranslationStatus().Text(error.starts_with("HTTP ") ? L"API 返回 " + to_hstring(error) + L"，请检查密钥、模型和服务额度。" :
                L"翻译失败，请检查 API 地址、模型或网络，稍后重试。");
            co_return;
        }
        // Recheck the original terminal after the network wait. Never redirect
        // an asynchronous result into a different tab or reconnected shell.
        if (_tmuxTranslationTarget.get() != target || target.Connection() != originalConnection || target.ConnectionState() != ConnectionState::Connected || _GetActiveControl() != target)
        { TmuxTranslationStatus().Text(L"原终端已变化，请关闭面板后重新选择终端。"); co_return; }
        target.SendInput(translated);
        TmuxTranslationText().Text(L"");
        TmuxStatus().Text(L"译文已填入终端，按 Enter 发送。");
        if (_tmuxDismissInput) { _tmuxDismissInput(); }
    }
}
