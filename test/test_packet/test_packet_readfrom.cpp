#include <gtest/gtest.h>
#include "Packet.h"
#include <cstdlib>

using namespace mesh;

// Hand readFrom a buffer allocated to EXACTLY the frame length. Under -e native
// this only checks the return value -- and the guards in readFrom deliberately do
// not change it, so these pass with or without them. The point is the ASAN build
// (-e native_asan): there the byte after the buffer is a red zone, so a read past
// the end of the frame aborts the test instead of silently returning leftovers.
static bool feedTight(Packet& p, const uint8_t* frame, uint8_t len) {
    uint8_t* tight = (uint8_t*)malloc(len ? len : 1);
    memcpy(tight, frame, len);
    bool ok = p.readFrom(tight, len);
    free(tight);
    return ok;
}

// Build a frame the parser should accept: header, path_len, then payload bytes.
static uint8_t makeFrame(uint8_t* out, uint8_t route, uint8_t type, uint8_t hash_size,
                         uint8_t hash_count, int payload_bytes) {
    uint8_t i = 0;
    out[i++] = route | (type << PH_TYPE_SHIFT);
    out[i++] = ((hash_size - 1) << 6) | (hash_count & 63);
    for (int h = 0; h < hash_count * hash_size; h++) out[i++] = 0xC0 + h;
    for (int p = 0; p < payload_bytes; p++) out[i++] = 0x40 + p;
    return i;
}

// ── the frame must be long enough for every field before it is read ──────────
//
// These all used to parse the field first and reject afterwards, so a short
// frame was read past its end. An ASAN sweep over 5 242 880 generated frames
// found four such reads; see the note in Packet::readFrom.

TEST(PacketReadFrom, RejectsFrameTooShortForHeaderAndPathLen) {
    Packet p;
    uint8_t frame[4] = {0};
    EXPECT_FALSE(feedTight(p, frame, 0));
    EXPECT_FALSE(feedTight(p, frame, 1));
}

TEST(PacketReadFrom, RejectsTruncatedTransportCodes) {
    Packet p;
    uint8_t frame[8];
    frame[0] = ROUTE_TYPE_TRANSPORT_FLOOD | (PAYLOAD_TYPE_ACK << PH_TYPE_SHIFT);
    for (int i = 1; i < 8; i++) frame[i] = 0x11;
    // needs header + 4 transport bytes + path_len + at least 1 payload byte
    for (uint8_t len = 2; len <= 6; len++) {
        EXPECT_FALSE(feedTight(p, frame, len)) << "len=" << (int)len;
    }
}

TEST(PacketReadFrom, RejectsPathOverrunningTheFrame) {
    Packet p;
    uint8_t frame[80];
    // declares 20 one-byte hashes but the frame carries only a few bytes of them
    uint8_t full = makeFrame(frame, ROUTE_TYPE_FLOOD, PAYLOAD_TYPE_ACK, 1, 20, 4);
    for (uint8_t len = 2; len < full - 4; len++) {
        EXPECT_FALSE(feedTight(p, frame, len)) << "len=" << (int)len;
    }
}

TEST(PacketReadFrom, RejectsPathWithNoRoomLeftForPayload) {
    Packet p;
    uint8_t frame[80];
    uint8_t full = makeFrame(frame, ROUTE_TYPE_FLOOD, PAYLOAD_TYPE_ACK, 1, 8, 0);
    EXPECT_FALSE(feedTight(p, frame, full));   // path fits exactly, payload is empty
}

// ── control group: the guards must not reject what used to parse ─────────────

TEST(PacketReadFrom, AcceptsWellFormedFrame) {
    Packet p;
    uint8_t frame[80];
    uint8_t len = makeFrame(frame, ROUTE_TYPE_FLOOD, PAYLOAD_TYPE_ACK, 1, 3, 5);

    ASSERT_TRUE(feedTight(p, frame, len));
    EXPECT_EQ(3, p.getPathHashCount());
    EXPECT_EQ(1, p.getPathHashSize());
    EXPECT_EQ(5, p.payload_len);
    EXPECT_EQ(0x40, p.payload[0]);
    EXPECT_EQ(0xC0, p.path[0]);
}

TEST(PacketReadFrom, AcceptsWellFormedFrameWithTransportCodes) {
    Packet p;
    uint8_t frame[80];
    uint8_t i = 0;
    frame[i++] = ROUTE_TYPE_TRANSPORT_DIRECT | (PAYLOAD_TYPE_ACK << PH_TYPE_SHIFT);
    frame[i++] = 0x34; frame[i++] = 0x12;      // transport_codes[0]
    frame[i++] = 0x78; frame[i++] = 0x56;      // transport_codes[1]
    frame[i++] = 0;                            // path_len: no hops
    frame[i++] = 0x99;                         // one payload byte

    ASSERT_TRUE(feedTight(p, frame, i));
    EXPECT_EQ(0x1234, p.transport_codes[0]);
    EXPECT_EQ(0x5678, p.transport_codes[1]);
    EXPECT_EQ(1, p.payload_len);
    EXPECT_EQ(0x99, p.payload[0]);
}

TEST(PacketReadFrom, RoundTripsThroughWriteTo) {
    Packet out;
    out.header = ROUTE_TYPE_FLOOD | (PAYLOAD_TYPE_TXT_MSG << PH_TYPE_SHIFT);
    out.setPathHashSizeAndCount(2, 3);
    for (int i = 0; i < 6; i++) out.path[i] = 0xA0 + i;
    out.payload_len = 10;
    for (int i = 0; i < 10; i++) out.payload[i] = 0x50 + i;

    uint8_t wire[MAX_TRANS_UNIT];
    uint8_t len = out.writeTo(wire);

    Packet in;
    ASSERT_TRUE(feedTight(in, wire, len));
    EXPECT_EQ(out.header, in.header);
    EXPECT_EQ(out.path_len, in.path_len);
    EXPECT_EQ(out.payload_len, in.payload_len);
    EXPECT_EQ(0, memcmp(out.path, in.path, out.getPathByteLen()));
    EXPECT_EQ(0, memcmp(out.payload, in.payload, out.payload_len));
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
