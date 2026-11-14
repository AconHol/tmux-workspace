// Licensed under the MIT license.
#include <windows.h>
#undef GetCurrentTime
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.UI.h>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Hosting.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include <winrt/Microsoft.Terminal.Settings.Model.h>
#include <iostream>
#include <stdexcept>
#include "../TerminalApp/TmuxUiHelpers.h"

using namespace winrt;
using namespace Windows::UI::Xaml;
using namespace Windows::UI::Xaml::Controls;

int main()
try
{
    init_apartment(apartment_type::single_threaded);
    const auto xaml = Hosting::WindowsXamlManager::InitializeForCurrentThread();
    unsigned checks{};
    const auto check = [&](bool passed, const char* message) {
        if (!passed) { throw std::runtime_error(message); }
        ++checks;
    };

    ListViewItem serverRow;
    Grid serverContent;
    serverRow.Tag(box_value(uint32_t{ 2 }));
    serverContent.Tag(serverRow.Tag());
    serverRow.Content(serverContent);
    check(!serverContent.try_as<ListViewItem>(), "Content is not a ListViewItem");
    check(unbox_value<uint32_t>(TmuxWorkspace::clickedRowTag(serverRow)) == 2, "Server container click");
    check(unbox_value<uint32_t>(TmuxWorkspace::clickedRowTag(serverContent)) == 2, "Server content click");

    ListViewItem sessionRow;
    StackPanel sessionContent;
    sessionRow.Tag(box_value(hstring{ L"123/$1/456" }));
    sessionContent.Tag(sessionRow.Tag());
    sessionRow.Content(sessionContent);
    check(unbox_value<hstring>(TmuxWorkspace::clickedRowTag(sessionRow)) == L"123/$1/456", "Session container click");
    check(unbox_value<hstring>(TmuxWorkspace::clickedRowTag(sessionContent)) == L"123/$1/456", "Session content click");
    check(!TmuxWorkspace::clickedRowTag(nullptr), "Null click is harmless");
    check(!TmuxWorkspace::clickedRowTag(box_value(L"unexpected")), "Unexpected click type is harmless");
    check(unbox_value_or<uint32_t>(TmuxWorkspace::clickedRowTag(Grid{}), UINT32_MAX) == UINT32_MAX, "Missing identifier cannot select the first server");

    const auto color = Windows::UI::Color{ 255, 32, 32, 32 };
    const auto solid = TmuxWorkspace::workspaceBrush(color, 0.4f, false, false).as<Media::SolidColorBrush>();
    check(solid.Opacity() > 0.39 && solid.Opacity() < 0.41, "Solid background retains requested opacity");
    check(solid.Color() == color, "Solid background retains theme color");
    const auto acrylic = TmuxWorkspace::workspaceBrush(color, 0.6f, true, false).as<Media::AcrylicBrush>();
    check(acrylic.TintOpacity() > 0.59 && acrylic.TintOpacity() < 0.61, "Acrylic retains requested opacity");
    check(acrylic.BackgroundSource() == Media::AcrylicBackgroundSource::HostBackdrop, "Use the desktop host backdrop");
    const auto unfocused = TmuxWorkspace::workspaceBrush(color, 0.6f, true, true).as<Media::AcrylicBrush>();
    check(unfocused.BackgroundSource() == Media::AcrylicBackgroundSource::Backdrop, "Honor unfocused acrylic setting");
    check(static_cast<bool>(TmuxWorkspace::workspaceBrush(color, 1.0f, true, false).try_as<Media::SolidColorBrush>()), "100 percent remains solid");

    // Exercise the real settings model: command lines without an explicit
    // profile fall back to profile defaults rather than the user's default.
    using namespace Microsoft::Terminal::Settings::Model;
    const CascadiaSettings settings{
        LR"({"defaultProfile":"{11111111-1111-1111-1111-111111111111}","profiles":{"defaults":{"opacity":100},"list":[{"guid":"{11111111-1111-1111-1111-111111111111}","name":"Workspace","commandline":"powershell.exe","opacity":40,"useAcrylic":true}]}})",
        L"{}"
    };
    NewTerminalArgs ssh;
    ssh.Commandline(L"ssh.exe example.invalid");
    check(settings.GetProfileForArgs(ssh).DefaultAppearance().Opacity() == 1.0f, "Unpinned SSH demonstrates the original profile mismatch");
    ssh.Profile(to_hstring(settings.GlobalSettings().DefaultProfile()));
    const auto selected = settings.GetProfileForArgs(ssh);
    check(selected.Guid() == settings.GlobalSettings().DefaultProfile(), "SSH uses the personalized profile");
    check(selected.DefaultAppearance().Opacity() > 0.39f && selected.DefaultAppearance().Opacity() < 0.41f, "SSH inherits the saved opacity");
    check(selected.DefaultAppearance().UseAcrylic(), "SSH inherits the saved acrylic choice");
    const auto updated = settings.Copy();
    updated.GetProfileForArgs(NewTerminalArgs{}).DefaultAppearance().Opacity(0.65f);
    const auto changedOpacity = updated.GetProfileForArgs(ssh).DefaultAppearance().Opacity();
    check(changedOpacity > 0.64f && changedOpacity < 0.66f, "Personalization updates the profile used by SSH");
    check(ssh.Commandline() == L"ssh.exe example.invalid", "Appearance selection preserves the SSH command");
    std::cout << "PASS: " << checks << " native XAML regression checks\n";
    return 0;
}
catch (const winrt::hresult_error& error)
{
    std::cerr << "FAIL HRESULT: " << std::hex << static_cast<uint32_t>(error.code()) << " " << winrt::to_string(error.message()) << "\n";
    return 1;
}
catch (const std::exception& error)
{
    std::cerr << "FAIL: " << error.what() << "\n";
    return 1;
}
