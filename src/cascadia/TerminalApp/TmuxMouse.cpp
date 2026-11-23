// Licensed under the MIT license.
#include "pch.h"
#include "TerminalPage.h"
#include "ContentManager.h"
#include <til/io.h>
#include <json/json.h>
#include <deque>
#include <cmath>

using namespace winrt;
using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Xaml::Controls;
using namespace winrt::Microsoft::Terminal::Control;
using namespace winrt::Microsoft::Terminal::TerminalConnection;

namespace TmuxWorkspace
{
    struct MouseFocusState
    {
        struct Click { int column; int row; std::wstring input; };
        uint64_t id{};
        uint64_t generation{};
        uint64_t settingsVersion{};
        winrt::weak_ref<TermControl> control;
        ControlCore core{ nullptr };
        ITerminalConnection connection{ nullptr };
        TabContext context;
        std::deque<Click> clicks;
    };
}

namespace
{
    std::filesystem::path mouseSettingsPath()
    {
        return std::filesystem::path{ std::wstring_view{ winrt::Microsoft::Terminal::Settings::Model::CascadiaSettings::SettingsPath() } }.parent_path() / L"tmux-workspace.json";
    }

    FrameworkElement viewport(DependencyObject root)
    {
        if (const auto element = root.try_as<FrameworkElement>(); element && element.Name() == L"SwapChainPanel") { return element; }
        const auto count = Media::VisualTreeHelper::GetChildrenCount(root);
        for (int index = 0; index < count; ++index)
        {
            if (const auto found = viewport(Media::VisualTreeHelper::GetChild(root, index))) { return found; }
        }
        return nullptr;
    }
}

namespace winrt::TerminalApp::implementation
{
    std::wstring TerminalPage::_TmuxAttachCommandline(const ::TmuxWorkspace::Server& server, const ::TmuxWorkspace::Session& session) const
    {
        return ::TmuxWorkspace::quoteArgument(::TmuxWorkspace::sshExecutable()) + ::TmuxWorkspace::sshArguments(server, true) + L" " +
            ::TmuxWorkspace::quoteArgument(to_hstring("sh -c " + ::TmuxWorkspace::quoteShell(::TmuxWorkspace::attachCommand(session, _tmuxMouseMode))));
    }

    void TerminalPage::_TmuxReloadMouseMode()
    {
        auto mode = ::TmuxWorkspace::MouseMode::Native;
        try
        {
            const auto data = til::io::read_file_as_utf8_string_if_exists(mouseSettingsPath());
            if (!data.empty())
            {
                Json::Value root;
                Json::CharReaderBuilder builder;
                std::string errors;
                const std::unique_ptr<Json::CharReader> reader{ builder.newCharReader() };
                if (!reader->parse(data.data(), data.data() + data.size(), &root, &errors)) { return; }
                const auto value = root.get("paneMouseMode", "native").asString();
                mode = value == "compatibility" ? ::TmuxWorkspace::MouseMode::Compatibility :
                       value == "off" ? ::TmuxWorkspace::MouseMode::Off : ::TmuxWorkspace::MouseMode::Native;
            }
        }
        catch (...) { return; }
        if (mode != _tmuxMouseMode)
        {
            _tmuxMouseMode = mode;
            ++_tmuxMouseSettingsVersion;
            _TmuxApplyMouseMode();
        }
    }

    void TerminalPage::_TmuxSaveMouseMode(::TmuxWorkspace::MouseMode mode)
    {
        Json::Value root;
        root["paneMouseMode"] = mode == ::TmuxWorkspace::MouseMode::Compatibility ? "compatibility" : mode == ::TmuxWorkspace::MouseMode::Off ? "off" : "native";
        std::filesystem::create_directories(mouseSettingsPath().parent_path());
        til::io::write_utf8_string_to_file_atomic(mouseSettingsPath(), Json::writeString(Json::StreamWriterBuilder{}, root));
    }

