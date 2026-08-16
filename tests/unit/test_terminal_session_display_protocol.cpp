#include "../tests_common.h"
#include "src/terminal_session_display_protocol.h"

#include <array>

TEST(TerminalSessionDisplayProtocol, CommonEnvelopeChecksApplyToEveryOperation) {
  using namespace terminal_session::display;
  for (const auto op : {operation::query, operation::set_mode, operation::set_hdr,
                        operation::seal_snapshot, operation::verify_snapshot}) {
    request_t request {
      .operation = static_cast<std::uint8_t>(op),
      .generation = 7,
      .request_id = 1,
    };
    if (op == operation::set_mode) {
      request.width = 1920;
      request.height = 1080;
      request.refresh_rate_millihz = 60'000;
    } else if (op == operation::set_hdr) {
      request.hdr_enabled = 1;
    } else if (op == operation::seal_snapshot) {
      request.snapshot_tier = 1;
      request.snapshot_digest[0] = 1;
    } else if (op == operation::verify_snapshot) {
      request.snapshot_tier = 1;
      request.snapshot_sequence = 1;
      request.snapshot_display_id = 42;
      request.snapshot_digest[0] = 1;
      request.snapshot_tag[0] = 2;
    }
    ASSERT_TRUE(valid_request(request));
    request.magic = 0;
    EXPECT_FALSE(valid_request(request));
    request.magic = magic;
    request.version = 0;
    EXPECT_FALSE(valid_request(request));
    request.version = version;
    request.flags = 1;
    EXPECT_FALSE(valid_request(request));
  }
}

TEST(TerminalSessionDisplayProtocol, UnknownAndOversizedFramesAreRejected) {
  using namespace terminal_session::display;
  request_t request {.operation = 0, .generation = 1, .request_id = 1};
  EXPECT_FALSE(valid_request(request));
  request.operation = 6;
  EXPECT_FALSE(valid_request(request));
  EXPECT_FALSE(decode(nullptr, sizeof(request), request));
  std::array<std::uint8_t, max_message_size + 1> oversized {};
  EXPECT_FALSE(decode(oversized.data(), oversized.size(), request));
}

TEST(TerminalSessionDisplayProtocol, QueryResponseCarriesBoundedBrokerState) {
  using namespace terminal_session::display;
  static_assert(sizeof(response_t) == 96);
  response_t response {};
  response.operation = static_cast<std::uint8_t>(operation::query);
  response.generation = 7;
  response.request_id = 1;
  response.display_id = 42;
  response.hdr_enabled = 1;
  response.snapshot_sequence = 9;
  response.snapshot_tag[0] = 0xA5;
  EXPECT_TRUE(valid_response(response));
  EXPECT_EQ(response.snapshot_tag.size(), 32u);
}

TEST(TerminalSessionDisplayProtocol, SnapshotSealAndVerifyFieldsAreStrictlyBounded) {
  using namespace terminal_session::display;
  static_assert(sizeof(request_t) == 128);
  request_t seal {
    .operation = static_cast<std::uint8_t>(operation::seal_snapshot),
    .generation = 7,
    .request_id = 1,
    .snapshot_tier = 1,
  };
  seal.snapshot_digest[0] = 1;
  EXPECT_TRUE(valid_request(seal));
  seal.snapshot_sequence = 1;
  EXPECT_FALSE(valid_request(seal));

  request_t verify {
    .operation = static_cast<std::uint8_t>(operation::verify_snapshot),
    .generation = 7,
    .request_id = 2,
    .snapshot_tier = 1,
    .snapshot_sequence = 3,
    .snapshot_display_id = 42,
  };
  verify.snapshot_digest[0] = 1;
  verify.snapshot_tag[0] = 2;
  EXPECT_TRUE(valid_request(verify));
  verify.snapshot_tier = 3;
  EXPECT_FALSE(valid_request(verify));
}
