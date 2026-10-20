/**
 * @file src/platform/windows/seamless_canvas.h
 * @brief The canvas a seamless session encodes: which cells to copy, and when a
 *        pass has nothing to do.
 * @details Stage 3 composes every window into its cell on every pass. What makes
 *          that cheap is not the copy, it is that a pass with no new content does
 *          not happen at all: the caller gets "nothing changed" and returns
 *          capture_e::timeout, so the existing skip path runs and no blank frame
 *          is encoded. The plan is pure - it reads the window set and the set of
 *          windows whose capture produced a frame, never a device and never a
 *          window - which is what makes the timeout rule testable without a
 *          session.
 */
#pragma once

// standard includes
#include <cstdint>
#include <vector>

// platform includes
#include <d3d11.h>
#include <windows.h>

// local includes
#include "seamless_table.h"
#include "window_enum.h"

namespace platf::seamless {
  /// One cell of the canvas: where a window's rectangle lands.
  struct canvas_cell {
    std::uint32_t id = 0;
    std::uint16_t x = 0;  ///< canvas pixels
    std::uint16_t y = 0;
    std::uint16_t w = 0;
    std::uint16_t h = 0;
    std::uint8_t z = 0;
  };

  /// Why a compose pass has work, strongest first.
  enum class compose_reason : std::uint8_t {
    none = 0,        ///< nothing changed: the pass is a timeout
    membership = 1,  ///< a window appeared or disappeared
    geometry = 2,    ///< a cell moved or changed shape, or the order changed
    content = 3,     ///< a window produced a frame
  };

  /// What one compose pass has to do.
  struct canvas_plan {
    bool changed = false;
    compose_reason reason = compose_reason::none;
    std::uint16_t canvas_w = 0;
    std::uint16_t canvas_h = 0;
    std::vector<canvas_cell> cells;  ///< bottom of the z order first
  };

  /**
   * @brief Plan one compose pass.
   * @param canvas the canvas the cells are measured from.
   * @param previous the set the last pass composed.
   * @param current the set to compose now.
   * @param dirty ids of windows whose capture produced a frame since the last pass.
   * @return the plan; when it has no reason, the caller times out instead of
   *         encoding, which is the whole rule behind "no blank frames".
   * @details Membership is checked first, then geometry (the cell rectangle or
   *          the z order), then content. The cells are always the whole set,
   *          because stage 3 rebuilds the canvas; stage 7 makes that list the
   *          dirty subset only.
   */
  /// One window's surface and the cell it belongs in.
  struct canvas_copy {
    ID3D11Texture2D *source = nullptr;
    canvas_cell cell;
  };

  /**
   * @brief Copy every window's own surface into its cell.
   * @details Pure copies: the cell is the window's size (cellW == clientW), so
   *          nothing here scales. The canvas is cleared by the caller when its
   *          previous content must not show through.
   */
  void compose_cells(ID3D11DeviceContext *context, ID3D11Texture2D *canvas,
                     const std::vector<canvas_copy> &copies);

  canvas_plan plan_compose(const canvas_rect &canvas,
                           const std::vector<window_rec> &previous,
                           const std::vector<window_rec> &current,
                           const std::vector<std::uint32_t> &dirty);
}  // namespace platf::seamless