    safe_void_coroutine TerminalPage::_TmuxApplyMouseMode()
    {
        if (_tmuxMouseApplying || !_tmuxAlive) { co_return; }
        const auto lifetime = get_strong();
        const auto dispatcher = Dispatcher();
        _tmuxMouseApplying = true;
        auto reset = wil::scope_exit([&]() noexcept { _tmuxMouseApplying = false; });
        uint64_t version;
        do
        {
            version = _tmuxMouseSettingsVersion;
            const auto mode = _tmuxMouseMode;
            const auto generation = _tmuxGeneration;
            const auto cancellation = _tmuxCancellation;
            std::vector<::TmuxWorkspace::TabContext> contexts;
            for (const auto& tab : _tabs)
            {
                if (const auto control = _GetTabImpl(tab)->GetActiveTerminalControl())
                {
                    if (const auto context = get_self<ContentManager>(_manager)->GetTmuxContext(control.ContentId()); context && context->session) { contexts.push_back(*context); }
                }
            }
            bool failed{};
            for (const auto& context : contexts)
            {
                ::TmuxWorkspace::CommandResult result;
                co_await resume_background();
                try { result = ::TmuxWorkspace::runSsh(context.server, ::TmuxWorkspace::mouseCommand(*context.session, mode), cancellation); }
                catch (...) { result.exitCode = 1; }
                co_await resume_foreground(dispatcher);
                if (!_tmuxAlive || generation != _tmuxGeneration) { co_return; }
                failed = failed || result.exitCode != 0;
                if (version != _tmuxMouseSettingsVersion) { break; }
            }
            if (failed && version == _tmuxMouseSettingsVersion) { TmuxStatus().Text(L"部分会话的鼠标设置未应用，请关闭对应标签页后重新打开重试。"); }
        } while (_tmuxAlive && version != _tmuxMouseSettingsVersion);
    }

    void TerminalPage::_TmuxBindMouse(const TermControl& control)
    {
        const auto id = control.ContentId();
        if (const auto found = _tmuxMouseHandlers.find(id); found != _tmuxMouseHandlers.end() && found->second.get() == control) { return; }
        _tmuxMouseHandlers.insert_or_assign(id, make_weak(control));
        control.TerminalPointerPressed([weak = get_weak()](const IInspectable& sender, const Input::PointerRoutedEventArgs& args) {
            if (const auto page = weak.get(); page && page->_tmuxAlive)
            {
                if (const auto clicked = sender.try_as<TermControl>()) { page->_TmuxMousePointer(clicked, args); }
            }
        });
        // TermControl handles Tapped itself to show the touch keyboard.
        const Input::TappedEventHandler onTap{ [weak = get_weak()](const IInspectable& sender, const Input::TappedRoutedEventArgs& args) {
            if (const auto page = weak.get(); page && page->_tmuxAlive)
            {
                if (const auto tapped = sender.try_as<TermControl>()) { page->_TmuxTouchTap(tapped, args); }
            }
        } };
        control.AddHandler(UIElement::TappedEvent(), box_value(onTap), true);
    }

    void TerminalPage::_TmuxMousePointer(const TermControl& control, const Input::PointerRoutedEventArgs& args)
    {
        if (_tmuxMouseMode != ::TmuxWorkspace::MouseMode::Compatibility || _tmuxDialogOpen ||
            args.Pointer().PointerDeviceType() == Windows::Devices::Input::PointerDeviceType::Touch ||
            args.KeyModifiers() != Windows::System::VirtualKeyModifiers::None) { return; }
        const auto panel = viewport(control);
        if (!panel) { return; }
        const auto point = args.GetCurrentPoint(panel);
        if (!point.Properties().IsLeftButtonPressed()) { return; }
        _TmuxFocusAtPoint(control, panel, point.Position());
    }

    void TerminalPage::_TmuxTouchTap(const TermControl& control, const Input::TappedRoutedEventArgs& args)
    {
        if (_tmuxMouseMode == ::TmuxWorkspace::MouseMode::Off || _tmuxDialogOpen ||
            args.PointerDeviceType() != Windows::Devices::Input::PointerDeviceType::Touch) { return; }
        const auto panel = viewport(control);
        if (!panel) { return; }

        // A tap anywhere else in the control (e.g. search or the scrollbar)
        // must not select a pane. The transparent input grid can be the source
        // when the rendered terminal itself has no XAML hit-test surface.
        const auto inputGrid = panel.Parent();
        auto source = args.OriginalSource().try_as<DependencyObject>();
        while (source && source != panel && source != inputGrid)
        {
            if (source.try_as<Controls::Control>()) { return; }
            source = Media::VisualTreeHelper::GetParent(source);
        }
        if (source) { _TmuxFocusAtPoint(control, panel, args.GetPosition(panel)); }
    }

    void TerminalPage::_TmuxFocusAtPoint(const TermControl& control, const FrameworkElement& panel, Windows::Foundation::Point position)
    {
        if (position.X < 0 || position.Y < 0 || position.X >= panel.ActualWidth() || position.Y >= panel.ActualHeight() ||
            control.ScrollOffset() < control.BufferHeight() - control.ViewHeight()) { return; }
        const auto content = get_self<ContentManager>(_manager)->TryLookupCore(control.ContentId());
        if (!content) { return; }
        const auto font = content.Core().FontSizeInDips();
        if (font.Width <= 0 || font.Height <= 0) { return; }
        control.Focus(FocusState::Pointer);
        _TmuxQueueMouseFocus(control, static_cast<int>(std::floor(position.X / font.Width)), static_cast<int>(std::floor(position.Y / font.Height)));
    }

