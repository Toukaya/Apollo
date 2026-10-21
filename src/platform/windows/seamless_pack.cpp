/**
 * @file src/platform/windows/seamless_pack.cpp
 * @brief The shelf packer and the plan's snapshot rule.
 */
// standard includes
#include <algorithm>
#include <cstdint>

// local includes
#include "seamless_pack.h"

namespace platf::seamless {
  namespace {
    /// The order windows are placed in: larger area first, ties by id.
    bool place_before(const window_rec &a, const window_rec &b) {
      const std::uint32_t area_a = (std::uint32_t) a.w * a.h;
      const std::uint32_t area_b = (std::uint32_t) b.w * b.h;
      if (area_a != area_b) {
        return area_a > area_b;
      }
      return a.id < b.id;
    }

    bool same_membership(const std::vector<pack_cell> &cells, const std::vector<window_rec> &windows) {
      if (cells.size() != windows.size()) {
        return false;
      }
      for (const auto &window : windows) {
        bool found = false;
        for (const auto &cell : cells) {
          if (cell.id == window.id) {
            found = true;
            break;
          }
        }
        if (!found) {
          return false;
        }
      }
      return true;
    }
  }  // namespace

  pack_plan pack_windows(const canvas_rect &limit, const std::vector<window_rec> &windows,
                         const pack_plan *previous, double client_screen_area) {
    pack_plan plan;

    std::vector<window_rec> order = windows;
    std::sort(order.begin(), order.end(), place_before);

    // Drop what can never be placed, and measure what has to fit.
    std::uint64_t total_area = 0;
    std::uint32_t widest = 0;
    std::vector<window_rec> placeable;
    for (const auto &window : order) {
      if (window.w == 0 || window.h == 0) {
        continue;
      }
      // A window that can never fit the protocol maximum leaves the table; the
      // others are still placed. One such window must not cost the session its
      // whole table.
      if (window.w > CANVAS_MAX || window.h > CANVAS_MAX) {
        continue;
      }
      total_area += (std::uint64_t) window.w * window.h;
      widest = std::max<std::uint32_t>(widest, window.w);
      placeable.push_back(window);
    }
    if (placeable.empty()) {
      return plan;
    }

    // The smallest 16-pixel-multiple box that actually holds the shelf packing:
    // for every candidate width, simulate the packing and keep the box with the
    // smallest area. The canvas is what the windows cost, not the desktop rect.
    const std::uint32_t limit_w = std::min<std::uint32_t>(limit.w ? limit.w : CANVAS_MAX, CANVAS_MAX);
    const std::uint32_t limit_h = std::min<std::uint32_t>(limit.h ? limit.h : CANVAS_MAX, CANVAS_MAX);

    std::uint32_t best_w = 0;
    std::uint32_t best_h = 0;
    std::vector<pack_cell> best_cells;

    for (std::uint32_t candidate_w = (std::uint32_t) (CANVAS_ALIGNMENT *
                                                       ((widest + CANVAS_ALIGNMENT - 1) / CANVAS_ALIGNMENT));
         candidate_w <= limit_w; candidate_w += CANVAS_ALIGNMENT) {
      std::vector<pack_cell> cells;
      std::uint32_t shelf_x = 0;
      std::uint32_t shelf_y = 0;
      std::uint32_t shelf_h = 0;
      bool fits = true;

      for (const auto &window : placeable) {
        if (window.w > candidate_w) {
          fits = false;
          break;
        }
        if (shelf_x + window.w > candidate_w) {
          shelf_x = 0;
          shelf_y += shelf_h;
          shelf_h = 0;
        }
        if (shelf_y + window.h > limit_h) {
          fits = false;
          break;
        }

        pack_cell cell;
        cell.id = window.id;
        cell.x = (std::uint16_t) shelf_x;
        cell.y = (std::uint16_t) shelf_y;
        cell.w = window.w;
        cell.h = window.h;
        cell.client_w = window.w;
        cell.client_h = window.h;
        cell.z = window.z;
        cells.push_back(cell);

        shelf_x += window.w;
        shelf_h = std::max<std::uint32_t>(shelf_h, window.h);
      }

      if (!fits) {
        continue;
      }

      const std::uint32_t needed_h = (std::uint32_t) (CANVAS_ALIGNMENT *
                                                      ((shelf_y + shelf_h + CANVAS_ALIGNMENT - 1) / CANVAS_ALIGNMENT));
      const std::uint64_t area = (std::uint64_t) candidate_w * needed_h;
      if (best_w == 0 || area < (std::uint64_t) best_w * best_h) {
        best_w = candidate_w;
        best_h = needed_h;
        best_cells = std::move(cells);
      }
    }

    if (best_w == 0) {
      return plan;
    }

    plan.ok = true;
    plan.canvas_w = (std::uint16_t) best_w;
    plan.canvas_h = (std::uint16_t) best_h;
    plan.cells = std::move(best_cells);

    double used = 0;
    for (const auto &cell : plan.cells) {
      used += (double) cell.w * cell.h;
    }
    plan.utilization = used / ((double) plan.canvas_w * (double) plan.canvas_h);

    // X2: the whole-plan degrade, total packed area against the CLIENT SCREEN
    // area, reported here and acted on by the caller - never per window and never
    // against the canvas.
    plan.degrade = client_screen_area > 0 && used > DEGRADE_AREA_FRACTION * client_screen_area;

    if (previous == nullptr || !previous->ok || previous->canvas_w != plan.canvas_w ||
        previous->canvas_h != plan.canvas_h || !same_membership(previous->cells, order)) {
      plan.kind = pack_kind::snapshot;
    }
    else {
      plan.kind = pack_kind::delta;
    }

    return plan;
  }
}  // namespace platf::seamless
