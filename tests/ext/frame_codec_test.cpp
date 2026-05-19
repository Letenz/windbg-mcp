// SPDX-License-Identifier: MIT
#include "ipc/frame_codec.h"

#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

using windbgmcp::ipc::Decoder;
using windbgmcp::ipc::Encode;
using windbgmcp::ipc::kMaxPayloadBytes;

TEST(FrameCodec, EncodeDecodeRoundTrip) {
    const std::string payload = R"({"frame":"req","id":1,"op":"session","args":{}})";
    std::vector<std::uint8_t> wire;
    ASSERT_TRUE(Encode(payload, wire));

    Decoder dec;
    dec.Feed({wire.data(), wire.size()});
    auto out = dec.TryPop();
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(*out, payload);
    EXPECT_FALSE(dec.TryPop().has_value());
    EXPECT_FALSE(dec.HasError());
}

TEST(FrameCodec, MultipleFramesIntoOneBuffer) {
    Decoder dec;
    for (int i = 0; i < 3; ++i) {
        std::string p = "{\"frame\":\"req\",\"id\":" + std::to_string(i) + "}";
        std::vector<std::uint8_t> w;
        Encode(p, w);
        dec.Feed({w.data(), w.size()});
    }
    int popped = 0;
    while (auto v = dec.TryPop()) {
        EXPECT_NE(v->find("\"id\":" + std::to_string(popped)), std::string::npos);
        ++popped;
    }
    EXPECT_EQ(popped, 3);
}

TEST(FrameCodec, ChunkedFeed) {
    const std::string payload = "abcdefghijklmnop";
    std::vector<std::uint8_t> wire;
    Encode(payload, wire);

    Decoder dec;
    // Feed one byte at a time.
    for (auto b : wire) {
        std::uint8_t single = b;
        dec.Feed({&single, 1});
    }
    auto out = dec.TryPop();
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(*out, payload);
}

TEST(FrameCodec, OverflowMarksError) {
    // Synthesize a bogus header claiming a length above the cap.
    std::vector<std::uint8_t> bogus(4);
    const std::uint32_t bad = kMaxPayloadBytes + 1;
    std::memcpy(bogus.data(), &bad, sizeof(bad));

    Decoder dec;
    dec.Feed({bogus.data(), bogus.size()});
    EXPECT_FALSE(dec.TryPop().has_value());
    EXPECT_TRUE(dec.HasError());
}
