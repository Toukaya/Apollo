/**
 * @file src/platform/windows/seamless_pack.h
 * @brief The packing of a window set into one canvas.
 * @details Pure and offline: a canvas rectangle and a window set go in, a
 *          placement plan comes out. No device, no window, no clock, so the same
 *          function decides the layout the encoder composes and the harness
 *          checks. The packer owns the cells, which is what removes the stage-3
 *          overhang shift: a cell no longer has to stay near the window's desktop
 *          position, so nothing is shifted or cropped to fit.
 */
#pragma once

// standard includes
#include <cstdint>
#include <vector>

// local includes
#include "seamless_table.h"
#include "window_enum.h"

namespace platf::seamless {
  /// One placed cell, in canvas coordinates.
  struct pack_cell {
    std::uint32_t id = 0;
    std::uint16_t x = 0;
    std::uint16_t y = 0;
    std::uint16_t w = 0;
    std::uint16_t h = 0;
    std::uint16_t client_w = 0;
    std::uint16_t client_h = 0;
    std::uint8_t z = 0;
  };

  /// Whether a plan is a whole table or an incremental one.
  enum class pack_kind : std::uint8_t {
    none = 0,
    snapshot = 1,
    delta = 2,
  };

  struct pack_plan {
    bool ok = false;             ///< false when nothing could be placed
    // The canvas the plan computed: the smallest 16-pixel-multiple box that holds
    // the packed area, clamped to the protocol maximum. It is an OUTPUT, not the
    // input rectangle, because the encoded frame must cost what the windows cost.
    std::uint16_t canvas_w = 0;
    std::uint16_t canvas_h = 0;
    std::vector<pack_cell> cells;  ///< area-descending, ties by id ascending
    // The whole-plan degrade judgement (reference X2): the total packed area is
    // above 60 percent of the client screen area. The CALLER acts on it; the
    // packer only reports it.
    bool degrade = false;
    pack_kind kind = pack_kind::snapshot;
    double utilization = 0.0;      ///< sum of cell areas over canvas area
  };

  /// The degrade edge, as a fraction of the CLIENT SCREEN area (60 percent).
  constexpr double DEGRADE_AREA_FRACTION = 0.60;

  /// The canvas is expressed in 16-pixel multiples.
  constexpr std::uint16_t CANVAS_ALIGNMENT = 16;

  /// The protocol's canvas maximum, both axes.
  constexpr std::uint16_t CANVAS_MAX = 4096;

  /// Cells are allocated at the window's own size, so the client's sampler is
  /// what scales a cell down to a local window. That is the pixel-budget
  /// decision of this stage: the budget becomes real here, with window-sized
  /// cells and a canvas computed from them, not with shrunken cells.

  /// Counters for one pack_windows call: where a candidate gave up, so an early
  /// give-up can be named from numbers instead of guessed.
  struct packer_stats {
    std::uint32_t candidates = 0;      ///< canvas widths tried
    std::uint32_t row_overflow = 0;    ///< a window did not fit the current row
    std::uint32_t height_overflow = 0; ///< a row did not fit the canvas height
    std::uint32_t too_wide = 0;        ///< a window was wider than the candidate
    std::uint32_t refused = 0;         ///< windows the accepted plan does not hold
  };

  const packer_stats &last_packer_stats();

  /**
   * @brief Pack the windows into the canvas.
   * @param limit the protocol maximum the computed canvas must stay inside.
   * @param windows the window set, in any order.
   * @param previous the plan last sent, or nullptr for a first plan.
   * @param client_screen_area the client's screen area, the reference the
   *        reference's X2 degrade rule is judged against.
   * @return the plan; `kind` is a snapshot whenever the canvas or the membership
   *         differs from `previous`, because only a snapshot carries both.
   * @details Windows are placed in non-increasing area order, ties by id, on a
   *          shelf: each row's height is the tallest cell in it and the next row
   *          starts at the current skyline minimum. The placement depends on the
   *          sorted sizes and ids only, never on the windows' desktop positions,
   *          so moving one window cannot re-layout the others.
   */
  pack_plan pack_windows(const canvas_rect &limit, const std::vector<window_rec> &windows,
                         const pack_plan *previous, double client_screen_area);
}  // namespace platf::seamless
