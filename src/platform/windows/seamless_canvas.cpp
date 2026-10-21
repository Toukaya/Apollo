/**
 * @file src/platform/windows/seamless_canvas.cpp
 * @brief The compose plan: the cells of one pass, and the rule that a pass with
 *        nothing new does not happen.
 */
// standard includes
#include <algorithm>

// local includes
#include "seamless_canvas.h"

namespace platf::seamless {
  namespace {
    // Written once by the capture side when a session starts, read by the
    // publisher afterwards. A plain bool is enough for a one-way init-time flag
    // and keeps this translation unit free of the atomic header, which drags in
    // a pthread chain this harness cannot compile.
    bool compose_ok = true;
  }

  namespace {
    /// The publishable cells of a window set, bottom of the z order first.
    std::vector<canvas_cell> cells_of(const canvas_rect &canvas, const std::vector<window_rec> &set) {
      std::vector<canvas_cell> cells;
      cells.reserve(set.size());
      for (const auto &rec : set) {
        LiSeamlessWindowEntry entry {};
        if (!to_entry(rec, canvas, entry)) {
          continue;
        }
        cells.push_back(canvas_cell {entry.id, entry.cellX, entry.cellY, entry.cellW, entry.cellH, entry.z});
      }
      std::sort(cells.begin(), cells.end(),
                [](const canvas_cell &a, const canvas_cell &b) { return a.z < b.z; });
      return cells;
    }

    const canvas_cell *find_cell(const std::vector<canvas_cell> &cells, std::uint32_t id) {
      for (const auto &cell : cells) {
        if (cell.id == id) {
          return &cell;
        }
      }
      return nullptr;
    }
  }  // namespace

  bool compose_available() {
    return compose_ok;
  }

  void set_compose_available(bool available) {
    compose_ok = available;
  }

  std::uint32_t crc32_begin() {
    return 0xFFFFFFFFu;
  }

  std::uint32_t crc32_update(std::uint32_t state, const std::uint8_t *data, std::size_t length) {
    for (std::size_t i = 0; i < length; i++) {
      state ^= data[i];
      for (int bit = 0; bit < 8; bit++) {
        state = (state & 1u) ? ((state >> 1) ^ 0xEDB88320u) : (state >> 1);
      }
    }
    return state;
  }

  std::uint32_t crc32_finish(std::uint32_t state) {
    return state ^ 0xFFFFFFFFu;
  }

  std::uint32_t crc32_of(const std::uint8_t *data, std::size_t length) {
    return crc32_finish(crc32_update(crc32_begin(), data, length));
  }

  void compose_cells(ID3D11DeviceContext *context, ID3D11Texture2D *canvas,
                     const std::vector<canvas_copy> &copies) {
    if (context == nullptr || canvas == nullptr) {
      return;
    }

    for (const auto &copy : copies) {
      if (copy.source == nullptr || copy.cell.w == 0 || copy.cell.h == 0) {
        continue;
      }
      // The whole window, never a crop: the source box is the window's size and
      // the client samples the same rectangle.
      const D3D11_BOX box {0, 0, 0, copy.cell.w, copy.cell.h, 1};
      context->CopySubresourceRegion(canvas, 0, copy.cell.x, copy.cell.y, 0, copy.source, 0, &box);
    }
  }

  canvas_plan plan_compose(const canvas_rect &canvas,
                           const std::vector<window_rec> &previous,
                           const std::vector<window_rec> &current,
                           const std::vector<std::uint32_t> &dirty) {
    canvas_plan plan;
    plan.canvas_w = canvas.w;
    plan.canvas_h = canvas.h;
    plan.cells = cells_of(canvas, current);

    const auto before = cells_of(canvas, previous);

    // Membership: a cell count change, or a cell that has no counterpart, means
    // the canvas describes a different set than the last pass.
    if (before.size() != plan.cells.size()) {
      plan.changed = true;
      plan.reason = compose_reason::membership;
    }
    else {
      for (const auto &cell : plan.cells) {
        if (find_cell(before, cell.id) == nullptr) {
          plan.changed = true;
          plan.reason = compose_reason::membership;
          break;
        }
      }
    }

    // Geometry: the same windows, but a cell moved, changed shape or changed its
    // place in the order.
    if (!plan.changed) {
      for (const auto &cell : plan.cells) {
        const auto *was = find_cell(before, cell.id);
        if (was == nullptr || was->x != cell.x || was->y != cell.y || was->w != cell.w ||
            was->h != cell.h || was->z != cell.z) {
          plan.changed = true;
          plan.reason = compose_reason::geometry;
          break;
        }
      }
    }

    // Content: a window whose capture produced a frame since the last pass.
    if (!plan.changed) {
      for (const auto id : dirty) {
        if (find_cell(plan.cells, id) != nullptr) {
          plan.changed = true;
          plan.reason = compose_reason::content;
          break;
        }
      }
    }

    return plan;
  }
}  // namespace platf::seamless
