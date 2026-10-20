/**
 * @file src/seamless_gate.h
 * @brief The two decisions of the seamless window channel, as pure functions.
 * @details Both are free of session state on purpose: the description answer and
 *          the per-session gate are the two places this feature can be wrong,
 *          and a pure function can be exercised without a live session.
 */
#pragma once

#include <string>

#include <moonlight-common-c/src/SeamlessStream.h>

namespace seamless {
  /**
   * @brief The per-session gate.
   * @details The client's feature bit is the only thing that opens the channel
   *          for a session. A client that never sent it gets a parser that drops
   *          and logs every message.
   */
  constexpr bool session_enabled(unsigned int ml_feature_flags) {
    return (ml_feature_flags & SS_FF_SEAMLESS) != 0;
  }

  /**
   * @brief The host capability lines of the description answer.
   * @details A host can carry the channel whether or not the client in front of
   *          it asked, so these lines are a property of the host, the same way
   *          the supported encryption flags are. The client still has to offer
   *          the bit before its own session gate opens.
   */
  inline std::string capability_attributes() {
    std::string lines;
    lines += "a=x-ss-general.seamless:1\n";
    lines += "a=x-ss-general.seamless.canvasMaxW:" + std::to_string(SEAMLESS_MAX_CANVAS) + "\n";
    lines += "a=x-ss-general.seamless.canvasMaxH:" + std::to_string(SEAMLESS_MAX_CANVAS) + "\n";
    lines += "a=x-ss-general.seamless.maxWindows:" + std::to_string(SEAMLESS_MAX_WINDOWS) + "\n";
    return lines;
  }
}  // namespace seamless
