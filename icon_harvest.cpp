#include "icon_harvest.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <unordered_set>

#ifdef _WIN32
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <shlobj.h>
#  include <shobjidl.h>
#  include <shellapi.h>

namespace {

void listShortcuts(const std::wstring& dir, int depth, std::vector<std::wstring>& out) {
    if (depth > 3 || out.size() > 800) return;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..") continue;
        std::wstring full = dir + L"\\" + name;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            listShortcuts(full, depth + 1, out);
        } else {
            std::wstring lower = name;
            std::transform(lower.begin(), lower.end(), lower.begin(), ::towlower);
            if (lower.size() > 4 && lower.compare(lower.size() - 4, 4, L".lnk") == 0 &&
                lower.find(L"uninstall") == std::wstring::npos &&
                lower.find(L"readme") == std::wstring::npos)
                out.push_back(full);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

std::wstring knownFolder(int csidl) {
    wchar_t buf[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathW(nullptr, csidl, nullptr, SHGFP_TYPE_CURRENT, buf))) return buf;
    return L"";
}

// Fetches the shell icon of a file as straight-alpha BGRA pixels, size x size.
bool getIconPixels(const std::wstring& path, int size, std::vector<unsigned char>& bgra) {
    IShellItemImageFactory* factory = nullptr;
    if (FAILED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&factory)))) return false;
    HBITMAP bmp = nullptr;
    SIZE sz{ size, size };
    HRESULT hr = factory->GetImage(sz, SIIGBF_ICONONLY | SIIGBF_BIGGERSIZEOK, &bmp);
    factory->Release();
    if (FAILED(hr) || !bmp) return false;

    BITMAP bm{};
    GetObjectW(bmp, sizeof(bm), &bm);
    if (bm.bmWidth <= 0 || bm.bmHeight <= 0) { DeleteObject(bmp); return false; }
    const int bw = int(bm.bmWidth), bh = int(bm.bmHeight);

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = bm.bmWidth;
    bi.bmiHeader.biHeight = -bm.bmHeight;        // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    std::vector<unsigned char> raw(size_t(bm.bmWidth) * size_t(bm.bmHeight) * 4);
    HDC dc = GetDC(nullptr);
    int lines = GetDIBits(dc, bmp, 0, bm.bmHeight, raw.data(), &bi, DIB_RGB_COLORS);
    ReleaseDC(nullptr, dc);
    DeleteObject(bmp);
    if (lines == 0) return false;

    // Reject bitmaps without an alpha channel (would render as opaque squares).
    unsigned maxA = 0;
    for (size_t i = 3; i < raw.size(); i += 4) maxA = std::max<unsigned>(maxA, raw[i]);
    if (maxA == 0) return false;

    // Box-resample to size x size, un-premultiplying alpha on the way.
    bgra.assign(size_t(size) * size_t(size) * 4, 0);
    for (int y = 0; y < size; ++y) {
        int sy0 = y * bh / size, sy1 = std::max(sy0 + 1, (y + 1) * bh / size);
        for (int x = 0; x < size; ++x) {
            int sx0 = x * bw / size, sx1 = std::max(sx0 + 1, (x + 1) * bw / size);
            unsigned sum[4] = { 0, 0, 0, 0 }, n = 0;
            for (int sy = sy0; sy < sy1; ++sy)
                for (int sx = sx0; sx < sx1; ++sx) {
                    const unsigned char* p = &raw[(size_t(sy) * bw + sx) * 4];
                    for (int c = 0; c < 4; ++c) sum[c] += p[c];
                    ++n;
                }
            unsigned char* d = &bgra[(size_t(y) * size + x) * 4];
            unsigned a = sum[3] / n;
            for (int c = 0; c < 3; ++c) {
                unsigned v = sum[c] / n;
                d[c] = (unsigned char)(a > 0 ? std::min(255u, v * 255u / a) : 0);
            }
            d[3] = (unsigned char)a;
        }
    }
    return true;
}

uint64_t hashPixels(const std::vector<unsigned char>& px) {
    uint64_t h = 1469598103934665603ull;
    for (unsigned char b : px) { h ^= b; h *= 1099511628211ull; }
    return h;
}

} // namespace

IconAtlas harvestAppIcons(int maxIcons) {
    IconAtlas atlas;
    HRESULT coInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    // Most personal first: taskbar pins, desktop, then the Start Menu.
    std::vector<std::wstring> shortcuts;
    std::wstring appdata = knownFolder(CSIDL_APPDATA);
    if (!appdata.empty())
        listShortcuts(appdata + L"\\Microsoft\\Internet Explorer\\Quick Launch\\User Pinned\\TaskBar", 0, shortcuts);
    for (int csidl : { CSIDL_DESKTOPDIRECTORY, CSIDL_COMMON_DESKTOPDIRECTORY, CSIDL_PROGRAMS, CSIDL_COMMON_PROGRAMS }) {
        std::wstring dir = knownFolder(csidl);
        if (!dir.empty()) listShortcuts(dir, 0, shortcuts);
    }

    std::vector<std::vector<unsigned char>> icons;
    std::unordered_set<uint64_t> seen;
    std::vector<unsigned char> px;
    for (const auto& lnk : shortcuts) {
        if ((int)icons.size() >= maxIcons) break;
        if (!getIconPixels(lnk, atlas.cell, px)) continue;
        uint64_t h = hashPixels(px);
        if (!seen.insert(h).second) continue;          // duplicate icon
        icons.push_back(px);
    }
    if (SUCCEEDED(coInit)) CoUninitialize();

    atlas.count = (int)icons.size();
    if (atlas.count == 0) return atlas;
    atlas.rows = (atlas.count + atlas.cols - 1) / atlas.cols;
    atlas.width = atlas.cols * atlas.cell;
    atlas.height = atlas.rows * atlas.cell;
    atlas.rgba.assign(size_t(atlas.width) * size_t(atlas.height) * 4, 0);

    // Pack: cell (c, r) -> pixels; OpenGL textures are bottom-up, so row 0 of
    // the atlas is at the bottom and each icon is flipped vertically.
    for (int i = 0; i < atlas.count; ++i) {
        int c = i % atlas.cols, r = i / atlas.cols;
        for (int y = 0; y < atlas.cell; ++y) {
            int ay = r * atlas.cell + (atlas.cell - 1 - y);
            for (int x = 0; x < atlas.cell; ++x) {
                const unsigned char* s = &icons[i][(size_t(y) * atlas.cell + x) * 4];
                unsigned char* d = &atlas.rgba[(size_t(ay) * atlas.width + c * atlas.cell + x) * 4];
                d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; d[3] = s[3];   // BGRA -> RGBA
            }
        }
    }
    return atlas;
}

#else   // non-Windows: no icon source

IconAtlas harvestAppIcons(int) { return IconAtlas{}; }

#endif
