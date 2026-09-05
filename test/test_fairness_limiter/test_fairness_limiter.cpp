#include <gtest/gtest.h>
#include "helpers/FairnessLimiter.h"

using namespace mesh;

// PAYLOAD_VER_1 = 0, so a plain route|type header is already version 1.
static Packet makePacket(uint8_t route, uint8_t type, std::initializer_list<uint8_t> payload) {
    Packet p;
    p.header = route | (type << PH_TYPE_SHIFT);
    p.payload_len = 0;
    for (uint8_t b : payload) p.payload[p.payload_len++] = b;
    p.path_len = 0;
    return p;
}

// ── classification ───────────────────────────────────────────────────────────

TEST(FairnessLimiter, DirectIsNeverLimited) {
    FairnessLimiter fl;   // zero tokens everywhere (no refill yet)
    Packet p = makePacket(ROUTE_TYPE_DIRECT, PAYLOAD_TYPE_TXT_MSG, {0x10, 0x22});
    for (int i = 0; i < 100; i++) EXPECT_TRUE(fl.allowPacket(&p));
    EXPECT_EQ(fl.deniedSenderNormal(), 0u);
}

TEST(FairnessLimiter, UnclassifiedTypesAllowedThrough) {
    FairnessLimiter fl;   // zero tokens: if these were classified, they'd deny
    Packet ack = makePacket(ROUTE_TYPE_FLOOD, PAYLOAD_TYPE_ACK, {1, 2, 3, 4});
    EXPECT_TRUE(fl.allowPacket(&ack));
    EXPECT_EQ(fl.deniedGroup() + fl.deniedSenderNormal() + fl.deniedSenderLow(), 0u);
}

TEST(FairnessLimiter, MalformedShortPayloadAllowedThrough) {
    FairnessLimiter fl;
    Packet grp = makePacket(ROUTE_TYPE_FLOOD, PAYLOAD_TYPE_GRP_TXT, {});
    EXPECT_TRUE(fl.allowPacket(&grp));                 // no payload[0] to read
    Packet txt = makePacket(ROUTE_TYPE_FLOOD, PAYLOAD_TYPE_TXT_MSG, {0x10});
    EXPECT_TRUE(fl.allowPacket(&txt));                 // no payload[1] to read
    EXPECT_EQ(fl.deniedGroup() + fl.deniedSenderNormal(), 0u);
}

// ── group bucket (GRP_TXT keyed by payload[0] = channel hash) ────────────────

TEST(FairnessLimiter, GroupBucketDrainsThenDenies) {
    FairnessLimiter fl;
    fl.refillGroup();   // 1 token per bucket
    Packet p = makePacket(ROUTE_TYPE_FLOOD, PAYLOAD_TYPE_GRP_TXT, {0xEF, 0, 0});
    EXPECT_TRUE(fl.allowPacket(&p));    // consumes the single token
    EXPECT_FALSE(fl.allowPacket(&p));   // bucket empty
    EXPECT_EQ(fl.deniedGroup(), 1u);
}

TEST(FairnessLimiter, GroupRefillRestoresOneToken) {
    FairnessLimiter fl;
    fl.refillGroup();
    Packet p = makePacket(ROUTE_TYPE_FLOOD, PAYLOAD_TYPE_GRP_TXT, {0xEF, 0, 0});
    EXPECT_TRUE(fl.allowPacket(&p));
    EXPECT_FALSE(fl.allowPacket(&p));
    fl.refillGroup();
    EXPECT_TRUE(fl.allowPacket(&p));
    EXPECT_FALSE(fl.allowPacket(&p));
}

TEST(FairnessLimiter, GroupCapBoundsTheBurst) {
    FairnessLimiter fl;
    for (int i = 0; i < 10 * FAIRNESS_GROUP_CAP; i++) fl.refillGroup();
    Packet p = makePacket(ROUTE_TYPE_FLOOD, PAYLOAD_TYPE_GRP_TXT, {0xEF, 0, 0});
    int allowed = 0;
    for (int i = 0; i < 10 * FAIRNESS_GROUP_CAP; i++) {
        if (fl.allowPacket(&p)) allowed++;
    }
    EXPECT_EQ(allowed, FAIRNESS_GROUP_CAP);
}

