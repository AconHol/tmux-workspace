// Licensed under the MIT license.
// Included only by a dedicated TmuxWorkflowTests=true build. Not present in
// the installed release. Runs the real page, SSH transport and TermControl.
#include <fstream>
#include <functional>
#include <sstream>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.UI.Xaml.Automation.Provider.h>

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
        static winrt::Windows::UI::Xaml::FrameworkElement FindVisual(winrt::Windows::UI::Xaml::DependencyObject root, std::wstring_view name)
        {
            using namespace winrt::Windows::UI::Xaml;
            if (const auto element = root.try_as<FrameworkElement>(); element && element.Name() == name) { return element; }
            const auto count = Media::VisualTreeHelper::GetChildrenCount(root);
            for (int i = 0; i < count; ++i)
            {
                if (const auto found = FindVisual(Media::VisualTreeHelper::GetChild(root, i), name)) { return found; }
            }
            return nullptr;
        }
        static winrt::Windows::Foundation::IAsyncAction CaptureJoin(winrt::com_ptr<Page> page, std::wstring path, bool uniform = false)
        {
            using namespace winrt::Windows::UI::Xaml;
            Media::Imaging::RenderTargetBitmap bitmap;
            co_await bitmap.RenderAsync(page->Root());
            const auto buffer = co_await bitmap.GetPixelsAsync();
            std::vector<uint8_t> pixels(buffer.Length());
            winrt::Windows::Storage::Streams::DataReader::FromBuffer(buffer).ReadBytes(pixels);
            const auto sx = bitmap.PixelWidth() / page->Root().ActualWidth();
            const auto sy = bitmap.PixelHeight() / page->Root().ActualHeight();
            const auto origin = page->_tabContent.TransformToVisual(page->Root()).TransformPoint({ 0, 0 });
            const auto left = static_cast<int>((origin.X - 8) * sx);
            const auto top = static_cast<int>(origin.Y * sy);
            const auto width = static_cast<int>(16 * sx);
            const auto height = std::min(bitmap.PixelHeight() - top, static_cast<int>(page->TmuxWorkspaceView().ActualHeight() * sy));
            BITMAPFILEHEADER header{ 0x4d42, static_cast<DWORD>(sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER) + width * height * 4), 0, 0, sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER) };
            BITMAPINFOHEADER info{ sizeof(BITMAPINFOHEADER), width, -height, 1, 32, BI_RGB };
            std::ofstream file{ std::filesystem::path{ path }, std::ios::binary };
            file.write(reinterpret_cast<const char*>(&header), sizeof(header));
            file.write(reinterpret_cast<const char*>(&info), sizeof(info));
            for (int y = top; y < top + height; ++y) { file.write(reinterpret_cast<const char*>(pixels.data() + (y * bitmap.PixelWidth() + left) * 4), width * 4); }
            require(file.good(), "Cannot save the rendered join strip");
            if (uniform)
            {
                // The central half has no sidebar buttons or terminal glyphs.
                // A contrasting magenta root exposes even opaque layout holes.
                for (int y = top + height / 4; y < top + height * 3 / 4; ++y)
                {
                    for (int x = left; x < left + width; ++x)
                    {
                        const auto offset = (y * bitmap.PixelWidth() + x) * 4;
                        require(pixels[offset] == 64 && pixels[offset + 1] == 128 && pixels[offset + 2] == 32 && pixels[offset + 3] == 255,
                                "Rendered sidebar join contains an uncovered or differently painted pixel");
                    }
                }
            }
        }
        static winrt::Windows::Foundation::IAsyncAction ProbeJoin(winrt::com_ptr<Page> page, std::wstring directory)
        {
            using namespace winrt::Windows::UI::Xaml;
            const auto oldSettings = page->_settings.Copy();
            const auto wasMaximized = IsZoomed(*page->_hostingHwnd) != FALSE;
            const auto wasOpen = page->TmuxWorkspaceView().IsPaneOpen();
            ShowWindow(*page->_hostingHwnd, SW_RESTORE);
            co_await pause(page->Dispatcher());
            const auto opaque = oldSettings.Copy();
            const auto appearance = opaque.GetProfileForArgs(winrt::Microsoft::Terminal::Settings::Model::NewTerminalArgs{}).DefaultAppearance();
            appearance.Opacity(1.0f);
            appearance.UseAcrylic(false);
            page->SetSettings(opaque, true);
            co_await pause(page->Dispatcher());
            co_await CaptureJoin(page, directory + L"/opaque-current.bmp");
            const auto root = page->Root();
            const auto side = page->TmuxSidebar();
            const auto terminalRoot = FindVisual(page->_GetActiveControl(), L"RootGrid").as<Controls::Grid>();
            require(terminalRoot != nullptr, "Cannot inspect the actual terminal background");
            const auto rootBrush = root.Background();
            const auto sideBrush = side.Background();
            const auto termBrush = terminalRoot.Background();
            const auto edgeOpacity = page->TmuxSidebarDivider().Opacity();
            auto restore = wil::scope_exit([&]() noexcept {
                root.Background(rootBrush); side.Background(sideBrush); terminalRoot.Background(termBrush);
                page->TmuxSidebarDivider().Opacity(edgeOpacity);
                page->TmuxWorkspaceView().IsPaneOpen(wasOpen);
                page->SetSettings(oldSettings, true);
                ShowWindow(*page->_hostingHwnd, wasMaximized ? SW_MAXIMIZE : SW_RESTORE);
            });
            root.Background(Media::SolidColorBrush{ winrt::Windows::UI::Color{ 255, 255, 0, 255 } });
            const Media::SolidColorBrush fill{ winrt::Windows::UI::Color{ 255, 32, 128, 64 } };
            side.Background(fill);
            terminalRoot.Background(fill);
            page->TmuxSidebarDivider().Opacity(0);
            // Never change the SplitView's border here: this catches a return
            // of its themed 1-DIP transparent border, which clips Pane content
            // before the full OpenPaneLength while Content still starts there.
            for (const auto state : { 0, 1, 2 })
            {
                page->TmuxWorkspaceView().IsPaneOpen(state != 1);
                if (state == 2) { ShowWindow(*page->_hostingHwnd, SW_MAXIMIZE); }
                for (unsigned i = 0; i < 3; ++i) { co_await pause(page->Dispatcher()); }
                co_await CaptureJoin(page, directory + L"/uniform-" + std::to_wstring(state) + L".bmp", true);
            }
        }
        static void VerifySidebarJoin(const winrt::com_ptr<Page>& page)
        {
            const auto workspace = page->TmuxWorkspaceView();
            const auto content = page->_tabContent;
            const auto paneWidth = workspace.IsPaneOpen() ? workspace.OpenPaneLength() : workspace.CompactPaneLength();
            const auto origin = content.TransformToVisual(workspace).TransformPoint({ 0, 0 });
            require(std::abs(origin.X - paneWidth) < 0.01 &&
                    std::abs(origin.X + content.ActualWidth() - workspace.ActualWidth()) < 0.01,
                    "The terminal must directly adjoin the visible sidebar without a layout gap");
            const auto edge = page->TmuxSidebarDivider();
            const auto brush = edge.Background().as<winrt::Windows::UI::Xaml::Media::SolidColorBrush>();
            require(brush.Color().A == 255 && brush.Opacity() == 1.0 && edge.Opacity() == 1.0 &&
                    std::abs(edge.TransformToVisual(workspace).TransformPoint({ 0, 0 }).X - paneWidth) < 0.01 &&
                    edge.ActualHeight() == workspace.ActualHeight(), "Sidebar edge is translucent or does not cover the visible join");
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
        static winrt::Windows::Foundation::IAsyncAction ProbeTranslation(winrt::com_ptr<Page> page, std::wstring endpoint, bool cancel, std::shared_ptr<std::string> failure)
        {
            try
            {
                page->TmuxTranslationEndpoint().Text(endpoint);
                page->TmuxTranslationModel().Text(L"fixture-model");
                page->TmuxTranslationKey().Password(L"local-test-token");
                page->TmuxTranslationLanguage().Text(L"英语");
                page->TmuxTranslationText().Text(L"");
                page->TmuxTranslationText().Focus(winrt::Windows::UI::Xaml::FocusState::Programmatic);
                co_await pause(page->Dispatcher());
                const auto input = GetFocus();
                for (const auto ch : std::wstring_view{ L"请帮我检查这个路径 /tmp/example" }) { PostMessageW(input, WM_CHAR, ch, 1); }
                co_await pause(page->Dispatcher());
                require(page->TmuxInputPrimary().IsEnabled(), "Native translation input did not enable its submit button");
                page->_TmuxTranslateSubmit();
                if (cancel) { page->_tmuxDismissInput(); co_return; }
                for (unsigned i = 0; page->_tmuxTranslationBusy && i < 100; ++i) { co_await pause(page->Dispatcher()); }
                require(!page->_tmuxTranslationBusy, "Translation never completed");
                if (endpoint.find(L"unauthorized") != std::wstring::npos)
                {
                    require(page->TmuxInputLayer().Visibility() == winrt::Windows::UI::Xaml::Visibility::Visible &&
                            std::wstring_view(page->TmuxTranslationStatus().Text()).find(L"401") != std::wstring_view::npos &&
                            !page->TmuxTranslationText().Text().empty() && page->TmuxInputPrimary().IsEnabled(),
                            "API authentication failure did not preserve editable source text and offer retry");
                    page->_tmuxDismissInput();
                }
                else { require(page->TmuxInputLayer().Visibility() == winrt::Windows::UI::Xaml::Visibility::Collapsed, "Successful translation did not return to the terminal"); }
            }
            catch (const std::exception& error) { *failure = error.what(); }
            catch (...) { *failure = "Live translation panel failed"; }
            if (!failure->empty() && page->_tmuxDismissInput) { page->_tmuxDismissInput(); }
        }
        static winrt::Windows::Foundation::IAsyncOperation<winrt::hstring> MouseRemote(::TmuxWorkspace::Server server, std::string command)
        {
            co_await winrt::resume_background();
            const auto result = ::TmuxWorkspace::runSsh(server, command, std::make_shared<std::atomic_bool>(false));
            require(result.exitCode == 0, "Mouse test remote command failed");
            co_return winrt::to_hstring(result.output);
        }

        static winrt::Windows::Foundation::IAsyncAction SetMouseMode(winrt::com_ptr<Page> page, ::TmuxWorkspace::MouseMode mode)
        {
            const auto previous = page->_dialogPresenter.get();
            auto restore = wil::scope_exit([&]() noexcept { page->DialogPresenter(previous); });
            auto settings = winrt::make_self<WorkflowDialogs>();
            settings->prepare = [page, mode](auto) { page->TmuxMouseMode().SelectedIndex(static_cast<int>(mode)); };
            page->DialogPresenter(settings.as<winrt::TerminalApp::IDialogPresenter>());
            page->_TmuxPersonalizationClicked(nullptr, nullptr);
            for (unsigned i = 0; (page->_tmuxDialogOpen || page->_tmuxMouseApplying) && i < 150; ++i) { co_await pause(page->Dispatcher()); }
            require(!page->_tmuxDialogOpen && !page->_tmuxMouseApplying && page->_tmuxMouseMode == mode, "Personalization did not save/apply the requested mouse mode");
            page->_TmuxReloadMouseMode();
            require(page->_tmuxMouseMode == mode, "Mouse preference did not persist");
            for (unsigned i = 0; i < 3; ++i) { co_await pause(page->Dispatcher()); }
        }

        static winrt::Windows::Foundation::IAsyncAction WaitMouseFocus(winrt::com_ptr<Page> page)
        {
            for (unsigned i = 0; !page->_tmuxMouseFocusRequests.empty() && i < 150; ++i) { co_await pause(page->Dispatcher()); }
            require(page->_tmuxMouseFocusRequests.empty(), "Compatibility focus/input buffering did not finish");
            co_await pause(page->Dispatcher());
        }

        static winrt::Windows::Foundation::IAsyncAction TouchGesture(winrt::com_ptr<Page> page, int column, int row, bool swipe = false,
                                                                   std::optional<int> holdToColumn = std::nullopt)
        {
            using namespace winrt;
            const auto control = page->_GetActiveControl();
            const auto panel = FindVisual(control, L"SwapChainPanel");
            if (!panel) { throw hresult_error(E_FAIL, L"No terminal viewport for touch input"); }
            const auto core = get_self<winrt::TerminalApp::implementation::ContentManager>(page->_manager)->TryLookupCore(control.ContentId()).Core();
            const auto font = core.FontSizeInDips();
            const auto hwnd = *page->_hostingHwnd;
            const auto origin = panel.TransformToVisual(nullptr).TransformPoint({ (column + 0.5f) * font.Width, (row + 0.5f) * font.Height });
            const auto scale = GetDpiForWindow(hwnd) / 96.0f;
            POINT location{ static_cast<LONG>(std::lround(origin.X * scale)), static_cast<LONG>(std::lround(origin.Y * scale)) };
            check_bool(ClientToScreen(hwnd, &location));
            const auto device = CreateSyntheticPointerDevice(PT_TOUCH, 1, POINTER_FEEDBACK_NONE);
            if (!device)
            {
                throw hresult_error(HRESULT_FROM_WIN32(GetLastError()), L"CreateSyntheticPointerDevice failed");
            }
            const auto destroyDevice = wil::scope_exit([&]() noexcept { DestroySyntheticPointerDevice(device); });
            auto inject = [&](POINTER_FLAGS flags) {
                if (GetAncestor(WindowFromPoint(location), GA_ROOT) != hwnd)
                {
                    throw hresult_error(E_FAIL, L"Touch coordinates did not hit the test window");
                }
                POINTER_TYPE_INFO pointer{};
                pointer.type = PT_TOUCH;
                auto& contact = pointer.touchInfo;
                contact.pointerInfo.pointerType = PT_TOUCH;
                contact.pointerInfo.pointerFlags = flags;
                const POINT devicePoint{ location.x - GetSystemMetrics(SM_XVIRTUALSCREEN), location.y - GetSystemMetrics(SM_YVIRTUALSCREEN) };
                contact.pointerInfo.ptPixelLocation = devicePoint;
                contact.touchMask = TOUCH_MASK_CONTACTAREA;
                contact.rcContact = { devicePoint.x - 2, devicePoint.y - 2, devicePoint.x + 2, devicePoint.y + 2 };
                if (!InjectSyntheticPointerInput(device, &pointer, 1))
                {
                    throw hresult_error(HRESULT_FROM_WIN32(GetLastError()), hstring{ L"InjectSyntheticPointerInput failed, flags=" + std::to_wstring(flags) });
                }
            };
            inject(POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT | POINTER_FLAG_DOWN);
            if (holdToColumn)
            {
                // Leave the contact stationary long enough for XAML's hold
                // recognizer, keeping the synthetic contact alive meanwhile.
                for (int i = 0; i < 7; ++i)
                {
                    co_await resume_after(std::chrono::milliseconds{ 125 });
                    co_await resume_foreground(page->Dispatcher());
                    inject(POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT | POINTER_FLAG_UPDATE);
                }
                location.x += static_cast<LONG>(std::lround((*holdToColumn - column) * font.Width * scale));
            }
            co_await resume_after(std::chrono::milliseconds{ 50 });
            co_await resume_foreground(page->Dispatcher());
            if (swipe)
            {
                location.y += static_cast<LONG>(std::lround(40 * scale));
            }
            inject(POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT | POINTER_FLAG_UPDATE);
            co_await resume_after(std::chrono::milliseconds{ 50 });
            co_await resume_foreground(page->Dispatcher());
            inject(POINTER_FLAG_UP);
            co_await pause(page->Dispatcher());
            co_await WaitMouseFocus(page);
        }

        static void InvokeTerminalMenuItem(const winrt::Microsoft::Terminal::Control::TermControl& control, std::wstring_view name)
        {
            using namespace winrt;
            using namespace winrt::Windows::UI::Xaml;
            using namespace Automation::Peers;
            for (const auto& popup : Media::VisualTreeHelper::GetOpenPopupsForXamlRoot(control.XamlRoot()))
            {
                if (const auto child = popup.Child())
                {
                    if (const auto item = FindVisual(child, name))
                    {
                        const auto peer = FrameworkElementAutomationPeer::CreatePeerForElement(item);
                        peer.GetPattern(PatternInterface::Invoke).as<Automation::Provider::IInvokeProvider>().Invoke();
                        return;
                    }
                }
            }
            throw hresult_error(E_FAIL, hstring{ L"Missing terminal context action: " + std::wstring{ name } });
        }

        static winrt::Windows::Foundation::IAsyncAction ProbeTouchCopy(winrt::com_ptr<Page> page, ::TmuxWorkspace::Server server, std::string pane, int left, int top)
        {
            using namespace winrt;
            using namespace winrt::Windows::UI::Xaml;
            const auto control = page->_GetActiveControl();
            const auto target = ::TmuxWorkspace::quoteShell(pane);
            const auto command = ::TmuxWorkspace::quoteShell("printf '\\033[2J\\033[2;1Htapcopy second\\n'");
            co_await MouseRemote(server, "tmux send-keys -t " + target + " -l -- " + command + "; tmux send-keys -t " + target + " Enter");
            for (int i = 0; i < 3; ++i) { co_await pause(page->Dispatcher()); }
            hstring copied;
            const auto copyEvent = control.WriteToClipboard(auto_revoke, [&](const auto&, const auto& args) { copied = args.Plain(); });
            co_await TouchGesture(page, left + 1, top + 1, false, left + 10);
            if (!control.HasSelection()) { throw hresult_error(E_FAIL, L"Touch hold and drag did not select text"); }
            InvokeTerminalMenuItem(control, L"TouchCopyItem");
            co_await pause(page->Dispatcher());
            if (copied != L"tapcopy second") { throw hresult_error(E_FAIL, hstring{ L"Touch Copy returned unexpected text: " + std::wstring{ copied } }); }
            if (!to_string(co_await MouseRemote(server, "tmux display-message -p -t " + target + " '#{pane_active}'")).starts_with("1"))
            {
                throw hresult_error(E_FAIL, L"Touch copy changed the active pane");
            }
            if (control.HasSelection()) { throw hresult_error(E_FAIL, L"Copy left a stale text highlight"); }
        }

        static winrt::Windows::Foundation::IAsyncAction ProbePaneCopy(winrt::com_ptr<Page> page, ::TmuxWorkspace::Server server,
                                                                     ::TmuxWorkspace::Session session, std::string pane, std::string other,
                                                                     int column, int row)
        {
            using namespace winrt;
            const auto control = page->_GetActiveControl();
            const auto send = [](const std::string& paneId, const std::string& command) {
                const auto target = ::TmuxWorkspace::quoteShell(paneId);
                return "tmux send-keys -t " + target + " C-u; tmux send-keys -t " + target + " -l -- " +
                    ::TmuxWorkspace::quoteShell(command) + "; tmux send-keys -t " + target + " Enter";
            };
            co_await MouseRemote(server, send(pane, "printf '\\103OPY_DIRECT_PANE_B\\n'") + "; " +
                                        send(other, "printf '\\103OPY_DIRECT_PANE_A\\n'"));
            co_await pause(page->Dispatcher());
            const auto expected = co_await MouseRemote(server, ::TmuxWorkspace::capturePaneAtCommand(session, column, row));
            if (std::wstring_view{ expected }.find(L"COPY_DIRECT_PANE_B") == std::wstring_view::npos)
            {
                throw hresult_error(E_FAIL, L"Remote capture did not find the fixture marker before opening the menu");
            }
            page->TmuxStatus().Text(L"");
            co_await TouchGesture(page, column, row, false, column);
            InvokeTerminalMenuItem(control, L"CopyPaneContextItem");
            for (int i = 0; page->TmuxStatus().Text().empty() && i < 40; ++i) { co_await pause(page->Dispatcher()); }
            const auto result = page->TmuxStatus().Text();
            if (std::wstring_view{ result }.find(L"COPY_DIRECT_PANE_B") == std::wstring_view::npos)
            {
                const auto detail = result.size() < 120 ? std::wstring{ result } : std::wstring{ result }.substr(0, 120);
                throw hresult_error(E_FAIL, hstring{ L"Direct pane copy returned unexpected text: " + detail });
            }
            if (std::wstring_view{ result }.find(L"COPY_DIRECT_PANE_A") != std::wstring_view::npos || control.HasSelection())
            {
                throw hresult_error(E_FAIL, L"Direct pane copy mixed panes or left a highlight");
            }
            if (!to_string(co_await MouseRemote(server, "tmux display-message -p -t " + ::TmuxWorkspace::quoteShell(other) + " '#{pane_active}'")).starts_with("1"))
            {
                throw hresult_error(E_FAIL, L"Copying an inactive pane changed tmux focus");
            }
        }

        static winrt::Windows::Foundation::IAsyncAction ProbeMouseModes(winrt::com_ptr<Page> page, ::TmuxWorkspace::Server server, ::TmuxWorkspace::Session session)
        {
            using namespace winrt;
            using namespace winrt::Microsoft::Terminal::Control;
            using Mode = ::TmuxWorkspace::MouseMode;
            struct Pane { std::string id; int left, top, width, height, active; };
            const auto target = ::TmuxWorkspace::quoteShell(session.id);
            const auto list = "tmux list-panes -t " + target + " -F '#{pane_id} #{pane_left} #{pane_top} #{pane_width} #{pane_height} #{pane_active}'";
            const auto active = "tmux display-message -p -t " + target + " '#{pane_id}'";
            const auto mouse = "tmux show-options -Aqv -t " + target + " mouse";
            const auto snapshot = to_string(co_await MouseRemote(server, list));
            std::istringstream stream{ snapshot };
            std::vector<Pane> panes;
            Pane pane{};
            while (stream >> pane.id >> pane.left >> pane.top >> pane.width >> pane.height >> pane.active) { panes.push_back(pane); }
            require(panes.size() >= 2, "Mouse test requires real split panes");
            const auto a = panes.front();
            const auto b = panes.back();
            const bool testTouch = GetEnvironmentVariableW(L"TMUX_WORKFLOW_TOUCH_INPUT", nullptr, 0) > 0;
            const auto hwnd = *page->_hostingHwnd;
            const auto restoreZOrder = wil::scope_exit([&]() noexcept {
                if (testTouch) { SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE); }
            });
            if (testTouch)
            {
                SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
                co_await pause(page->Dispatcher());
            }
            const auto column = [](const Pane& value) { return value.left + value.width / 2; };
            const auto row = [](const Pane& value) { return value.top + value.height / 2; };
            auto control = page->_GetActiveControl();
            const auto native = get_self<winrt::TerminalApp::implementation::ContentManager>(page->_manager)->TryLookupCore(control.ContentId());
            co_await MouseRemote(server, "tmux select-pane -t " + ::TmuxWorkspace::quoteShell(a.id));
            require(to_string(co_await MouseRemote(server, mouse)).starts_with("on"), "Native attach did not enable session mouse support");
            const auto font = native.Core().FontSize();
            const winrt::Microsoft::Terminal::Core::Point point{ static_cast<int>((column(b) + 0.5f) * font.Width), static_cast<int>((row(b) + 0.5f) * font.Height) };
            native.PointerPressed(MouseButtonState::IsLeftButtonDown, WM_LBUTTONDOWN, GetTickCount64() * 1000, {}, point);
            native.PointerReleased(static_cast<MouseButtonState>(0), WM_LBUTTONUP, {}, point);
            co_await pause(page->Dispatcher());
            require(to_string(co_await MouseRemote(server, active)).starts_with(b.id + "\n"), "Native pointer pipeline did not select the clicked tmux pane");
            // A normal wheel report must reach tmux at the pointed cell.
            // Populate history so the default WheelUpPane action can scroll.
            const auto paneTarget = ::TmuxWorkspace::quoteShell(b.id);
            co_await MouseRemote(server, "tmux send-keys -t " + paneTarget + " -l -- 'seq 1 100'; tmux send-keys -t " + paneTarget + " Enter");
            co_await pause(page->Dispatcher());
            native.MouseWheel({}, { 0, WHEEL_DELTA }, point, static_cast<MouseButtonState>(0));
            co_await pause(page->Dispatcher());
            if (!to_string(co_await MouseRemote(server, "tmux display-message -p -t " + paneTarget + " '#{pane_in_mode}'")).starts_with("1"))
            {
                throw hresult_error(E_FAIL, L"Native wheel did not reach the pointed tmux pane");
            }
            co_await MouseRemote(server, "tmux send-keys -X -t " + paneTarget + " cancel");
            if (testTouch)
            {
                co_await TouchGesture(page, column(a), row(a));
                if (!to_string(co_await MouseRemote(server, active)).starts_with(a.id + "\n"))
                { throw hresult_error(E_FAIL, L"Touch tap did not focus the pane in native mode"); }
                const auto clipboardSequence = GetClipboardSequenceNumber();
                co_await ProbeTouchCopy(page, server, a.id, a.left, a.top);
                co_await ProbePaneCopy(page, server, session, b.id, a.id, column(b), row(b));
                if (GetClipboardSequenceNumber() != clipboardSequence)
                {
                    throw hresult_error(E_FAIL, L"Clipboard changed during the isolated touch copy tests");
                }
            }

            co_await SetMouseMode(page, Mode::Compatibility);
            require(to_string(co_await MouseRemote(server, mouse)).starts_with("off"), "Compatibility mode must work with tmux mouse disabled");
            if (testTouch)
            {
                co_await TouchGesture(page, column(b), row(b));
                if (!to_string(co_await MouseRemote(server, active)).starts_with(b.id + "\n"))
                { throw hresult_error(E_FAIL, L"Touch tap did not focus the pane with tmux mouse off"); }
                co_await TouchGesture(page, column(a), row(a), true);
                if (!to_string(co_await MouseRemote(server, active)).starts_with(b.id + "\n"))
                { throw hresult_error(E_FAIL, L"A touch swipe changed pane focus"); }
            }
            // Type immediately after two fast clicks. The first batch belongs
            // to B and the second to A even while SSH selection is in flight.
            page->_TmuxQueueMouseFocus(control, column(b), row(b));
            control.SendInput(L"\x15printf 'MOUSE_BATCH_B_OK\\n'\r");
            page->_TmuxQueueMouseFocus(control, column(a), row(a));
            control.SendInput(L"\x15printf 'MOUSE_BATCH_A_OK\\n'\r");
            co_await WaitMouseFocus(page);
            require(!page->TmuxMouseProblem().IsOpen(), "Compatibility mouse focus unexpectedly failed");
            const auto aText = to_string(co_await MouseRemote(server, "tmux capture-pane -p -t " + ::TmuxWorkspace::quoteShell(a.id)));
            const auto bText = to_string(co_await MouseRemote(server, "tmux capture-pane -p -t " + ::TmuxWorkspace::quoteShell(b.id)));
            require(aText.find("MOUSE_BATCH_A_OK") != std::string::npos && aText.find("MOUSE_BATCH_B_OK") == std::string::npos &&
                    bText.find("MOUSE_BATCH_B_OK") != std::string::npos && bText.find("MOUSE_BATCH_A_OK") == std::string::npos,
                    "Rapid clicks routed keyboard input into the wrong pane");

            co_await MouseRemote(server, "tmux set-option -t " + target + " status-position top; tmux set-option -t " + target + " status 2");
            std::istringstream topLayout{ to_string(co_await MouseRemote(server, list)) };
            Pane topPane{};
            topLayout >> topPane.id >> topPane.left >> topPane.top >> topPane.width >> topPane.height >> topPane.active;
            page->_TmuxQueueMouseFocus(control, column(topPane), row(topPane) + 2);
            co_await WaitMouseFocus(page);
            require(to_string(co_await MouseRemote(server, active)).starts_with(topPane.id + "\n"), "Top multiline status bar offset selected the wrong pane");
            co_await MouseRemote(server, "tmux resize-pane -Z -t " + ::TmuxWorkspace::quoteShell(topPane.id));
            page->_TmuxQueueMouseFocus(control, 1, 3);
            co_await WaitMouseFocus(page);
            require(to_string(co_await MouseRemote(server, active)).starts_with(topPane.id + "\n"), "Zoomed view selected an invisible pane");
            co_await MouseRemote(server, "tmux resize-pane -Z -t " + ::TmuxWorkspace::quoteShell(topPane.id) + "; tmux set-option -u -t " + target + " status; tmux set-option -u -t " + target + " status-position");

            // Reject an expired context before changing any real tmux pane.
            const auto manager = get_self<winrt::TerminalApp::implementation::ContentManager>(page->_manager);
            const auto context = *manager->GetTmuxContext(control.ContentId());
            auto expired = context;
            expired.session->created += "-expired";
            manager->SetTmuxContext(control.ContentId(), expired);
            page->_TmuxQueueMouseFocus(control, column(a), row(a));
            control.SendInput(L"MOUSE_KEEP_UNSENT");
            co_await WaitMouseFocus(page);
            manager->SetTmuxContext(control.ContentId(), context);
            require(page->TmuxMouseProblem().IsOpen() && page->_tmuxUnsentMouseInput.find(L"MOUSE_KEEP_UNSENT") != std::wstring::npos,
                    "Failed focus lost pending input instead of preserving it for recovery");
            require(to_string(co_await MouseRemote(server, "tmux capture-pane -p -t " + target)).find("MOUSE_KEEP_UNSENT") == std::string::npos,
                    "Failed focus sent pending input to a pane");
            page->_tmuxUnsentMouseInput.clear();
            page->TmuxMouseProblem().IsOpen(false);

            co_await SetMouseMode(page, Mode::Off);
            if (testTouch)
            {
                co_await TouchGesture(page, column(b), row(b));
                if (!to_string(co_await MouseRemote(server, active)).starts_with(topPane.id + "\n"))
                { throw hresult_error(E_FAIL, L"Touch tap changed pane focus in Off mode"); }
            }
            page->_TmuxQueueMouseFocus(control, column(b), row(b));
            require(page->_tmuxMouseFocusRequests.empty(), "Off mode initiated a compatibility focus request");
            co_await SetMouseMode(page, Mode::Compatibility);
            page->_RemoveTab(page->_GetFocusedTab());
            page->_TmuxOpenSession(session);
            for (unsigned i = 0; i < 15; ++i) { co_await pause(page->Dispatcher()); }
            require(to_string(co_await MouseRemote(server, mouse)).starts_with("off"), "Reopened session ignored the saved compatibility mode");
            co_await SetMouseMode(page, Mode::Native);
            require(to_string(co_await MouseRemote(server, mouse)).starts_with("on"), "Switching back to native did not restore tmux mouse support");
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
                if (!page->_tmuxBusy && !page->TmuxProblem().IsOpen())
                {
                    const auto baselineName = "codex-check-base-" + std::to_string(GetTickCount64());
                    co_await resume_background();
                    const auto created = ::TmuxWorkspace::runSsh(*server, ::TmuxWorkspace::createCommand(baselineName), std::make_shared<std::atomic_bool>(false));
                    const auto sessions = created.exitCode == 0 ? ::TmuxWorkspace::parseSessions(created.output) : std::vector<::TmuxWorkspace::Session>{};
                    co_await resume_foreground(dispatcher);
                    require(sessions.size() == 1, "Cannot create an isolated baseline tmux session");
                    baselineSession = sessions.front();
                    page->_TmuxRefresh(true);
                    for (unsigned i = 0; page->_tmuxBusy && i < 100; ++i) { co_await pause(dispatcher); }
                    note("BASELINE: created an isolated session so mouse options and input never modify an existing user session");
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
                require(baselineSession.has_value(), "Isolated baseline session missing");
                const auto first = *baselineSession;
                for (const auto& item : page->TmuxSessionList().Items())
                {
                    const auto sessionRow = item.as<ListViewItem>();
                    if (to_string(unbox_value_or<hstring>(::TmuxWorkspace::clickedRowTag(sessionRow.Content()), hstring{})) == first.key())
                    { page->_TmuxSessionClicked(nullptr, clickArgs(sessionRow.Content())); break; }
                }
                for (unsigned i = 0; i < 15; ++i) { co_await pause(dispatcher); }
                require(page->_tabs.Size() == 1, "First session did not create a terminal tab");
                page->_FocusCurrentTab(false);
                require(page->_GetActiveControl().ConnectionState() == winrt::Microsoft::Terminal::TerminalConnection::ConnectionState::Connected, "SSH terminal disconnected");
                require(page->TmuxTools().IsEnabled() && page->TmuxRailTools().IsEnabled() && page->TmuxRailTranslate().IsEnabled(), "Connected session did not enable the visible shortcut entry points");
                note("FOCUS: original crashing focus path completed in the real terminal tab");
                page->_TmuxOpenSession(first);
                require(page->_tabs.Size() == 1, "Repeated session selection duplicated the tab");
                note("REUSE: selecting the same session returned to its existing tab");

                const auto expandedWidth = page->_tabContent.ActualWidth();
                VerifySidebarJoin(page);
                page->_TmuxToggleSidebarClicked(nullptr, nullptr);
                for (unsigned i = 0; i < 3; ++i) { co_await pause(dispatcher); }
                require(!page->TmuxWorkspaceView().IsPaneOpen() && page->TmuxSidebarRail().Visibility() == Visibility::Visible && page->_tabContent.ActualWidth() >= expandedWidth + 220,
                        "Compact sidebar did not collapse to the native rail");
                VerifySidebarJoin(page);
                page->_TmuxToggleSidebarClicked(nullptr, nullptr);
                for (unsigned i = 0; i < 3; ++i) { co_await pause(dispatcher); }
                require(page->TmuxWorkspaceView().IsPaneOpen() && page->TmuxSidebarBody().Visibility() == Visibility::Visible && page->_tabContent.ActualWidth() <= expandedWidth + 1,
                        "Expanded sidebar did not restore its content");
                VerifySidebarJoin(page);
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
                VerifySidebarJoin(page);
                note("LAYOUT: expanded and compact sidebar edges directly adjoined the terminal; no gap at maximized 20-percent acrylic opacity");
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
                wchar_t joinDirectory[4096]{};
                if (GetEnvironmentVariableW(L"TMUX_WORKFLOW_JOIN_PROBE", joinDirectory, ARRAYSIZE(joinDirectory)))
                {
                    co_await ProbeJoin(page, joinDirectory);
                    note("PIXELS: opaque join rendered without uncovered pixels in expanded, compact and maximized layouts");
                }
                // Send a marker only to the shell in the test-owned session,
                // through SSH/tmux. Never send input to an existing user pane.
                const auto markerTarget = ::TmuxWorkspace::quoteShell(ownedSession->id);
                const auto markerCommand = "tmux send-keys -t " + markerTarget + " C-u; tmux send-keys -t " + markerTarget +
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
                if (!rendered) { log << "OWNED TERMINAL: " << to_string(page->_GetActiveControl().ReadEntireBuffer()).substr(0, 1600) << std::endl; }
                require(rendered, "Actual terminal did not display the output of the remote tmux shell");
                note("CREATE: queued native text and Enter created a real tmux shell and its output reached TermControl");


                wchar_t aiBaseUrl[4096]{};
                if (GetEnvironmentVariableW(L"TMUX_WORKFLOW_AI_URL", aiBaseUrl, ARRAYSIZE(aiBaseUrl)))
                {
                    for (const auto suffix : { L"/unauthorized", L"/slow", L"/v1" })
                    {
                        inputFailure->clear();
                        const auto endpoint = std::wstring{ aiBaseUrl } + suffix;
                        liveDialog->opened = [page, endpoint, inputFailure](ContentDialog) { ProbeTranslation(page, endpoint, endpoint.ends_with(L"/slow"), inputFailure); };
                        page->DialogPresenter(liveDialog.as<winrt::TerminalApp::IDialogPresenter>());
                        page->_TmuxTranslateClicked(nullptr, nullptr);
                        for (unsigned i = 0; page->_tmuxDialogOpen && i < 100; ++i) { co_await pause(dispatcher); }
                        require(inputFailure->empty() && !page->_tmuxDialogOpen, inputFailure->empty() ? "Translation panel did not close" : inputFailure->c_str());
                        require(page->_GetActiveControl().FocusState() != FocusState::Unfocused, "Closing translation did not return keyboard focus to the terminal");
                        if (endpoint.ends_with(L"/slow")) { for (unsigned i = 0; i < 8; ++i) { co_await pause(dispatcher); } }
                    }
                    for (unsigned i = 0; i < 10; ++i) { co_await pause(dispatcher); }
                    const auto text = std::wstring(page->_GetActiveControl().ReadEntireBuffer());
                    require(text.find(L"CODEX_TRANSLATION_MARKER") != std::wstring::npos && text.find(L"CANCEL_SHOULD_NOT_INSERT") == std::wstring::npos,
                            "Translation did not reach its terminal, or a cancelled result was injected");
                    note("AI: native form input called the configured local API; 401 preserved editable text, cancel suppressed input, success filled the original real SSH terminal");
                }

                // All shortcuts act only on our own temporary remote session.
                // Observe tmux's state after the actual page event handler runs.
                std::string originalWindow, originalPane, lastPane;
                bool expectedZoom{};
                const auto query = "tmux display-message -p -t " + ::TmuxWorkspace::quoteShell(ownedSession->id) + " '#{window_id}|#{pane_id}|#{window_panes}|#{window_zoomed_flag}|#{pane_in_mode}'";
                co_await resume_background();
                const auto initialState = ::TmuxWorkspace::runSsh(*server, query, std::make_shared<std::atomic_bool>(false));
                co_await resume_foreground(dispatcher);
                { std::istringstream state{ initialState.output }; std::getline(state, originalWindow, '|'); std::getline(state, originalPane, '|'); lastPane = originalPane; }
                for (const auto action : { L"split-horizontal", L"split-vertical", L"next-pane", L"zoom-pane", L"zoom-pane", L"next-layout", L"new-window", L"previous-window", L"next-window", L"copy-mode" })
                {
                    Button trigger;
                    trigger.Tag(box_value(action));
                    page->_TmuxActionClicked(trigger, nullptr);
                    for (unsigned i = 0; page->_tmuxActionBusy && i < 80; ++i) { co_await pause(dispatcher); }
                    require(!page->_tmuxActionBusy && page->TmuxStatus().Text() == L"已完成 tmux 操作", "Sidebar shortcut failed against real tmux");
                    co_await resume_background();
                    const auto current = ::TmuxWorkspace::runSsh(*server, query, std::make_shared<std::atomic_bool>(false));
                    co_await resume_foreground(dispatcher);
                    std::istringstream state{ current.output };
                    std::string window, pane, panes, zoomed, inMode;
                    std::getline(state, window, '|'); std::getline(state, pane, '|'); std::getline(state, panes, '|'); std::getline(state, zoomed, '|'); std::getline(state, inMode);
                    if (std::wstring_view{ action } == L"split-horizontal") { require(panes == "2", "Horizontal split did not create a remote pane"); }
                    if (std::wstring_view{ action } == L"split-vertical") { require(panes == "3", "Vertical split did not create a remote pane"); }
                    if (std::wstring_view{ action } == L"next-pane") { require(pane != lastPane, "Pane focus did not change"); }
                    if (std::wstring_view{ action } == L"zoom-pane") { expectedZoom = !expectedZoom; require(zoomed == (expectedZoom ? "1" : "0"), "Zoom shortcut did not toggle the remote pane zoom state"); }
                    if (std::wstring_view{ action } == L"new-window" || std::wstring_view{ action } == L"next-window") { require(window != originalWindow, "Window shortcut did not switch the remote window"); }
                    if (std::wstring_view{ action } == L"previous-window") { require(window == originalWindow, "Previous window shortcut selected the wrong window"); }
                    if (std::wstring_view{ action } == L"copy-mode") { require(inMode.starts_with("1"), "History shortcut did not enter tmux copy mode"); }
                    lastPane = pane;
                }
                // These shortcut tests changed the visible tmux window/pane.
                // Restore the original pane and refresh its known marker before
                // the independent reconnect/tear-out buffer-preservation checks.
                const auto restorePane = "tmux send-keys -X -t " + ::TmuxWorkspace::quoteShell(ownedSession->id) + " cancel; tmux select-window -t " + ::TmuxWorkspace::quoteShell(originalWindow) + "; tmux select-pane -t " + ::TmuxWorkspace::quoteShell(originalPane) + "; tmux send-keys -t " + ::TmuxWorkspace::quoteShell(originalPane) + " C-u; " + markerCommand;
                co_await resume_background();
                const auto restored = ::TmuxWorkspace::runSsh(*server, restorePane, std::make_shared<std::atomic_bool>(false));
                co_await resume_foreground(dispatcher);
                require(restored.exitCode == 0, "Cannot restore the original temporary test pane");
                for (unsigned i = 0; i < 5; ++i) { co_await pause(dispatcher); }
                note("TOOLS: sidebar handlers split real tmux panes, changed focus, toggled zoom, switched layouts/windows and entered history mode");
                co_await ProbeMouseModes(page, *server, *ownedSession);
                if (GetEnvironmentVariableW(L"TMUX_WORKFLOW_TOUCH_INPUT", nullptr, 0))
                {
                    note("TOUCH: Windows touch gestures passed in Native, Compatibility and Off; swiping did not switch pane");
                    note("COPY: long press and drag selected terminal output; the visible Copy action produced the exact text");
                    note("PANE COPY: terminal long press copied the pointed inactive pane and cleared selection highlighting");
                }
                co_await MouseRemote(*server, restorePane);
                for (unsigned i = 0; i < 5; ++i) { co_await pause(dispatcher); }
                note("MOUSE: native pointer focus, compatibility with mouse off, rapid-click input routing, top status bars, zoom, failure recovery, saved settings and reconnect passed");
                note("WHEEL: native VT scroll reached the pointed tmux pane");

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
                VerifySidebarJoin(page);
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
                require(std::wstring_view(page->_GetActiveControl().ReadEntireBuffer()).find(L"CODEX_WORKFLOW_OK") != std::wstring_view::npos, "Restored temporary tmux pane did not show the marker before tear-out");

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
