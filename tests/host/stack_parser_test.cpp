// SPDX-License-Identifier: MIT
#include "analysis/stack_parser.h"

#include <gtest/gtest.h>

using namespace wmh::analysis::stack;

constexpr const char* kKb = R"(# Child-SP          RetAddr           : Args to Child                                                           : Call Site
00 fffff80b`00000060 fffff800`12345abc : 0000000000000000 0000000000000000 0000000000000000 0000000000000000 : nt!KeBugCheckEx+0x107
01 fffff80b`00000080 fffff800`12345def : 0000000000000000 0000000000000000 0000000000000000 0000000000000000 : nt!KiBugCheckDispatch+0x69
02 fffff80b`000000c0 0000000000000000  : 0000000000000000 0000000000000000 0000000000000000 0000000000000000 : myDriver+0x5678
)";

TEST(StackParser, ThreeFrames) {
    auto frames = ParseKb(kKb);
    ASSERT_EQ(frames.size(), 3u);

    EXPECT_EQ(frames[0]["module"], "nt");
    EXPECT_EQ(frames[0]["func"],   "KeBugCheckEx");
    EXPECT_EQ(frames[0]["offset"], "+0x107");

    EXPECT_EQ(frames[1]["module"], "nt");
    EXPECT_EQ(frames[1]["func"],   "KiBugCheckDispatch");
    EXPECT_EQ(frames[1]["offset"], "+0x69");

    EXPECT_EQ(frames[2]["module"], "myDriver");
    EXPECT_TRUE(frames[2]["func"].is_null());
    EXPECT_EQ(frames[2]["offset"], "+0x5678");
}
