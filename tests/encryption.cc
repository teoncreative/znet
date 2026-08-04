//
//    Copyright 2026 Metehan Gezer
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//

//
// Session-crypto tests over a fake transport, so encrypted frames can be held,
// reordered, replayed and edited by hand. Two real PeerSessions run the real
// handshake; only the wire between them is under the test's control, which is
// the seam a socket-level test does not offer.
//

#include "znet/codec.h"
#include "znet/encryption.h"
#include "znet/init.h"
#include "znet/packet.h"
#include "znet/packet_handler.h"
#include "znet/peer_session.h"
#include "znet/transport.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <deque>
#include <memory>
#include <string>
#include <vector>

using namespace znet;

#include "session_pair.h"

TEST(SessionCrypto, RoundTripsOverTheFakeWire) {
  ASSERT_EQ(Init(), Result::Success);
  Pair pair;
  ASSERT_TRUE(pair.Handshake());

  for (uint32_t i = 0; i < 10; i++) {
    pair.Deliver(pair.Emit(i, 0));
  }
  ASSERT_EQ(pair.server_got.size(), 10u);
  for (uint32_t i = 0; i < 10; i++) {
    EXPECT_EQ(pair.server_got[i], i);
  }
}

// The regression this file exists for. Channels are ordered independently, so
// one can run far ahead while another waits on a retransmit. With a single
// sequence and a single replay window shared across channels, the stalled
// channel's backlog lands outside the window and is refused as too old --
// silent data loss on a channel the caller was promised was reliable.
TEST(SessionCrypto, ChannelRunningAheadDoesNotStarveAStalledChannel) {
  ASSERT_EQ(Init(), Result::Success);
  Pair pair;
  ASSERT_TRUE(pair.Handshake());

  // channel 0 stalls: its frames are held rather than delivered
  const uint32_t kHeld = 200;  // far more than one replay window
  std::vector<FakeTransport::Frame> held;
  for (uint32_t i = 0; i < kHeld; i++) {
    held.push_back(pair.Emit(i, 0));
  }
  // meanwhile channel 1 keeps flowing
  for (uint32_t i = 0; i < 5; i++) {
    pair.Deliver(pair.Emit(1000 + i, 1));
  }
  ASSERT_EQ(pair.server_got.size(), 5u) << "channel 1 should have gone through";

  // the stall clears and channel 0's backlog arrives, in its own order
  for (const auto& frame : held) {
    pair.Deliver(frame);
  }

  ASSERT_EQ(pair.server_got.size(), 5u + kHeld)
      << "every held message must still be delivered; a shared replay window "
         "would refuse the ones more than 64 counters behind";
  for (uint32_t i = 0; i < kHeld; i++) {
    EXPECT_EQ(pair.server_got[5 + i], i) << "held message " << i;
  }
}

TEST(SessionCrypto, ChannelsKeepIndependentSequences) {
  ASSERT_EQ(Init(), Result::Success);
  Pair pair;
  ASSERT_TRUE(pair.Handshake());

  // interleaving two channels must not make either look like a replay
  for (uint32_t i = 0; i < 50; i++) {
    pair.Deliver(pair.Emit(i, 0));
    pair.Deliver(pair.Emit(100 + i, 7));
  }
  EXPECT_EQ(pair.server_got.size(), 100u);
}

TEST(SessionCrypto, ReplayedFrameIsRefused) {
  ASSERT_EQ(Init(), Result::Success);
  Pair pair;
  ASSERT_TRUE(pair.Handshake());

  FakeTransport::Frame frame = pair.Emit(42, 0);
  FakeTransport::Frame captured = Pair::Snapshot(frame);
  pair.Deliver(frame);
  ASSERT_EQ(pair.server_got.size(), 1u);

  // the same bytes again, as an attacker who captured them would send
  pair.Deliver(captured);
  EXPECT_EQ(pair.server_got.size(), 1u)
      << "a captured frame replayed verbatim must not be delivered twice";
}

TEST(SessionCrypto, TamperedCiphertextIsRefused) {
  ASSERT_EQ(Init(), Result::Success);
  Pair pair;
  ASSERT_TRUE(pair.Handshake());

  FakeTransport::Frame frame = pair.Emit(7, 0);
  // flip a bit in the body; with an unauthenticated cipher this would decrypt
  // to altered plaintext and be handed to the application
  ASSERT_GT(frame.buffer->readable_bytes(), 12u);
  // the frame starts at the read cursor; in front of it is unspent headroom
  char* bytes = const_cast<char*>(frame.buffer->read_cursor_data());
  bytes[frame.buffer->readable_bytes() / 2] ^= 0x01;

  pair.Deliver(frame);
  EXPECT_TRUE(pair.server_got.empty())
      << "an altered frame must fail its tag and be dropped";
}

TEST(SessionCrypto, TamperedChannelByteIsRefused) {
  ASSERT_EQ(Init(), Result::Success);
  Pair pair;
  ASSERT_TRUE(pair.Handshake());

  FakeTransport::Frame frame = pair.Emit(7, 0);
  // the channel is not in the AEAD's associated data, it is in the nonce, so
  // moving a frame to another channel must still fail
  char* bytes = const_cast<char*>(frame.buffer->read_cursor_data());
  bytes[1] ^= 0x03;  // [0] is the mode byte, [1] the channel

  pair.Deliver(frame);
  EXPECT_TRUE(pair.server_got.empty())
      << "a frame re-labelled onto another channel derives a different nonce "
         "and must fail its tag";
}

