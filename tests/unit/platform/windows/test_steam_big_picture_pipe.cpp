/**
 * @file tests/unit/platform/windows/test_steam_big_picture_pipe.cpp
 * @brief Framed JSON messages over a real overlapped named pipe, both directions.
 */
#include "src/platform/windows/steam_big_picture_pipe.h"

#include <gtest/gtest.h>
#include <string>

namespace {
  using namespace std::chrono_literals;
  using platf::steam::big_picture::framed_pipe_t;
  namespace protocol = platf::steam::big_picture::protocol;

  class SteamBigPicturePipe: public ::testing::Test {
  protected:
    void SetUp() override {
      const auto name = L"\\\\.\\pipe\\vibeshine_steam_ui_test_" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount64());
      server = CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 64 * 1024, 64 * 1024, 0, nullptr);
      ASSERT_NE(server, INVALID_HANDLE_VALUE);
      client = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
      ASSERT_NE(client, INVALID_HANDLE_VALUE);
      OVERLAPPED connection {};
      connection.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
      const bool connected = ConnectNamedPipe(server, &connection) || GetLastError() == ERROR_PIPE_CONNECTED;
      CloseHandle(connection.hEvent);
      ASSERT_TRUE(connected);
    }

    void TearDown() override {
      if (client != INVALID_HANDLE_VALUE) {
        CloseHandle(client);
      }
      if (server != INVALID_HANDLE_VALUE) {
        CloseHandle(server);
      }
    }

    // Write raw bytes, bypassing the framing, to send malformed frames.
    void write_raw(HANDLE pipe, const std::string &bytes) {
      OVERLAPPED overlapped {};
      overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
      DWORD written = 0;
      if (!WriteFile(pipe, bytes.data(), static_cast<DWORD>(bytes.size()), &written, &overlapped)) {
        ASSERT_EQ(GetLastError(), static_cast<DWORD>(ERROR_IO_PENDING));
        ASSERT_TRUE(GetOverlappedResult(pipe, &overlapped, &written, TRUE));
      }
      CloseHandle(overlapped.hEvent);
    }

    HANDLE server = INVALID_HANDLE_VALUE;
    HANDLE client = INVALID_HANDLE_VALUE;
  };
}  // namespace

TEST_F(SteamBigPicturePipe, CarriesMessagesBothWays) {
  framed_pipe_t host(server);
  framed_pipe_t helper(client);
  ASSERT_TRUE(helper.write({{"type", "hello"}, {"protocol", 1}, {"pid", 42}}));
  ASSERT_TRUE(host.write({{"type", "status"}, {"status", {{"appRunning", true}}}}));

  const auto hello = host.read(2s);
  ASSERT_TRUE(hello.has_value());
  EXPECT_EQ((*hello)["pid"], 42);
  const auto status = helper.read(2s);
  ASSERT_TRUE(status.has_value());
  EXPECT_EQ((*status)["status"]["appRunning"], true);
}

TEST_F(SteamBigPicturePipe, ReassemblesLargeAndBackToBackMessages) {
  framed_pipe_t host(server);
  framed_pipe_t helper(client);
  const std::string script(50 * 1024, 'x');  // larger than one read chunk
  ASSERT_TRUE(host.write({{"type", "config"}, {"script", script}}));
  for (int i = 0; i < 20; ++i) {
    ASSERT_TRUE(host.write({{"type", "status"}, {"n", i}}));
  }
  const auto config = helper.read(2s);
  ASSERT_TRUE(config.has_value());
  EXPECT_EQ((*config)["script"].get<std::string>().size(), script.size());
  for (int i = 0; i < 20; ++i) {
    const auto status = helper.read(2s);
    ASSERT_TRUE(status.has_value());
    EXPECT_EQ((*status)["n"], i);
  }
}

TEST_F(SteamBigPicturePipe, TimesOutWithoutClosing) {
  framed_pipe_t host(server);
  EXPECT_FALSE(host.read(50ms).has_value());
  EXPECT_FALSE(host.closed());
}

TEST_F(SteamBigPicturePipe, ClosesOnAnOversizedFrame) {
  framed_pipe_t host(server);
  write_raw(client, std::string("\xFF\xFF\xFF\x00", 4));
  EXPECT_FALSE(host.read(2s).has_value());
  EXPECT_TRUE(host.closed());
}

TEST_F(SteamBigPicturePipe, ClosesOnAFrameThatIsNotAnObject) {
  framed_pipe_t host(server);
  write_raw(client, *protocol::encode_frame("[1,2,3]"));
  EXPECT_FALSE(host.read(2s).has_value());
  EXPECT_TRUE(host.closed());
}

TEST_F(SteamBigPicturePipe, ClosesWhenThePeerGoesAway) {
  framed_pipe_t host(server);
  CloseHandle(client);
  client = INVALID_HANDLE_VALUE;
  EXPECT_FALSE(host.read(2s).has_value());
  EXPECT_TRUE(host.closed());
  EXPECT_FALSE(host.write({{"type", "status"}}));
}
