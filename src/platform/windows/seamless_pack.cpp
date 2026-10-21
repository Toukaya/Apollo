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

  pack_plan pack_windows(const canvas_rect &canvas, const std::vector<window_rec> &windows,
                         const pack_plan *previous) {
    pack_plan plan;
    plan.canvas_w = canvas.w;
    plan.canvas_h = canvas.h;

    if (canvas.w == 0 || canvas.h == 0) {
      return plan;
    }

    std::vector<window_rec> order = windows;
    std::sort(order.begin(), order.end(), place_before);

    const double canvas_area = (double) canvas.w * (double) canvas.h;

    // Shelf packing: one row at a time, the row's height is its tallest cell.
    std::uint32_t shelf_x = 0;
    std::uint32_t shelf_y = 0;
    std::uint32_t shelf_h = 0;

    for (const auto &window : order) {
      if (window.w == 0 || window.h == 0 || window.w > canvas.w || window.h > canvas.h) {
        // A window that cannot fit the canvas at all is not placeable; the caller
        // sees it missing from the cell list.
        continue;
      }

      if (shelf_x + window.w > canvas.w) {
        shelf_x = 0;
        shelf_y += shelf_h;
        shelf_h = 0;
      }
      if (shelf_y + window.h > canvas.h) {
        continue;
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
      cell.whole_window = ((double) window.w * window.h) >= DEGRADE_AREA_FRACTION * canvas_area;
      if (cell.whole_window) {
        plan.degrade = true;
      }
      plan.cells.push_back(cell);

      shelf_x += window.w;
      shelf_h = std::max<std::uint32_t>(shelf_h, window.h);
    }

    if (plan.cells.empty()) {
      return plan;
    }

    plan.ok = true;

    double used = 0;
    for (const auto &cell : plan.cells) {
      used += (double) cell.w * cell.h;
    }
    plan.utilization = used / canvas_area;

    // A snapshot is the only table that carries the canvas and the cells
    // together, so a canvas or membership change must be one; a plan that only
    // moves a cell inside the same canvas may be a delta.
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
