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

TEST(StackParser, KbWithoutChildSpAndTickSeparatedArguments) {
    const auto frames = ParseKb(
        "0b fffff802`79605020 : ffffa10c`2c983e30 ffffa10c`32147000 00000000`00000000 ffffffff`800028a0 : HelloWorld!DriverEntry+0x2c [C:\\lab\\HelloWorld.c @ 26]\n");
    ASSERT_EQ(frames.size(), 1u);
    EXPECT_EQ(frames[0]["frame"], 11);
    EXPECT_EQ(frames[0]["addr"], "0xfffff80279605020");
    EXPECT_EQ(frames[0]["module"], "HelloWorld");
    EXPECT_EQ(frames[0]["offset"], "+0x2c");
    EXPECT_EQ(frames[0]["source"]["line"], 26);
}

TEST(StackParser, AnalyzeStackWithoutFrameNumbers) {
    const auto frames = ParseKb(
        "STACK_TEXT:\n"
        "ffffb182`751418e0 fffff802`79605020 : ffffa10c`2c983e30 ffffa10c`32147000 00000000`00000000 ffffffff`800028a0 : HelloWorld!DriverEntry+0x2c [C:\\lab\\HelloWorld.c @ 26]\n"
        "ffffb182`75141920 fffff802`737107c6 : 00000000`00000000 00000000`00000000 00000000`00000000 00000000`00000000 : nt!IopLoadDriver+0x4c2\n");
    ASSERT_EQ(frames.size(), 2u);
    EXPECT_EQ(frames[0]["func"], "DriverEntry");
    EXPECT_EQ(frames[0]["source"]["file"], "C:\\lab\\HelloWorld.c");
    EXPECT_EQ(frames[1]["frame"], 1);
}
