/**
 * @file src/platform/windows/window_enum.cpp
 * @brief The host window set, read from the desktop compositor.
 */
// standard includes
#include <algorithm>
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
      auto *ctx = reinterpret_cast<std::pair<std::vector<window_rec> *, DWORD> *>(param);

      RECT bounds {};
      if (!is_candidate(hwnd, ctx->second, bounds)) {
        return TRUE;
      }

      window_rec rec;
      rec.id = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(hwnd));
      DWORD pid = 0;
      GetWindowThreadProcessId(hwnd, &pid);
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
      rec.flags |= SEAMLESS_WF_VISIBLE;
      rec.flags |= SEAMLESS_WF_CAPTURABLE;

      ctx->first->push_back(std::move(rec));
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

  bool snapshot(std::vector<window_rec> &out) {
    out.clear();

    const auto own_pid = GetCurrentProcessId();
    std::pair<std::vector<window_rec> *, DWORD> ctx {&out, own_pid};

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