TEST(SessionCrypto, UnencryptedSessionStillDelivers) {
  ASSERT_EQ(Init(), Result::Success);
  Pair pair(/*encryption=*/false);
  ASSERT_TRUE(pair.Handshake());

  for (uint32_t i = 0; i < 5; i++) {
    pair.Deliver(pair.Emit(i, 0));
  }
  EXPECT_EQ(pair.server_got.size(), 5u);
}

// --- Keying material export ---------------------------------------------------
//
// The exchange is anonymous, so a credential sent over it proves only that
// somebody holds it. An export is what an application binds one to: both ends of
// a session derive it, nobody else can, and it is different on every session.
// That last part is what an interceptor cannot get around, since it runs two
// separate exchanges and holds two different values.

namespace {

std::vector<unsigned char> Export(const PeerSession& session,
                                  const std::string& label, size_t len = 32) {
  std::vector<unsigned char> out(len, 0);
  EXPECT_EQ(session.ExportKeyingMaterial(label, out.data(), out.size()),
            Result::Success);
  return out;
}

}  // namespace

TEST(SessionExport, BothEndsDeriveTheSameValue) {
  ASSERT_EQ(Init(), Result::Success);
  Pair pair;
  ASSERT_TRUE(pair.Handshake());

  auto from_client = Export(*pair.client, "auth v1");
  auto from_server = Export(*pair.server, "auth v1");

  EXPECT_EQ(from_client, from_server);
  // a failed derive that still reported success would leave the buffer zeroed
  EXPECT_NE(from_client, std::vector<unsigned char>(32, 0));
}

TEST(SessionExport, IsStableAcrossCalls) {
  ASSERT_EQ(Init(), Result::Success);
  Pair pair;
  ASSERT_TRUE(pair.Handshake());

  EXPECT_EQ(Export(*pair.client, "auth v1"), Export(*pair.client, "auth v1"));
}

TEST(SessionExport, DiffersByLabel) {
  ASSERT_EQ(Init(), Result::Success);
  Pair pair;
  ASSERT_TRUE(pair.Handshake());

  EXPECT_NE(Export(*pair.client, "auth v1"), Export(*pair.client, "auth v2"));
}

// The property the whole thing rests on. Two sessions are two exchanges, so a
// proof built on one is worthless on the other, which is exactly what an
// interceptor is holding: its session with the client is not its session with
// the server.
TEST(SessionExport, DiffersBetweenSessions) {
  ASSERT_EQ(Init(), Result::Success);
  Pair first;
  Pair second;
  ASSERT_TRUE(first.Handshake());
  ASSERT_TRUE(second.Handshake());

  EXPECT_NE(Export(*first.client, "auth v1"), Export(*second.client, "auth v1"));
  // and both ends of each still agree, so the difference is the session and not
  // some per-end divergence
  EXPECT_EQ(Export(*second.client, "auth v1"), Export(*second.server, "auth v1"));
}

TEST(SessionExport, LengthIsHonored) {
  ASSERT_EQ(Init(), Result::Success);
  Pair pair;
  ASSERT_TRUE(pair.Handshake());

  auto short_export = Export(*pair.client, "auth v1", 16);
  auto long_export = Export(*pair.client, "auth v1", 64);
  ASSERT_EQ(short_export.size(), 16u);
  ASSERT_EQ(long_export.size(), 64u);
  // HKDF is a stream, so a shorter ask is a prefix of a longer one
  EXPECT_TRUE(std::equal(short_export.begin(), short_export.end(),
                         long_export.begin()));
}

TEST(SessionExport, RefusedBeforeTheHandshakeCompletes) {
  ASSERT_EQ(Init(), Result::Success);
  Pair pair;  // deliberately not handshaked

  unsigned char out[32] = {};
  EXPECT_EQ(pair.client->ExportKeyingMaterial("auth v1", out, sizeof(out)),
            Result::Failure);
}

TEST(SessionExport, RefusedOnAnUnencryptedSession) {
  ASSERT_EQ(Init(), Result::Success);
  Pair pair(/*encryption=*/false);
  ASSERT_TRUE(pair.Handshake());

  unsigned char out[32] = {};
  EXPECT_EQ(pair.client->ExportKeyingMaterial("auth v1", out, sizeof(out)),
            Result::Failure)
      << "there is no exchange to bind to, so this must not hand back bytes";
}

TEST(SessionExport, RejectsOutOfRangeRequests) {
  ASSERT_EQ(Init(), Result::Success);
  Pair pair;
  ASSERT_TRUE(pair.Handshake());

  unsigned char out[32] = {};
  EXPECT_EQ(pair.client->ExportKeyingMaterial("", out, sizeof(out)),
            Result::InvalidArgument);
  EXPECT_EQ(pair.client->ExportKeyingMaterial(
                std::string(EncryptionLayer::kMaxExportLabelLength + 1, 'x'),
                out, sizeof(out)),
            Result::InvalidArgument);
  EXPECT_EQ(pair.client->ExportKeyingMaterial("auth v1", out, 0),
            Result::InvalidArgument);
  EXPECT_EQ(pair.client->ExportKeyingMaterial("auth v1", nullptr, 32),
            Result::InvalidArgument);

  std::vector<unsigned char> huge(EncryptionLayer::kMaxExportLength + 1, 0);
  EXPECT_EQ(
      pair.client->ExportKeyingMaterial("auth v1", huge.data(), huge.size()),
      Result::InvalidArgument);
}
