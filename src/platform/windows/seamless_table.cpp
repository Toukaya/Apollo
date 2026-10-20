/**
 * @file src/platform/windows/seamless_table.cpp
 * @brief The window table diffing, sequence and acknowledgement rules.
 */
// standard includes
#include <algorithm>
#include <cstring>

// platform includes
#include <windows.h>

// local includes
#include "seamless_table.h"

namespace platf::seamless {
  namespace {
    bool same_geometry(const window_rec &a, const window_rec &b) {
      return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
    }

    bool same_z(const window_rec &a, const window_rec &b) {
      return a.z == b.z;
    }

    bool same_flags(const window_rec &a, const window_rec &b) {
      return a.flags == b.flags;
    }

    const window_rec *find_by_id(const std::vector<window_rec> &set, std::uint32_t id) {
      for (const auto &rec : set) {
        if (rec.id == id) {
          return &rec;
        }
      }
      return nullptr;
    }
  }  // namespace

  work_area_t virtual_work_area() {
    work_area_t area;

    RECT rect {};
    if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &rect, 0)) {
      area.x = static_cast<std::int16_t>(rect.left);
      area.y = static_cast<std::int16_t>(rect.top);
      area.w = static_cast<std::uint16_t>(rect.right - rect.left);
      area.h = static_cast<std::uint16_t>(rect.bottom - rect.top);
    }

    return area;
  }

  canvas_rect virtual_canvas() {
    canvas_rect canvas;

    canvas.x = static_cast<std::int16_t>(GetSystemMetrics(SM_XVIRTUALSCREEN));
    canvas.y = static_cast<std::int16_t>(GetSystemMetrics(SM_YVIRTUALSCREEN));
    canvas.w = static_cast<std::uint16_t>(std::clamp(GetSystemMetrics(SM_CXVIRTUALSCREEN), 0, SEAMLESS_MAX_CANVAS));
    canvas.h = static_cast<std::uint16_t>(std::clamp(GetSystemMetrics(SM_CYVIRTUALSCREEN), 0, SEAMLESS_MAX_CANVAS));

    return canvas;
  }

  bool to_entry(const window_rec &rec, const canvas_rect &canvas, LiSeamlessWindowEntry &out) {
    out = LiSeamlessWindowEntry {};

    if (rec.w == 0 || rec.h == 0 || rec.w > canvas.w || rec.h > canvas.h) {
      return false;
    }

    // The cell is the whole window, never a crop of it: the client draws the
    // cell the host filled, so a cropped cell would be drawn stretched. A window
    // that hangs over an edge keeps its whole rectangle and the cell moves by the
    // overhang to stay inside the canvas; packing owns the layout from stage 4.
    std::int32_t cell_x = static_cast<std::int32_t>(rec.x) - canvas.x;
    std::int32_t cell_y = static_cast<std::int32_t>(rec.y) - canvas.y;
    cell_x = std::clamp<std::int32_t>(cell_x, 0, static_cast<std::int32_t>(canvas.w) - rec.w);
    cell_y = std::clamp<std::int32_t>(cell_y, 0, static_cast<std::int32_t>(canvas.h) - rec.h);

    out.id = rec.id;
    out.desktopX = rec.x;
    out.desktopY = rec.y;
    out.z = rec.z;
    out.flags = rec.flags;
    out.titleHash = rec.title_hash;
    out.cellX = static_cast<std::uint16_t>(cell_x);
    out.cellY = static_cast<std::uint16_t>(cell_y);
    out.cellW = rec.w;
    out.cellH = rec.h;
    out.clientW = rec.w;
    out.clientH = rec.h;
    return true;
  }

  bool build_delta(const std::vector<window_rec> &prev, const std::vector<window_rec> &cur,
                   std::uint8_t &mask, std::vector<window_rec> &changed) {
    mask = 0;
    changed.clear();

    // Membership change: a delta carries no removal, so only a snapshot can
    // describe a set that gained or lost a window.
    if (prev.size() != cur.size()) {
      return false;
    }
    for (const auto &rec : cur) {
      if (find_by_id(prev, rec.id) == nullptr) {
        return false;
      }
    }

    for (const auto &rec : cur) {
      const auto *before = find_by_id(prev, rec.id);
      std::uint8_t groups = 0;
      if (!same_geometry(*before, rec)) {
        groups |= SEAMLESS_DM_GEOMETRY;
      }
      if (!same_z(*before, rec)) {
        groups |= SEAMLESS_DM_ZORDER;
      }
      if (!same_flags(*before, rec)) {
        groups |= SEAMLESS_DM_FLAGS;
      }
      if (before->title_hash != rec.title_hash) {
        groups |= SEAMLESS_DM_TITLE;
      }

      if (groups != 0) {
        mask |= groups;
        changed.push_back(rec);
      }
    }

    return mask != 0;
  }

  bool plan_publish(table_state &state, const std::vector<window_rec> &cur, std::int64_t now_ms,
                    const canvas_rect &canvas,
                    int &out_kind, std::uint8_t &out_seq, std::uint8_t &out_mask,
                    std::vector<LiSeamlessWindowEntry> &out_entries) {
    out_kind = 0;
    out_seq = state.seq;
    out_mask = 0;
    out_entries.clear();

    // The table describes the windows that keep a cell on the canvas. A window
    // that does not fit has nothing the client could draw, so it is not part of
    // the set the client is told about, and its arrival or departure is not a
    // membership change the client could apply.
    std::vector<window_rec> shown;
    shown.reserve(cur.size());
    for (const auto &rec : cur) {
      LiSeamlessWindowEntry entry {};
      if (to_entry(rec, canvas, entry)) {
        shown.push_back(rec);
      }
    }

    // A version is in flight: either wait, or re-send that same version. The
    // sequence never moves on while the client has not answered.
    bool resend = false;
    if (state.pending) {
      if (now_ms < state.deadline_ms) {
        return false;
      }
      state.pending = false;
      state.published.clear();
      state.inflight.clear();
      // The version timed out unacknowledged, so the same version goes out
      // again. This must not fall through to the identical-set test: with an
      // empty window set that test sees 0 against 0 and decides there is
      // nothing to publish, which would leave the host waiting forever.
      resend = true;
    }

    // A delta carries no canvas, so a version whose canvas differs from the last
    // sent one has to be a snapshot: the client would read its cells against the
    // canvas it holds.
    const bool canvas_moved = state.sent_once &&
                              (state.canvas.x != canvas.x || state.canvas.y != canvas.y ||
                               state.canvas.w != canvas.w || state.canvas.h != canvas.h);

    std::uint8_t mask = 0;
    std::vector<window_rec> changed;

    bool delta = state.sent_once && !canvas_moved && !state.published.empty() &&
                 build_delta(state.published, shown, mask, changed);

    if (!state.sent_once) {
      delta = false;
    }

    if (delta) {
      if (changed.empty()) {
        return false;
      }
      out_kind = 2;
      out_mask = mask;
      for (const auto &rec : changed) {
        LiSeamlessWindowEntry entry {};
        if (to_entry(rec, canvas, entry)) {
          out_entries.push_back(entry);
        }
      }
      if (out_entries.empty()) {
        return false;
      }
    }
    else {
      // Nothing to say when the set is identical to what was acknowledged.
      if (!resend && state.sent_once && state.published.size() == shown.size()) {
        bool identical = true;
        for (const auto &rec : shown) {
          const auto *before = find_by_id(state.published, rec.id);
          if (before == nullptr || !same_geometry(*before, rec) || !same_z(*before, rec) ||
              !same_flags(*before, rec) || before->title_hash != rec.title_hash) {
            identical = false;
            break;
          }
        }
        if (identical) {
          return false;
        }
      }

      out_kind = 1;
      for (const auto &rec : shown) {
        LiSeamlessWindowEntry entry {};
        if (to_entry(rec, canvas, entry)) {
          out_entries.push_back(entry);
        }
      }
    }

    out_seq = state.seq;
    state.pending = true;
    state.deadline_ms = now_ms + ACK_TIMEOUT_MS;
    state.sent_once = true;
    state.inflight = shown;
    state.canvas = canvas;
    return true;
  }

  bool encode_publish(table_state &state, const std::vector<window_rec> &cur, std::int64_t now_ms,
                      const canvas_rect &canvas, const work_area_t &area,
                      int &out_kind, std::uint8_t &out_seq, std::uint8_t &out_mask,
                      std::vector<std::uint8_t> &out_wire) {
    out_wire.clear();

    std::vector<LiSeamlessWindowEntry> entries;
    if (!plan_publish(state, cur, now_ms, canvas, out_kind, out_seq, out_mask, entries)) {
      return false;
    }

    out_wire.resize(SEAMLESS_SNAPSHOT_HEADER_SIZE + SEAMLESS_MAX_WINDOWS * SEAMLESS_ENTRY_SIZE);

    int length = 0;
    if (out_kind == 1) {
      length = LiEncodeWindowSnapshot(out_wire.data(), static_cast<int>(out_wire.size()), out_seq,
                                      canvas.w, canvas.h, area.x, area.y, area.w, area.h,
                                      entries.data(), static_cast<int>(entries.size()));
    }
    else {
      length = LiEncodeWindowDelta(out_wire.data(), static_cast<int>(out_wire.size()), out_seq,
                                   out_mask, area.x, area.y, area.w, area.h,
                                   entries.data(), static_cast<int>(entries.size()));
    }

    if (length <= 0) {
      // The encoder refused the version, so the client will never see it. The
      // state goes back to before the plan: the next attempt is a snapshot of
      // the set as it stands then.
      state.pending = false;
      state.sent_once = false;
      state.inflight.clear();
      out_wire.clear();
      return false;
    }

    out_wire.resize(static_cast<std::size_t>(length));
    return true;
  }

  bool apply_ack(table_state &state, std::uint8_t seq, std::uint8_t result) {
    if (!state.pending || seq != state.seq) {
      return false;
    }

    state.pending = false;

    if (result == SEAMLESS_ACK_APPLIED) {
      // The client applied the version that was sent, so that set is the base
      // the next delta is built against. A window that changed while the
      // version was in flight is not in this set, and the next delta carries it.
      state.published = state.inflight;
    }
    else {
      // The client could not apply this version, so the next one starts from
      // nothing and is a snapshot.
      state.published.clear();
      state.sent_once = false;
    }

    state.inflight.clear();

    ++state.seq;
    return true;
  }
}  // namespace platf::seamless
