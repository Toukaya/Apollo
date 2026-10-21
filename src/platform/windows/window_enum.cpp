/**
 * @file src/platform/windows/window_enum.cpp
 * @brief The host window set, read from the desktop compositor.
 */
// standard includes
#include <algorithm>
#include <map>
#include <cstring>

// platform includes
#include <windows.h>
#include <dwmapi.h>

// local includes
#include "window_enum.h"

#include <moonlight-common-c/src/SeamlessStream.h>

using namespace std::literals;

namespace platf::seamless {
  namespace {
    std::map<std::uint32_t, std::vector<std::uint8_t>> g_icons;

    /// FNV-1a over the icon bytes: the wire carries this hash, never the pixels.
    std::uint32_t hash_bytes(const std::uint8_t *data, std::size_t length) {
      std::uint32_t hash = 2166136261u;
      for (std::size_t i = 0; i < length; i++) {
        hash ^= data[i];
        hash *= 16777619u;
      }
      return hash;
    }

    /**
     * @brief Draw one window icon into a 32 by 32 BGRA buffer.
     * @details The compositor hands out HICONs in two shapes: 32-bit with alpha,
     *          and 1-bit masked. DrawIconEx fills a 32-bit DIB for both; the
     *          masked ones come back with every alpha byte at zero, so the alpha
     *          pass below reconstructs it from the colour (a transparent pixel in a
     *          masked icon is black with the mask bit set).
     */
    bool extract_icon(HICON icon, std::vector<std::uint8_t> &out) {
      if (icon == nullptr) {
        return false;
      }

      BITMAPINFO info {};
      info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
      info.bmiHeader.biWidth = ICON_SIZE;
      info.bmiHeader.biHeight = -ICON_SIZE;  // top down
      info.bmiHeader.biPlanes = 1;
      info.bmiHeader.biBitCount = 32;
      info.bmiHeader.biCompression = BI_RGB;

      HDC dc = CreateCompatibleDC(nullptr);
      if (dc == nullptr) {
        return false;
      }

      void *bits = nullptr;
      HBITMAP dib = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
      if (dib == nullptr || bits == nullptr) {
        DeleteDC(dc);
        return false;
      }

      HGDIOBJ previous = SelectObject(dc, dib);
      DrawIconEx(dc, 0, 0, icon, ICON_SIZE, ICON_SIZE, 0, nullptr, DI_NORMAL);
      SelectObject(dc, previous);

      out.assign(static_cast<std::uint8_t *>(bits), static_cast<std::uint8_t *>(bits) + ICON_SIZE * ICON_SIZE * 4);

      bool anyAlpha = false;
      for (std::size_t i = 3; i < out.size(); i += 4) {
        if (out[i] != 0) {
          anyAlpha = true;
          break;
        }
      }
      if (!anyAlpha) {
        for (std::size_t i = 0; i < out.size(); i += 4) {
          const bool transparent = out[i] == 0 && out[i + 1] == 0 && out[i + 2] == 0;
          out[i + 3] = transparent ? 0 : 255;
        }
      }

      DeleteObject(dib);
      DeleteDC(dc);
      return true;
    }

    /// What the enumeration callback needs: the output, this process, and the
    /// optional app the table is scoped to.
    struct collect_ctx_t {
      std::vector<window_rec> *out;
      DWORD own_pid;
      std::uint32_t only_pid;
    };

    /// The title is truncated to the protocol limit, in bytes, before hashing.
    constexpr std::size_t TITLE_LIMIT = SEAMLESS_MAX_TITLE;

    /**
     * @brief The geometry the compositor reports for a window.
     * @details The legacy rectangle includes the invisible resize border, so a
     *          client drawing that rectangle is off by the border on every edge.
     *          When the compositor has no answer, the window is skipped rather
     *          than described by the legacy rectangle.
     */
    bool composed_bounds(HWND hwnd, RECT &out) {
      RECT bounds {};
      if (FAILED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &bounds, sizeof(bounds)))) {
        return false;
      }
      if (bounds.right - bounds.left <= 0 || bounds.bottom - bounds.top <= 0) {
        return false;
      }

