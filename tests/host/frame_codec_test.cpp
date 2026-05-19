// SPDX-License-Identifier: MIT
#include "transport/frame_codec.h"

#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

using namespace wmh::transport;

TEST(HostFrameCodec, RoundTrip) {
    const std::string p = R"({"frame":"req","id":1,"op":"session","args":{}})";
    std::vector<std::uint8_t> wire;
    ASSERT_TRUE(Encode(p, wire));
    Decoder d;
    d.Feed({wire.data(), wire.size()});
    auto out = d.TryPop();
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(*out, p);
}

TEST(HostFrameCodec, OverflowMarksError) {
    std::vector<std::uint8_t> bogus(4);
    const std::uint32_t bad = kMaxPayloadBytes + 1;
    std::memcpy(bogus.data(), &bad, sizeof(bad));
    Decoder d;
    d.Feed({bogus.data(), bogus.size()});
    EXPECT_FALSE(d.TryPop().has_value());
    EXPECT_TRUE(d.HasError());
}
