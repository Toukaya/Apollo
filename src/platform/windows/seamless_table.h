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

  /**
   * @brief The virtual desktop rectangle, the canvas a published table describes.
   * @details The origin is the virtual desktop's own top left corner, so a cell
   *          coordinate is a positive offset inside the canvas. Packing is a
   *          later stage: until it exists the canvas is the desktop rectangle
   *          itself, not a rearranged one, and it is the same rectangle for the
   *          whole session so a delta never has to restate it.
   */
  struct canvas_rect {
    std::int16_t x = 0;
    std::int16_t y = 0;
    std::uint16_t w = 0;
    std::uint16_t h = 0;
  };

  /// Read the virtual desktop rectangle of the host.
  canvas_rect virtual_canvas();

  /**
   * @brief Fill one wire entry from a window record.
   * @param canvas the virtual desktop rectangle a cell is measured from.
   * @param out receives the entry.
   * @return false when the window has no cell inside the canvas.
   * @details The cell of a window is its own rectangle, never a crop of it: the
   *          cell size equals the client size, so the texture the host copies
   *          into the cell and the rectangle the client samples cannot disagree
   *          and no window is ever drawn stretched. A window that hangs over an
   *          edge of the desktop keeps its whole rectangle and its cell is
   *          shifted to stay inside the canvas, by at most the overhang, until
   *          packing owns the layout; a window wider or taller than the canvas
   *          has no cell and is left out of the table.
   */
  bool to_entry(const window_rec &rec, const canvas_rect &canvas, LiSeamlessWindowEntry &out);

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
    std::vector<window_rec> inflight;   ///< the set sent with the version in flight
    canvas_rect canvas {};              ///< the canvas of the last sent version
    bool empty_sent = false;            ///< the empty table of a fallback has gone out
    std::vector<std::uint32_t> icons_sent;  ///< hashes whose icon already went out
  };

  /**
   * @brief Decide what to publish this tick.
   * @param state the session state, updated in place.
   * @param cur the current window set.
   * @param now_ms a monotonic millisecond clock.
   * @param canvas the virtual desktop rectangle the cells are measured from.
   * @param out_kind receives 0 for nothing, 1 for a snapshot, 2 for a delta.
   * @param out_seq receives the sequence number of the published version.
   * @param out_mask receives the delta mask.
   * @param out_entries receives the entries to send.
   * @return true when something should be sent.
   * @details The sequence advances only when the previous version was
   *          acknowledged or the acknowledgement timed out, which is what stops
   *          the client from applying a table built on an unacknowledged one. A
   *          timeout re-sends the SAME version, so a lost acknowledgement costs
   *          one second and never a divergent table. The set that goes out with
   *          the version is kept in the state, because that set, and not a set
   *          read later, is what the client's acknowledgement applies to. A
   *          canvas that differs from the last sent one forces a snapshot: a
   *          delta carries no canvas, so the cells of a delta would be read
   *          against the wrong rectangle.
   */
  bool plan_publish(table_state &state, const std::vector<window_rec> &cur, std::int64_t now_ms,
                    const canvas_rect &canvas,
                    int &out_kind, std::uint8_t &out_seq, std::uint8_t &out_mask,
                    std::vector<LiSeamlessWindowEntry> &out_entries);

  /**
   * @brief Plan one version and encode it as the payload of one message.
   * @param out_wire receives the encoded message, empty when nothing is sent.
   * @return true when a version was encoded.
   * @details This is the host publish path up to the transport: the same encoder
   *          the client uses, with the same canvas and work area the entries were
   *          measured against. A host that sends these bytes on its own session
   *          connection sends exactly what the shared sender would have sent.
   */
  bool encode_publish(table_state &state, const std::vector<window_rec> &cur, std::int64_t now_ms,
                      const canvas_rect &canvas, const work_area_t &area,
                      int &out_kind, std::uint8_t &out_seq, std::uint8_t &out_mask,
                      std::vector<std::uint8_t> &out_wire);

  /**
   * @brief Apply an acknowledgement.
   * @return true when the acknowledgement matched the version in flight.
   * @details The acknowledgement applies to the set that was sent with the
   *          version in flight, so that set becomes the diff base. A window that
   *          changed while the version was in flight is not part of the base and
   *          the next delta carries it; reading the set again here would absorb
   *          the change unsent and leave a stale cell on the client. A rejected
   *          or over-limit result clears the base, so the next version is a
   *          snapshot rather than a delta the client could not apply.
   */
  bool apply_ack(table_state &state, std::uint8_t seq, std::uint8_t result);
}  // namespace platf::seamless