// The masks make collisions expected: 0xEF and 0xEF^0x20 share a 5-bit bucket,
// while 0xEE does not. This documents the approximation, it is not a bug.
TEST(FairnessLimiter, GroupBucketsCollideOnMask) {
    FairnessLimiter fl;
    fl.refillGroup();
    Packet a = makePacket(ROUTE_TYPE_FLOOD, PAYLOAD_TYPE_GRP_TXT, {0xEF, 0, 0});
    Packet b = makePacket(ROUTE_TYPE_FLOOD, PAYLOAD_TYPE_GRP_TXT, {(uint8_t)(0xEF ^ 0x20), 0, 0});
    Packet c = makePacket(ROUTE_TYPE_FLOOD, PAYLOAD_TYPE_GRP_TXT, {0xEE, 0, 0});
    EXPECT_TRUE(fl.allowPacket(&a));
    EXPECT_FALSE(fl.allowPacket(&b));   // same bucket as a
    EXPECT_TRUE(fl.allowPacket(&c));    // its own bucket
}

// ── sender-normal bucket (TXT_MSG etc. keyed by payload[1] = 1 B src hash) ───

TEST(FairnessLimiter, SenderBucketKeysOnSrcHash) {
    FairnessLimiter fl;
    fl.refillSenderNormal();
    // dest 0x10 src 0x22 vs dest 0x99 src 0x22: SAME sender, same bucket
    Packet a = makePacket(ROUTE_TYPE_FLOOD, PAYLOAD_TYPE_TXT_MSG, {0x10, 0x22, 0, 0});
    Packet b = makePacket(ROUTE_TYPE_FLOOD, PAYLOAD_TYPE_TXT_MSG, {0x99, 0x22, 0, 0});
    EXPECT_TRUE(fl.allowPacket(&a));
    EXPECT_FALSE(fl.allowPacket(&b));
    EXPECT_EQ(fl.deniedSenderNormal(), 1u);
}

TEST(FairnessLimiter, ReservedSenderHashesAlwaysDeny) {
    FairnessLimiter fl;
    for (int i = 0; i < 5; i++) fl.refillSenderNormal();
    Packet z = makePacket(ROUTE_TYPE_FLOOD, PAYLOAD_TYPE_TXT_MSG, {0x10, 0x00, 0, 0});
    Packet f = makePacket(ROUTE_TYPE_FLOOD, PAYLOAD_TYPE_TXT_MSG, {0x10, 0xFF, 0, 0});
    EXPECT_FALSE(fl.allowPacket(&z));   // 0x00 reserved (Identity.cpp:56)
    EXPECT_FALSE(fl.allowPacket(&f));   // 0xFF reserved
}

// ── advert bucket (keyed by payload[0] = pubkey first byte) ──────────────────

TEST(FairnessLimiter, AdvertBucketIsSeparateFromGroupBucket) {
    FairnessLimiter fl;
    fl.refillSenderLow();
    // no group refill: a GRP_TXT with the same first byte must NOT be allowed
    // via the advert bucket, and the advert must not touch the group counter.
    Packet adv = makePacket(ROUTE_TYPE_FLOOD, PAYLOAD_TYPE_ADVERT, {0x42, 0, 0});
    Packet grp = makePacket(ROUTE_TYPE_FLOOD, PAYLOAD_TYPE_GRP_TXT, {0x42, 0, 0});
    EXPECT_TRUE(fl.allowPacket(&adv));
    EXPECT_FALSE(fl.allowPacket(&grp));
    EXPECT_EQ(fl.deniedSenderLow(), 0u);
    EXPECT_EQ(fl.deniedGroup(), 1u);
}

TEST(FairnessLimiter, AdvertCapIsLow) {
    FairnessLimiter fl;
    for (int i = 0; i < 100; i++) fl.refillSenderLow();
    Packet adv = makePacket(ROUTE_TYPE_FLOOD, PAYLOAD_TYPE_ADVERT, {0x42, 0, 0});
    int allowed = 0;
    for (int i = 0; i < 100; i++) {
        if (fl.allowPacket(&adv)) allowed++;
    }
    EXPECT_EQ(allowed, FAIRNESS_SENDER_LOW_CAP);
}

// ── transport-flood route is a flood too ─────────────────────────────────────

TEST(FairnessLimiter, TransportFloodIsLimitedLikeFlood) {
    FairnessLimiter fl;   // zero tokens
    Packet p = makePacket(ROUTE_TYPE_TRANSPORT_FLOOD, PAYLOAD_TYPE_GRP_TXT, {0xEF, 0, 0});
    EXPECT_FALSE(fl.allowPacket(&p));
    EXPECT_EQ(fl.deniedGroup(), 1u);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