      out = bounds;
      return true;
    }

    /**
     * @brief Whether a window belongs in the table.
     * @details A window the client cannot draw or focus is worse than absent: it
     *          would occupy an entry and a canvas cell for nothing.
     */
    bool is_candidate(HWND hwnd, DWORD own_pid, RECT &bounds) {
      if (!IsWindowVisible(hwnd)) {
        return false;
      }

      // Only the root owner window of a group: an owned dialog is drawn inside
      // its owner, so listing both would draw the dialog twice.
      if (GetAncestor(hwnd, GA_ROOTOWNER) != hwnd) {
        return false;
      }

      const auto ex_style = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE));
      if ((ex_style & WS_EX_TOOLWINDOW) || (ex_style & WS_EX_NOACTIVATE)) {
        return false;
      }

      DWORD pid = 0;
      GetWindowThreadProcessId(hwnd, &pid);
      if (pid == 0 || pid == own_pid) {
        return false;
      }

      return composed_bounds(hwnd, bounds);
    }

    /**
     * @brief The z order of the visible windows, top of the stack first.
     * @details EnumWindows already walks the stack in order, so the index in the
     *          call is the order; the table stores it inverted, because the wire
     *          says 0 is the bottom.
     */
    BOOL CALLBACK collect_proc(HWND hwnd, LPARAM param) {
      auto *ctx = reinterpret_cast<collect_ctx_t *>(param);

      RECT bounds {};
      if (!is_candidate(hwnd, ctx->own_pid, bounds)) {
        return TRUE;
      }

      window_rec rec;
      rec.id = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(hwnd));
      DWORD pid = 0;
      GetWindowThreadProcessId(hwnd, &pid);

      // Scoped to the launched app when the caller asks for it: a user picking
      // one app gets that app's windows (and its own children), nothing else.
      if (ctx->only_pid != 0 && pid != ctx->only_pid) {
        return TRUE;
      }

      rec.pid = static_cast<std::uint32_t>(pid);
      rec.x = static_cast<std::int16_t>(bounds.left);
      rec.y = static_cast<std::int16_t>(bounds.top);
      rec.w = static_cast<std::uint16_t>(bounds.right - bounds.left);
      rec.h = static_cast<std::uint16_t>(bounds.bottom - bounds.top);

      wchar_t title[SEAMLESS_MAX_TITLE] {};
      const auto length = GetWindowTextW(hwnd, title, SEAMLESS_MAX_TITLE);
      if (length > 0) {
        rec.title.assign(title, static_cast<std::size_t>(length));
        rec.flags |= SEAMLESS_WF_HAS_TITLE;
      }
      rec.title_hash = hash_title(rec.title);

      const auto ex_style = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE));
      rec.flags |= state_bits(IsIconic(hwnd) != 0, IsZoomed(hwnd) != 0,
                              (ex_style & WS_EX_TOPMOST) != 0);

      // The icon: WM_GETICON first, then the class icon. Only the hash crosses the
      // wire, and only once per hash.
      HICON icon = reinterpret_cast<HICON>(SendMessageTimeoutW(hwnd, WM_GETICON, ICON_SMALL2, 0,
                                                               SMTO_ABORTIFHUNG, 100, nullptr));
      if (icon == nullptr) {
        icon = reinterpret_cast<HICON>(GetClassLongPtrW(hwnd, GCLP_HICONSM));
      }
      if (icon == nullptr) {
        icon = reinterpret_cast<HICON>(GetClassLongPtrW(hwnd, GCLP_HICON));
      }

      std::vector<std::uint8_t> iconPixels;
      if (extract_icon(icon, iconPixels) && iconPixels.size() == ICON_SIZE * ICON_SIZE * 4) {
        rec.icon_hash = hash_bytes(iconPixels.data(), iconPixels.size());
        g_icons[rec.icon_hash] = std::move(iconPixels);
        rec.flags |= SEAMLESS_WF_HAS_ICON;
      }
      rec.flags |= SEAMLESS_WF_VISIBLE;
      rec.flags |= SEAMLESS_WF_CAPTURABLE;

      ctx->out->push_back(std::move(rec));
      return TRUE;
    }

    constexpr UINT HOOK_EVENTS[] {
      EVENT_OBJECT_CREATE,
      EVENT_OBJECT_DESTROY,
      EVENT_OBJECT_LOCATIONCHANGE,
      EVENT_OBJECT_NAMECHANGE,
    };

  }  // namespace

  std::uint8_t state_bits(bool minimized, bool maximized, bool topmost) {
    std::uint8_t bits = 0;
    if (minimized) {
      bits |= (SEAMLESS_WSTATE_MINIMIZED << SEAMLESS_WF_STATE_SHIFT);
    }
    else if (maximized) {
      bits |= (SEAMLESS_WSTATE_MAXIMIZED << SEAMLESS_WF_STATE_SHIFT);
    }
    if (topmost) {
      bits |= SEAMLESS_WF_STICKY;
    }
    return bits;
  }

  std::uint32_t hash_title(const std::wstring &title) {
    // The table carries a hash, not a name, so both ends can compare titles
    // without the title crossing the wire.
    std::uint32_t hash = 2166136261u;
    std::size_t bytes = 0;

    for (const wchar_t wide : title) {
      char utf8[4] {};
      int length = 0;

      const auto code = static_cast<std::uint32_t>(wide);
      if (code < 0x80) {
        utf8[length++] = static_cast<char>(code);
      }
      else if (code < 0x800) {
        utf8[length++] = static_cast<char>(0xC0 | (code >> 6));
        utf8[length++] = static_cast<char>(0x80 | (code & 0x3F));
      }
      else {
        utf8[length++] = static_cast<char>(0xE0 | (code >> 12));
        utf8[length++] = static_cast<char>(0x80 | ((code >> 6) & 0x3F));
        utf8[length++] = static_cast<char>(0x80 | (code & 0x3F));
      }

      for (int i = 0; i < length; ++i) {
        if (bytes >= TITLE_LIMIT) {
          return hash;
        }
        hash ^= static_cast<std::uint8_t>(utf8[i]);
        hash *= 16777619u;
        ++bytes;
      }
    }

    return hash;
  }

  bool icon_bytes(std::uint32_t icon_hash, std::vector<std::uint8_t> &out) {
    const auto it = g_icons.find(icon_hash);
    if (it == g_icons.end()) {
      return false;
    }
    out = it->second;
    return true;
  }

  bool snapshot(std::vector<window_rec> &out, std::uint32_t only_pid) {
    out.clear();

    const auto own_pid = GetCurrentProcessId();
    collect_ctx_t ctx {&out, own_pid, only_pid};

    if (!EnumWindows(collect_proc, reinterpret_cast<LPARAM>(&ctx))) {
      out.clear();
      return false;
    }

    // EnumWindows walks top of the stack first; the wire counts from the bottom.
    const auto count = static_cast<std::uint8_t>(std::min<std::size_t>(out.size(), SEAMLESS_MAX_WINDOWS));
    if (out.size() > SEAMLESS_MAX_WINDOWS) {
      out.resize(SEAMLESS_MAX_WINDOWS);
    }
    for (std::size_t i = 0; i < count; ++i) {
      out[i].z = static_cast<std::uint8_t>(count - 1 - i);
    }

    return true;
  }

  bool install_hooks(void (*on_change)(void *), void *context) {
    // The hooks tell the table thread to re-read early; the poll is what makes
    // the set correct even when a hook is missed. Neither changes a window.
    static void (*callback)(void *) = nullptr;
    static void *callback_context = nullptr;
    callback = on_change;
    callback_context = context;

    for (const auto event : HOOK_EVENTS) {
      SetWinEventHook(event, event, nullptr,
                      [](HWINEVENTHOOK, DWORD, HWND, LONG, LONG, DWORD, DWORD) {
                        if (callback) {
                          callback(callback_context);
                        }
                      },
                      0, 0, WINEVENT_OUTOFCONTEXT);
    }

    return true;
  }
}  // namespace platf::seamless
