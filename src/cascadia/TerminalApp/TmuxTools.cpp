// Licensed under the MIT license.
#include "pch.h"
#include "TerminalPage.h"
#include "ContentManager.h"
#include "TmuxAi.h"
#include "SshCommandParser.h"
#include "TmuxUiHelpers.h"
#include <winrt/Windows.ApplicationModel.DataTransfer.h>
#include <winrt/Windows.UI.ViewManagement.h>
#include <sstream>

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
        const auto canCopy = enabled && !_tmuxDialogOpen && !_tmuxMouseFocusRequests.contains(control.ContentId());
        TmuxCopyPaneButton().IsEnabled(canCopy);
        TmuxRailCopyPane().IsEnabled(canCopy);
    }

    safe_void_coroutine TerminalPage::_TmuxCopyPaneClicked(IInspectable, RoutedEventArgs)
    {
        const auto control = _GetActiveControl();
        if (_tmuxDialogOpen || _tmuxActionBusy || !control || control.ConnectionState() != ConnectionState::Connected ||
            _tmuxMouseFocusRequests.contains(control.ContentId()))
        {
            co_return;
        }
        const auto context = get_self<ContentManager>(_manager)->GetTmuxContext(control.ContentId());
        if (!context || !context->session) { co_return; }

        const auto lifetime = get_strong();
        const auto cancelled = _tmuxPaneCopyCancellation = std::make_shared<std::atomic_bool>(false);
        _tmuxDialogOpen = true;
        _TmuxUpdateTools();
        auto reset = wil::scope_exit([&]() noexcept {
            cancelled->store(true);
            _tmuxPaneCopyCancellation.reset();
            _tmuxDialogOpen = false;
            try
            {
                TmuxPaneCopyList().Items().Clear();
                _tmuxPaneCopyLines.clear();
                _tmuxPaneCopyStart.reset();
                _tmuxPaneCopyEnd.reset();
                if (_tmuxAlive) { _TmuxUpdateTools(); }
            }
            CATCH_LOG();
        });
        TmuxPaneCopyHint().Text(L"正在读取当前窗格…");
        TmuxPaneCopyProgress().Visibility(Visibility::Visible);
        TmuxPaneCopyClear().IsEnabled(false);
        TmuxPaneCopyDialog().PrimaryButtonText(L"复制全部");
        TmuxPaneCopyDialog().IsPrimaryButtonEnabled(false);
        _TmuxLoadPaneCopy(*context, cancelled);
        co_await _ShowDialogHelper(L"TmuxPaneCopyDialog");
    }

    safe_void_coroutine TerminalPage::_TmuxLoadPaneCopy(::TmuxWorkspace::TabContext context, std::shared_ptr<std::atomic_bool> cancelled)
    {
        const auto lifetime = get_strong();
        const auto dispatcher = Dispatcher();
        std::vector<hstring> lines;
        bool failed{};
        co_await resume_background();
        try
        {
            const auto result = ::TmuxWorkspace::runSsh(context.server, ::TmuxWorkspace::captureActivePaneCommand(*context.session), cancelled);
            failed = result.exitCode != 0;
            if (!failed)
            {
                std::istringstream stream{ result.output };
                std::string line;
                while (std::getline(stream, line))
                {
                    if (!line.empty() && line.back() == '\r') { line.pop_back(); }
                    lines.push_back(to_hstring(line));
                }
                // Drop the blank area below the last line of output. Blank
                // lines between output lines still belong to the snapshot.
                while (!lines.empty() && lines.back().empty()) { lines.pop_back(); }
            }
        }
        catch (...) { failed = true; }
        co_await resume_foreground(dispatcher);
        if (!_tmuxAlive || cancelled->load() || _tmuxPaneCopyCancellation != cancelled) { co_return; }
        TmuxPaneCopyProgress().Visibility(Visibility::Collapsed);
        if (failed || lines.empty())
        {
            TmuxPaneCopyHint().Text(failed ? L"无法读取窗格。请关闭面板，确认连接后重试。" : L"当前窗格没有可复制的文字。");
            co_return;
        }

        _tmuxPaneCopyLines = std::move(lines);
        const auto items = TmuxPaneCopyList().Items();
        for (uint32_t i = 0; i < _tmuxPaneCopyLines.size(); ++i)
        {
            const auto& line = _tmuxPaneCopyLines[i];
            const auto tag = box_value(i);
            TextBlock text;
            text.Text(line);
            text.FontFamily(Media::FontFamily{ L"Cascadia Mono, Consolas" });
            text.TextWrapping(TextWrapping::Wrap);
            text.Tag(tag);
            ListViewItem item;
            item.MinHeight(40);
            item.HorizontalContentAlignment(HorizontalAlignment::Stretch);
            item.Content(text);
            item.Tag(tag);
            Automation::AutomationProperties::SetName(item, L"第 " + std::to_wstring(i + 1) + L" 行 " + std::wstring{ line });
            items.Append(item);
        }
        TmuxPaneCopyDialog().IsPrimaryButtonEnabled(true);
        _TmuxUpdatePaneCopyRange();
        TmuxPaneCopyList().ScrollIntoView(items.GetAt(items.Size() - 1));
    }

    void TerminalPage::_TmuxPaneCopyLineClicked(const IInspectable&, const ItemClickEventArgs& args)
    {
        if (!_tmuxPaneCopyCancellation) { return; }
        const auto index = unbox_value_or<uint32_t>(::TmuxWorkspace::clickedRowTag(args.ClickedItem()), UINT32_MAX);
        if (index >= _tmuxPaneCopyLines.size()) { return; }
        if (!_tmuxPaneCopyStart || _tmuxPaneCopyEnd)
        {
            _tmuxPaneCopyStart = index;
            _tmuxPaneCopyEnd.reset();
        }
        else { _tmuxPaneCopyEnd = index; }
        _TmuxUpdatePaneCopyRange();
    }

    void TerminalPage::_TmuxPaneCopyClearClicked(const IInspectable&, const RoutedEventArgs&)
    {
        _tmuxPaneCopyStart.reset();
        _tmuxPaneCopyEnd.reset();
        _TmuxUpdatePaneCopyRange();
    }

    void TerminalPage::_TmuxUpdatePaneCopyRange()
    {
        const auto selected = _tmuxPaneCopyStart.has_value();
        const auto start = _tmuxPaneCopyStart.value_or(0);
        const auto end = _tmuxPaneCopyEnd.value_or(start);
        const auto [first, last] = std::minmax(start, end);
        const auto items = TmuxPaneCopyList().Items();
        Media::SolidColorBrush highlight{
            Windows::UI::ViewManagement::UISettings().GetColorValue(Windows::UI::ViewManagement::UIColorType::Accent)
        };
        highlight.Opacity(0.26);
        for (uint32_t i = 0; i < items.Size(); ++i)
        {
            items.GetAt(i).as<ListViewItem>().Background(selected && i >= first && i <= last ? highlight : nullptr);
        }
        TmuxPaneCopyClear().IsEnabled(selected);
        TmuxPaneCopyDialog().PrimaryButtonText(selected ? L"复制选中行" : L"复制全部");
        if (!selected) { TmuxPaneCopyHint().Text(L"可复制全部，或点起始行，再点结束行。"); }
        else if (!_tmuxPaneCopyEnd) { TmuxPaneCopyHint().Text(L"已选第 " + std::to_wstring(start + 1) + L" 行。再点一行可以选取范围。"); }
        else { TmuxPaneCopyHint().Text(L"已选第 " + std::to_wstring(first + 1) + L" 至 " + std::to_wstring(last + 1) + L" 行。"); }
    }

    hstring TerminalPage::_TmuxPaneCopyText() const
    {
        if (_tmuxPaneCopyLines.empty()) { return {}; }
        const auto start = _tmuxPaneCopyStart.value_or(0);
        const auto end = _tmuxPaneCopyEnd.value_or(_tmuxPaneCopyStart.value_or(static_cast<uint32_t>(_tmuxPaneCopyLines.size() - 1)));
        const auto [first, last] = std::minmax(start, end);
        std::wstring text;
        for (uint32_t i = first; i <= last; ++i)
        {
            if (i != first) { text += L"\r\n"; }
            text += _tmuxPaneCopyLines[i];
        }
        return hstring{ text };
    }

    bool TerminalPage::_TmuxCommitPaneCopy()
    {
        if (_tmuxPaneCopyLines.empty()) { return false; }
        try
        {
            const auto text = _TmuxPaneCopyText();
#ifdef TMUX_WORKFLOW_TESTS
            if (!GetEnvironmentVariableW(L"TMUX_WORKFLOW_TOUCH_INPUT", nullptr, 0))
#endif
            {
                Windows::ApplicationModel::DataTransfer::DataPackage data;
                data.SetText(text);
                Windows::ApplicationModel::DataTransfer::Clipboard::SetContent(data);
                Windows::ApplicationModel::DataTransfer::Clipboard::Flush();
            }
            TmuxStatus().Text(L"已复制当前窗格的文字");
            return true;
        }
        catch (...)
        {
            TmuxPaneCopyHint().Text(L"剪贴板暂时无法写入，请再点一次复制。");
            return false;
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