    void TerminalPage::_TmuxQueueMouseFocus(const TermControl& control, int column, int row)
    {
        if (_tmuxMouseMode == ::TmuxWorkspace::MouseMode::Off || !_tmuxAlive || _tmuxDialogOpen ||
            column < 0 || row < 0 || column > 32766 || row > 32766 || control.ConnectionState() != ConnectionState::Connected) { return; }
        const auto manager = get_self<ContentManager>(_manager);
        const auto context = manager->GetTmuxContext(control.ContentId());
        const auto content = manager->TryLookupCore(control.ContentId());
        if (!context || !context->session || !content) { return; }
        const auto id = control.ContentId();
        if (const auto found = _tmuxMouseFocusRequests.find(id); found != _tmuxMouseFocusRequests.end())
        {
            const auto& state = found->second;
            if (state->clicks.back().column == column && state->clicks.back().row == row) { return; }
            // Seal keys typed before this newer click, preserving their target.
            state->clicks.back().input += state->core.EndInputBuffering(false);
            state->core.BeginInputBuffering();
            state->clicks.push_back({ column, row, {} });
            return;
        }
        auto state = std::make_shared<::TmuxWorkspace::MouseFocusState>();
        state->id = id;
        state->generation = _tmuxGeneration;
        state->settingsVersion = _tmuxMouseSettingsVersion;
        state->control = make_weak(control);
        state->core = content.Core();
        state->connection = control.Connection();
        state->context = *context;
        state->clicks.push_back({ column, row, {} });
        state->core.BeginInputBuffering();
        _tmuxMouseFocusRequests.emplace(id, state);
        _TmuxUpdateTools();
        _TmuxRunMouseFocus(std::move(state));
    }

    safe_void_coroutine TerminalPage::_TmuxRunMouseFocus(std::shared_ptr<::TmuxWorkspace::MouseFocusState> state)
    {
        const auto lifetime = get_strong();
        const auto dispatcher = Dispatcher();
        const auto cancellation = _tmuxCancellation;
        auto release = wil::scope_exit([&]() noexcept { try { state->core.EndInputBuffering(false); } catch (...) {} });
        std::wstring retained;
        bool failed{};
        while (!state->clicks.empty())
        {
            const auto column = state->clicks.front().column;
            const auto row = state->clicks.front().row;
            ::TmuxWorkspace::CommandResult result;
            co_await resume_background();
            try { result = ::TmuxWorkspace::runSsh(state->context.server, ::TmuxWorkspace::focusPaneCommand(*state->context.session, column, row), cancellation); }
            catch (...) { result.exitCode = 1; }
            co_await resume_foreground(dispatcher);
            const auto control = state->control.get();
            if (!_tmuxAlive) { co_return; }
            if (state->generation != _tmuxGeneration || state->settingsVersion != _tmuxMouseSettingsVersion ||
                !control || control.Connection() != state->connection || control.ConnectionState() != ConnectionState::Connected ||
                (result.exitCode != 0 && result.exitCode != 2)) { failed = true; break; }
            if (state->clicks.size() == 1)
            {
                retained = state->core.EndInputBuffering(true);
                state->clicks.pop_front();
                break;
            }
            // Newer clicks have their own still-buffered keys. Deliver only
            // this completed click's sealed batch to its now-selected pane.
            const auto& input = state->clicks.front().input;
            try { if (!input.empty()) { state->connection.WriteInput(winrt_wstring_to_array_view(input)); } }
            catch (...) { failed = true; break; }
            state->clicks.pop_front();
        }
        if (failed)
        {
            for (const auto& click : state->clicks) { retained += click.input; }
            retained += state->core.EndInputBuffering(false);
            TmuxStatus().Text(L"窗格焦点切换失败，请重新点击目标窗格重试。");
        }
        if (!retained.empty())
        {
            _tmuxUnsentMouseInput += retained;
            TmuxMouseProblem().IsOpen(true);
        }
        _tmuxMouseFocusRequests.erase(state->id);
        _TmuxUpdateTools();
    }

    void TerminalPage::_TmuxCopyMouseInputClicked(const IInspectable&, const RoutedEventArgs&)
    {
        if (_tmuxUnsentMouseInput.empty()) { return; }
        Windows::ApplicationModel::DataTransfer::DataPackage data;
        data.SetText(_tmuxUnsentMouseInput);
        Windows::ApplicationModel::DataTransfer::Clipboard::SetContent(data);
        TmuxStatus().Text(L"暂存输入已复制，可粘贴到需要的窗格。");
    }
}
