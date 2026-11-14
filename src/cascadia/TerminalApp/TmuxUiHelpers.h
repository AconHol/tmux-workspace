// Licensed under the MIT license.
#pragma once

namespace TmuxWorkspace
{
    inline winrt::Windows::UI::Xaml::Media::Brush workspaceBrush(
        winrt::Windows::UI::Color color, float opacity, bool acrylic, bool unfocusedAcrylic)
    {
        namespace Media = winrt::Windows::UI::Xaml::Media;
        if (acrylic && opacity < 1.0f)
        {
            Media::AcrylicBrush brush;
            brush.BackgroundSource(unfocusedAcrylic ? Media::AcrylicBackgroundSource::Backdrop : Media::AcrylicBackgroundSource::HostBackdrop);
            brush.TintColor(color);
            brush.FallbackColor(color);
            brush.TintOpacity(opacity);
            return brush;
        }
        Media::SolidColorBrush brush{ color };
        brush.Opacity(opacity);
        return brush;
    }

    // ItemClick may supply the content of an explicit ListViewItem instead
    // of its container. Keep the same stable identifier on both objects.
    inline winrt::Windows::Foundation::IInspectable clickedRowTag(
        const winrt::Windows::Foundation::IInspectable& item)
    {
        if (const auto element = item.try_as<winrt::Windows::UI::Xaml::FrameworkElement>())
        {
            return element.Tag();
        }
        return nullptr;
    }
}
