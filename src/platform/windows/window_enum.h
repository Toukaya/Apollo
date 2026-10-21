/**
 * @file src/platform/windows/window_enum.h
 * @brief The host window set: enumeration, geometry, state and title hash.
 * @details Read only. Nothing here changes a window, injects input or captures
 *          pixels; the table it produces is what the client is told about.
 */
#pragma once

// standard includes
#include <cstdint>
#include <string>
#include <vector>

namespace platf::seamless {
  /**
   * @brief One window as the table describes it.
   * @details Field widths match the wire entry exactly (see SeamlessStream.h in
   *          the shared protocol), so the table builder can fill an entry
   *          without a second conversion step.
   */
  struct window_rec {
    std::uint32_t id = 0;  ///< native handle, stable for the session
    std::uint32_t pid = 0;
    std::int16_t x = 0;
    std::int16_t y = 0;
    std::uint16_t w = 0;
    std::uint16_t h = 0;
    std::uint8_t z = 0;      ///< 0 is the bottom of the stack
    std::uint8_t flags = 0;  ///< SEAMLESS_WF_* and the window state bits
    std::wstring title;
    std::uint32_t title_hash = 0;
    std::uint32_t icon_hash = 0;  ///< hash of the 32x32 BGRA icon, 0 when none
  };

  /// The 32x32 BGRA bytes of an icon, when the host has one for that hash.
  bool icon_bytes(std::uint32_t icon_hash, std::vector<std::uint8_t> &out);

  /// The icon edge the uplink sends: 32 pixels, 4096 bytes of BGRA.
  constexpr std::uint16_t ICON_SIZE = 32;

  /**
   * @brief The window state bits of the flags byte.
   * @details Bit 7 is always on top of the desktop, which is what the client's
   *          sticky bit means.
   */
  std::uint8_t state_bits(bool minimized, bool maximized, bool topmost);

  /**
   * @brief The hash the table carries instead of the title text.
   * @details FNV-1a over the UTF-8 form of the title, truncated to the title
   *          limit first, so both ends can agree on it without a name on the
   *          wire.
   */
  std::uint32_t hash_title(const std::wstring &title);

  /**
   * @brief Read the current window set.
   * @param out the filtered windows, bottom of the z order first.
   * @return true when the enumeration completed.
   * @details Filters: visible, a root owner, neither a tool window nor a
   *          no-activate window, not this process, with a non-empty extended
   *          frame bounds from the desktop compositor. Geometry never comes from
   *          the legacy window rectangle, because that rectangle includes the
   *          invisible resize border. When only_pid is non-zero the set is scoped
   *          to that process: the app the user launched, and its own windows.
   */
  bool snapshot(std::vector<window_rec> &out, const std::vector<std::uint32_t> &only_pids = {});

  /**
   * @brief The processes the table is scoped to: the launched app and every
   *        process that belongs to it (its own children, so helpers and modals
   *        come along). A second launch appends its tree to the caller's set.
   */
  std::vector<std::uint32_t> process_tree(std::uint32_t root);

  /**
   * @brief Install the change hooks and the fallback poll.
   * @param on_change called from whatever thread the hook lands on.
   * @return true when at least the fallback poll is armed.
   * @details The hooks are a wake-up hint, not the source of truth: the poll
   *          re-reads the set, so a missed or duplicated hook costs one poll of
   *          latency and never a wrong table.
   */
  bool install_hooks(void (*on_change)(void *), void *context);

  /// The fallback poll period. Never enumerate per frame.
  constexpr int POLL_INTERVAL_MS = 250;
}  // namespace platf::seamless
