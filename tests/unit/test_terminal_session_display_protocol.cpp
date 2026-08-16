#include "../tests_common.h"
#include "src/terminal_session_display_protocol.h"

#include <array>

TEST(TerminalSessionDisplayProtocol, CommonEnvelopeChecksApplyToEveryOperation) {
  using namespace terminal_session::display;
  for (const auto op : {operation::query, operation::set_mode, operation::set_hdr}) {
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
  EXPECT_FALSE(decode(nullptr, sizeof(request), request));
  std::array<std::uint8_t, max_message_size + 1> oversized {};
  EXPECT_FALSE(decode(oversized.data(), oversized.size(), request));
}
