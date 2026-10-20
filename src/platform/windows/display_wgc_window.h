/**
 * @file src/platform/windows/display_wgc_window.h
 * @brief One host window, captured as its own surface.
 * @details A seamless session does not capture the desktop: every window is
 *          captured from its own surface, so a cell holds the window's content
 *          even when the window is partly off the desktop, and a window that is
 *          behind another one still shows what it holds. The mechanics are the
 *          ones the monitor capture already uses: a free threaded frame pool per
 *          window, a producer thread that hands frames to the capture thread, and
 *          the same timeout contract.
 */
#pragma once

// standard includes
#include <chrono>
#include <cstdint>
#include <memory>

// platform includes
#include <d3d11.h>
#include <windows.h>

// local includes
#include "src/video.h"

namespace platf::seamless {
  /**
   * @brief The capture session of one window.
   * @details Read only: nothing here moves, resizes, focuses or draws a window.
   *          The capture runs until close(), which is what the caller must do
   *          when the window leaves the table.
   */
  class window_capture_t {
  public:
    window_capture_t();
    ~window_capture_t();
    window_capture_t(const window_capture_t &) = delete;
    window_capture_t &operator=(const window_capture_t &) = delete;

    /**
     * @brief Start capturing one window.
     * @param hwnd the window handle the table publishes.
     * @param device the device the canvas lives on; the frames must come from it
     *        so a copy into the canvas needs no cross device transfer.
     * @param framerate the session frame rate, used to bound the update interval.
     * @return 0 on success, -1 when the window cannot be captured.
     */
    int init(HWND hwnd, ID3D11Device *device, std::uint32_t framerate);

    /// Whether the capture session is running.
    bool valid() const;

    /**
     * @brief Wait for the next frame of this window.
     * @param timeout how long to wait.
     * @param out receives the frame texture; it stays valid until release_frame().
     * @param out_time receives the frame's own timestamp, in raw ticks.
     * @return ok, timeout when the window produced nothing in time, or error.
     */
    capture_e next_frame(std::chrono::milliseconds timeout, ID3D11Texture2D **out,
                         std::uint64_t &out_time);

    /// Release the frame next_frame() handed out.
    capture_e release_frame();

    /// Stop capturing and drop the session.
    void close();

  private:
    struct impl_t;
    std::unique_ptr<impl_t> _impl;
  };
}  // namespace platf::seamless
