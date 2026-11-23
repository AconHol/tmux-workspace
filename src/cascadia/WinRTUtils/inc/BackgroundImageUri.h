// Licensed under the MIT license.
#pragma once

#include <shlwapi.h>
#include <winrt/Windows.Foundation.h>

namespace Microsoft::Terminal::UI
{
    inline winrt::Windows::Foundation::Uri BackgroundImageUri(const winrt::hstring& path)
    {
        // Canonicalize filesystem paths before creating the WinRT URI so
        // literal '%' or '#' characters are not interpreted as URI syntax.
        wchar_t url[4096]{};
        DWORD length = ARRAYSIZE(url);
        if (UrlCreateFromPathW(path.c_str(), url, &length, 0) == S_OK)
        {
            return winrt::Windows::Foundation::Uri{ url };
        }
        return winrt::Windows::Foundation::Uri{ path };
    }
}
