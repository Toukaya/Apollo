/**
 * @file src/platform/windows/seamless_table.cpp
 * @brief The window table diffing, sequence and acknowledgement rules.
 */
// standard includes
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

  LiSeamlessWindowEntry to_entry(const window_rec &rec) {
    LiSeamlessWindowEntry entry {};
    entry.id = rec.id;
    entry.desktopX = rec.x;
    entry.desktopY = rec.y;
    entry.z = rec.z;
    entry.flags = rec.flags;
    entry.titleHash = rec.title_hash;
    entry.cellW = rec.w;
    entry.cellH = rec.h;
    entry.clientW = rec.w;
    entry.clientH = rec.h;
    return entry;
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
                    int &out_kind, std::uint8_t &out_seq, std::uint8_t &out_mask,
                    std::vector<LiSeamlessWindowEntry> &out_entries) {
    out_kind = 0;
    out_seq = state.seq;
    out_mask = 0;
    out_entries.clear();

    // A version is in flight: either wait, or re-send that same version. The
    // sequence never moves on while the client has not answered.
    if (state.pending) {
      if (now_ms < state.deadline_ms) {
        return false;
      }
      state.pending = false;
      state.published.clear();
    }

    std::uint8_t mask = 0;
    std::vector<window_rec> changed;

    bool delta = state.sent_once && !state.published.empty() &&
                 build_delta(state.published, cur, mask, changed);

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
        out_entries.push_back(to_entry(rec));
      }
    }
    else {
      // Nothing to say when the set is identical to what was acknowledged.
      if (state.sent_once && state.published.size() == cur.size()) {
        bool identical = true;
        for (const auto &rec : cur) {
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
      for (const auto &rec : cur) {
        out_entries.push_back(to_entry(rec));
      }
    }

    out_seq = state.seq;
    state.pending = true;
    state.deadline_ms = now_ms + ACK_TIMEOUT_MS;
    state.sent_once = true;
    return true;
  }

  bool apply_ack(table_state &state, std::uint8_t seq, std::uint8_t result,
                 const std::vector<window_rec> &cur) {
    if (!state.pending || seq != state.seq) {
      return false;
    }

    state.pending = false;

    if (result == SEAMLESS_ACK_APPLIED) {
      state.published = cur;
    }
    else {
      // The client could not apply this version, so the next one starts from
      // nothing and is a snapshot.
      state.published.clear();
      state.sent_once = false;
    }

    ++state.seq;
    return true;
  }
}  // namespace platf::seamless
