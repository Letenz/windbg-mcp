// SPDX-License-Identifier: MIT
#include "analysis/bugcheck_parser.h"

#include <gtest/gtest.h>

using nlohmann::json;
using namespace wmh::analysis::bugcheck;

constexpr const char* kSample = R"(
*******************************************************************************
*                                                                             *
*                        Bugcheck Analysis                                    *
*                                                                             *
*******************************************************************************

DRIVER_IRQL_NOT_LESS_OR_EQUAL (d1)
An attempt was made to access a pageable (or completely invalid) address at an
interrupt request level (IRQL) that is too high.  This is usually
caused by drivers using improper addresses.
If kernel debugger is available get stack backtrace.
Arguments:
Arg1: fffff80012345000, memory referenced
Arg2: 0000000000000002, IRQL
Arg3: 0000000000000001, value 0 = read operation, 1 = write operation
Arg4: fffff80012345678, address which referenced memory

Debugging Details:
------------------

FAULTING_IP:
myDriver+5678
fffff800`12345678 488b01          mov     rax,qword ptr [rcx]

CONTEXT:  fffff80b00000000 -- (.cxr 0xfffff80b00000000)
rax=0000000000000000 rbx=fffff80b00000010 rcx=fffff80012345000
rdx=0000000000000020 rsi=fffff80b00000040 rdi=fffff80b00000050
rip=fffff80012345678 rsp=fffff80b00000060 rbp=fffff80b00000070

TRAP_FRAME:  fffff80b00000080 -- (.trap 0xfffff80b00000080)

MODULE_NAME: myDriver

IMAGE_NAME:  myDriver.sys

DEBUG_FLR_IMAGE_TIMESTAMP:  61234567
)";

TEST(BugcheckParser, ParseBugcheckBasic) {
    auto bc = ParseBugcheck(kSample);
    ASSERT_TRUE(bc.is_object());
    EXPECT_EQ(bc["name"], "DRIVER_IRQL_NOT_LESS_OR_EQUAL");
    EXPECT_EQ(bc["code"], "0x000000D1");
    ASSERT_TRUE(bc["params"].is_array());
    EXPECT_EQ(bc["params"][0], "0xfffff80012345000");
    EXPECT_EQ(bc["params"][1], "0x0000000000000002");
    EXPECT_EQ(bc["params"][3], "0xfffff80012345678");
    EXPECT_FALSE(bc["summary"].is_null());
}

TEST(BugcheckParser, ParseFaultingClassicFormat) {
    auto f = ParseFaulting(kSample);
    ASSERT_TRUE(f.is_object());
    EXPECT_EQ(f["module"], "myDriver");
    EXPECT_TRUE(f["function"].is_null());
    EXPECT_EQ(f["offset"], "+5678");
    EXPECT_EQ(f["ip"], "0xfffff80012345678");
}

TEST(BugcheckParser, ParseProbableCulprit) {
    auto c = ParseProbableCulprit(kSample);
    ASSERT_TRUE(c.is_object());
    EXPECT_EQ(c["module"], "myDriver.sys");
    EXPECT_EQ(c["timestamp"], "61234567");
}

TEST(BugcheckParser, ParseContextHasRegisters) {
    auto ctx = ParseContext(kSample);
    ASSERT_TRUE(ctx.is_object());
    EXPECT_EQ(ctx["rax"], "0x0000000000000000");
    EXPECT_EQ(ctx["rip"], "0xfffff80012345678");
}

TEST(BugcheckParser, ParseIretFrame) {
    auto t = ParseIretFrame(kSample);
    ASSERT_TRUE(t.is_object());
    EXPECT_EQ(t["trap_frame"], "0xfffff80b00000080");
}

TEST(BugcheckParser, ReturnsNullOnEmpty) {
    EXPECT_TRUE(ParseBugcheck("").is_null());
    EXPECT_TRUE(ParseFaulting("nothing here").is_null());
}
