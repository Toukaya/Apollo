/**
 * @file src/platform/windows/seamless_table.h
 * @brief The window table the host publishes: snapshot, delta, sequence, ack.
 * @details The sequence number is the contract. The host does not advance to the
 *          next version until the client acknowledged the current one, so a
 *          client never applies a table that was built on top of a version it
 *          did not accept. Everything here is a pure function of the window set,
 *          which is what makes the diffing testable without a session.
 */
#pragma once

// standard includes
#include <cstdint>
#include <vector>

// local includes
#include "window_enum.h"

#include <moonlight-common-c/src/SeamlessStream.h>

namespace platf::seamless {
  /// How long the host waits for an acknowledgement before re-sending the table.
  constexpr std::int64_t ACK_TIMEOUT_MS = 1000;

  /// The work area of the virtual desktop, for a maximize on the client side.
  struct work_area_t {
    std::int16_t x = 0;
    std::int16_t y = 0;
    std::uint16_t w = 0;
    std::uint16_t h = 0;
  };

  /// Read the work area of the virtual desktop.
  work_area_t virtual_work_area();

  /// Fill one wire entry from a window record. The canvas cell is left at zero:
  /// packing is a later stage and this one publishes the desktop geometry.
  LiSeamlessWindowEntry to_entry(const window_rec &rec);

  /**
   * @brief The delta between two sets.
   * @param prev the acknowledged set.
   * @param cur the current set.
   * @param mask receives the SEAMLESS_DM_* groups that changed.
   * @param changed receives the entries of the new or changed windows.
   * @return true when a delta describes the change, false when the sets differ
   *         in membership and only a snapshot can describe it.
   * @details A window that appeared or disappeared changes the set, and a delta
   *          carries no removal, so that case returns false by design.
   */
  bool build_delta(const std::vector<window_rec> &prev, const std::vector<window_rec> &cur,
                   std::uint8_t &mask, std::vector<window_rec> &changed);

  /// The publish state of one session.
  struct table_state {
    std::uint8_t seq = 0;
    bool pending = false;          ///< a version is in flight
    std::int64_t deadline_ms = 0;  ///< when to re-send the same version
    bool sent_once = false;        ///< the first publish is always a snapshot
    std::vector<window_rec> published;  ///< the set the client last acknowledged
  };

  /**
   * @brief Decide what to publish this tick.
   * @param state the session state, updated in place.
   * @param cur the current window set.
   * @param now_ms a monotonic millisecond clock.
   * @param out_kind receives 0 for nothing, 1 for a snapshot, 2 for a delta.
   * @param out_seq receives the sequence number of the published version.
   * @param out_mask receives the delta mask.
   * @param out_entries receives the entries to send.
   * @return true when something should be sent.
   * @details The sequence advances only when the previous version was
   *          acknowledged or the acknowledgement timed out, which is what stops
   *          the client from applying a table built on an unacknowledged one. A
   *          timeout re-sends the SAME version, so a lost acknowledgement costs
   *          one second and never a divergent table.
   */
  bool plan_publish(table_state &state, const std::vector<window_rec> &cur, std::int64_t now_ms,
                    int &out_kind, std::uint8_t &out_seq, std::uint8_t &out_mask,
                    std::vector<LiSeamlessWindowEntry> &out_entries);

  /**
   * @brief Apply an acknowledgement.
   * @return true when the acknowledgement matched the version in flight.
   * @details A rejected or over-limit result clears the published set, so the
   *          next version is a snapshot rather than a delta the client could not
   *          apply on top of what it has.
   */
  bool apply_ack(table_state &state, std::uint8_t seq, std::uint8_t result,
                 const std::vector<window_rec> &cur);
}  // namespace platf::seamless
