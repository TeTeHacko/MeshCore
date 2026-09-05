#include <gtest/gtest.h>

// Pure helpers shared by the repeater channel modules (deny list, bot ignore
// list). The header is self-contained on purpose so it compiles natively.
#include "../../examples/simple_repeater/ChanListUtil.h"

// ── chan_list_has: exact, case-insensitive, never substring ──────────────────

TEST(ChanListHas, ExactMatch) {
    EXPECT_TRUE(chan_list_has("DandyPDA", "DandyPDA"));
    EXPECT_TRUE(chan_list_has("a,DandyPDA,b", "DandyPDA"));
    EXPECT_FALSE(chan_list_has("", "DandyPDA"));
    EXPECT_FALSE(chan_list_has("DandyPDA", ""));
}

TEST(ChanListHas, CaseInsensitive) {
    EXPECT_TRUE(chan_list_has("dandypda", "DandyPDA"));
    EXPECT_TRUE(chan_list_has("DANDYPDA", "dandypda"));
}

// "TTH" in the list must not silence "TTH-L1" -- the trap that made the bot's
// ignore list exact-match in the first place.
TEST(ChanListHas, NeverSubstring) {
    EXPECT_FALSE(chan_list_has("TTH", "TTH-L1"));
    EXPECT_FALSE(chan_list_has("TTH-L1", "TTH"));
}

TEST(ChanListHas, EntriesAreTrimmed) {
    EXPECT_TRUE(chan_list_has("a, DandyPDA ,b", "DandyPDA"));
}

TEST(ChanListHas, Utf8NamesMatchByteExact) {
    EXPECT_TRUE(chan_list_has("BUBEN \xF0\x9F\xA5\x81", "BUBEN \xF0\x9F\xA5\x81"));
    EXPECT_FALSE(chan_list_has("BUBEN \xF0\x9F\xA5\x81", "BUBEN"));
}

// ── chan_list_add ─────────────────────────────────────────────────────────────

TEST(ChanListAdd, AppendsWithComma) {
    char list[128] = "";
    EXPECT_EQ(chan_list_add(list, sizeof(list), "DandyPDA", 8), CHAN_LIST_OK);
    EXPECT_STREQ(list, "DandyPDA");
    EXPECT_EQ(chan_list_add(list, sizeof(list), "Foo", 3), CHAN_LIST_OK);
    EXPECT_STREQ(list, "DandyPDA,Foo");
}

TEST(ChanListAdd, TrimsSpaces) {
    char list[128] = "";
    EXPECT_EQ(chan_list_add(list, sizeof(list), "  DandyPDA  ", 12), CHAN_LIST_OK);
    EXPECT_STREQ(list, "DandyPDA");
}

TEST(ChanListAdd, NamesMayContainSpaces) {
    char list[128] = "";
    EXPECT_EQ(chan_list_add(list, sizeof(list), "Dejv Bobry mobile", 17), CHAN_LIST_OK);
    EXPECT_TRUE(chan_list_has(list, "Dejv Bobry mobile"));
}

TEST(ChanListAdd, RejectsBadNames) {
    char list[16] = "";
    EXPECT_EQ(chan_list_add(list, sizeof(list), "", 0), CHAN_LIST_BAD_NAME);
    EXPECT_EQ(chan_list_add(list, sizeof(list), "   ", 3), CHAN_LIST_BAD_NAME);
    EXPECT_EQ(chan_list_add(list, sizeof(list), "a,b", 3), CHAN_LIST_BAD_NAME);
    char longname[CHAN_NAME_MAX + 8];
    memset(longname, 'x', sizeof(longname));
    EXPECT_EQ(chan_list_add(list, sizeof(list), longname, CHAN_NAME_MAX), CHAN_LIST_BAD_NAME);
    EXPECT_STREQ(list, "");   // nothing leaked in
}

TEST(ChanListAdd, RejectsDuplicatesCaseInsensitively) {
    char list[128] = "DandyPDA";
    EXPECT_EQ(chan_list_add(list, sizeof(list), "dandypda", 8), CHAN_LIST_DUP);
    EXPECT_STREQ(list, "DandyPDA");
}

TEST(ChanListAdd, RefusesWhenFull) {
    char list[12] = "0123456789";   // 10 chars used, cap 12
    EXPECT_EQ(chan_list_add(list, sizeof(list), "ab", 2), CHAN_LIST_FULL);
    EXPECT_STREQ(list, "0123456789");
    // exactly fitting entry is fine: 10 + comma + 0 chars is the edge below
    char list2[13] = "0123456789";
    EXPECT_EQ(chan_list_add(list2, sizeof(list2), "a", 1), CHAN_LIST_OK);
    EXPECT_STREQ(list2, "0123456789,a");
}

// ── chan_list_del ─────────────────────────────────────────────────────────────

TEST(ChanListDel, RemovesMiddleEntry) {
    char list[128] = "a,DandyPDA,b";
    EXPECT_TRUE(chan_list_del(list, "DandyPDA"));
    EXPECT_STREQ(list, "a,b");
}

TEST(ChanListDel, RemovesOnlyEntry) {
    char list[128] = "DandyPDA";
    EXPECT_TRUE(chan_list_del(list, "DandyPDA"));
    EXPECT_STREQ(list, "");
}

TEST(ChanListDel, RemovesFirstAndLast) {
    char list[128] = "a,b,c";
    EXPECT_TRUE(chan_list_del(list, "a"));
    EXPECT_STREQ(list, "b,c");
    EXPECT_TRUE(chan_list_del(list, "c"));
    EXPECT_STREQ(list, "b");
}

TEST(ChanListDel, CaseInsensitive) {
    char list[128] = "DandyPDA";
    EXPECT_TRUE(chan_list_del(list, "DANDYPDA"));
    EXPECT_STREQ(list, "");
}

TEST(ChanListDel, AbsentEntryLeavesListAlone) {
    char list[128] = "a,b";
    EXPECT_FALSE(chan_list_del(list, "c"));
    EXPECT_STREQ(list, "a,b");
}

// ── round trip: what add put in, has finds and del removes ───────────────────

TEST(ChanList, RoundTrip) {
    char list[128] = "";
    ASSERT_EQ(chan_list_add(list, sizeof(list), "DandyPDA", 8), CHAN_LIST_OK);
    ASSERT_EQ(chan_list_add(list, sizeof(list), "honzaa\xF0\x9F\x98\x9B", 10), CHAN_LIST_OK);
    EXPECT_TRUE(chan_list_has(list, "DandyPDA"));
    EXPECT_TRUE(chan_list_has(list, "honzaa\xF0\x9F\x98\x9B"));
    EXPECT_TRUE(chan_list_del(list, "DandyPDA"));
    EXPECT_FALSE(chan_list_has(list, "DandyPDA"));
    EXPECT_TRUE(chan_list_has(list, "honzaa\xF0\x9F\x98\x9B"));
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
