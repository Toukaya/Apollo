/**
 * @file src/platform/windows/seamless_shim.cpp
 * @brief The two symbols the shared protocol source needs from a client host.
 * @details The protocol encoders are shared with the client so the wire cannot
 *          fork, and that one source also carries the client's send path. A
 *          multi-session host never takes that path: it encodes with the
 *          encoders and sends on the session's own connection. These two
 *          definitions exist so the shared encoder can be linked here; the send
 *          path refuses loudly instead of guessing a connection.
 */
// local includes
#include "src/logging.h"

extern "C" {
#include <moonlight-common-c/src/Limelight-internal.h>
}

/// The protocol logs through this table. A host has no listener to call, so the
/// log line goes to the host log instead of nowhere.
CONNECTION_LISTENER_CALLBACKS ListenerCallbacks {};

/// Refused: the host sends on a session connection, never on a shared one.
extern "C" int sendControlMessageOnChannel(uint8_t, const void *, int) {
  BOOST_LOG(error) << "Seamless: the shared protocol send path was used on the host";
  return -1;
}
