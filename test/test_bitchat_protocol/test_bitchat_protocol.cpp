#include <gtest/gtest.h>

#include <cstring>
#include <vector>

extern "C" {
#include <helpers/miniz/miniz_tinfl.c>
}
#include <helpers/bitchat/BitchatProtocol.cpp>

TEST(BitchatProtocol, SerializeParseV1RoundTrip) {
  BitchatMessage tx;
  tx.version = BITCHAT_VERSION;
  tx.type = BITCHAT_MSG_MESSAGE;
  tx.ttl = 7;
  tx.timestamp = 1767000000123ULL;
  tx.flags = BITCHAT_FLAG_HAS_SIGNATURE;
  tx.setSenderId64(0x0102030405060708ULL);
  const char* text = "hello mesh";
  memcpy(tx.payload, text, strlen(text));
  tx.payloadLength = static_cast<uint16_t>(strlen(text));
  memset(tx.signature, 0xA5, BITCHAT_SIGNATURE_SIZE);

  uint8_t wire[512];
  size_t n = BitchatProtocol::serializeMessage(tx, wire, sizeof(wire));
  ASSERT_GT(n, 0u);
  EXPECT_EQ(static_cast<int>(n), BitchatProtocol::frameLength(wire, n));

  BitchatMessage rx;
  ASSERT_TRUE(BitchatProtocol::parseMessage(wire, n, rx));
  EXPECT_TRUE(BitchatProtocol::validateMessage(rx));
  EXPECT_EQ(tx.version, rx.version);
  EXPECT_EQ(tx.type, rx.type);
  EXPECT_EQ(tx.ttl, rx.ttl);
  EXPECT_EQ(tx.timestamp, rx.timestamp);
  EXPECT_EQ(tx.getSenderId64(), rx.getSenderId64());
  EXPECT_EQ(tx.payloadLength, rx.payloadLength);
  EXPECT_EQ(0, memcmp(tx.payload, rx.payload, rx.payloadLength));
  EXPECT_EQ(0, memcmp(tx.signature, rx.signature, BITCHAT_SIGNATURE_SIZE));
}

TEST(BitchatProtocol, ParseV2WithRouteSkipsHops) {
  // v2 header (16B) + sender(8) + route(1 + 2*8 = 17) + payload(4)
  std::vector<uint8_t> wire;
  wire.push_back(2);  // version 2
  wire.push_back(BITCHAT_MSG_MESSAGE);
  wire.push_back(5);  // ttl
  uint8_t ts[8] = {0, 0, 1, 0x9B, 0x10, 0, 0, 1};
  wire.insert(wire.end(), ts, ts + 8);
  wire.push_back(BITCHAT_FLAG_HAS_ROUTE);
  uint8_t plen[4] = {0, 0, 0, 4};
  wire.insert(wire.end(), plen, plen + 4);
  for (int i = 0; i < 8; i++) wire.push_back(0x11);  // sender
  wire.push_back(2);                                  // 2 route hops
  for (int i = 0; i < 16; i++) wire.push_back(0x22);  // hop IDs
  const char* body = "test";
  wire.insert(wire.end(), body, body + 4);

  EXPECT_EQ(static_cast<int>(wire.size()), BitchatProtocol::frameLength(wire.data(), wire.size()));

  BitchatMessage rx;
  ASSERT_TRUE(BitchatProtocol::parseMessage(wire.data(), wire.size(), rx));
  EXPECT_EQ(2, rx.version);
  EXPECT_EQ(4, rx.payloadLength);
  EXPECT_EQ(0, memcmp(rx.payload, "test", 4));
}

// Raw stored deflate block (BFINAL=1, BTYPE=00) for "hello" (5 bytes)
static std::vector<uint8_t> makeStoredDeflateHello() {
  return {0x01, 0x05, 0x00, 0xFA, 0xFF, 'h', 'e', 'l', 'l', 'o'};
}

TEST(BitchatProtocol, InflateV1TwoByteAndV2FourByteOriginalSize) {
  auto defl = makeStoredDeflateHello();

  // v1: 2-byte BE originalSize
  std::vector<uint8_t> v1Payload = {0x00, 0x05};
  v1Payload.insert(v1Payload.end(), defl.begin(), defl.end());
  uint8_t out[64];
  int n1 = BitchatProtocol::inflatePayload(v1Payload.data(), v1Payload.size(), out, sizeof(out), 1);
  ASSERT_EQ(5, n1);
  EXPECT_EQ(0, memcmp(out, "hello", 5));

  // v2: 4-byte BE originalSize
  std::vector<uint8_t> v2Payload = {0x00, 0x00, 0x00, 0x05};
  v2Payload.insert(v2Payload.end(), defl.begin(), defl.end());
  memset(out, 0, sizeof(out));
  int n2 = BitchatProtocol::inflatePayload(v2Payload.data(), v2Payload.size(), out, sizeof(out), 2);
  ASSERT_EQ(5, n2);
  EXPECT_EQ(0, memcmp(out, "hello", 5));

  // Declared size mismatch must be rejected
  std::vector<uint8_t> badSize = {0x00, 0x00, 0x00, 0x06};
  badSize.insert(badSize.end(), defl.begin(), defl.end());
  EXPECT_LT(BitchatProtocol::inflatePayload(badSize.data(), badSize.size(), out, sizeof(out), 2), 0);
}

TEST(BitchatProtocol, DuplicateCacheHashesFullPayload) {
  BitchatDuplicateCache cache;
  BitchatMessage a, b;
  a.version = b.version = BITCHAT_VERSION;
  a.type = b.type = BITCHAT_MSG_MESSAGE;
  a.timestamp = b.timestamp = 1767000000000ULL;
  a.setSenderId64(0x123456789ABCDEF0ULL);
  b.setSenderId64(0x123456789ABCDEF0ULL);

  // Same 16-byte prefix, different tail
  const char* p1 = "0123456789abcdef_first";
  const char* p2 = "0123456789abcdef_secon";  // same length!
  memcpy(a.payload, p1, strlen(p1));
  a.payloadLength = static_cast<uint16_t>(strlen(p1));
  memcpy(b.payload, p2, strlen(p2));
  b.payloadLength = static_cast<uint16_t>(strlen(p2));

  EXPECT_FALSE(cache.isDuplicate(a));
  EXPECT_TRUE(cache.isDuplicate(a));
  EXPECT_FALSE(cache.isDuplicate(b));
}

TEST(BitchatProtocol, SigningPreimageClearsTtlSigAndPads) {
  BitchatMessage msg;
  msg.version = BITCHAT_VERSION;
  msg.type = BITCHAT_MSG_MESSAGE;
  msg.ttl = 7;
  msg.timestamp = 1767000000000ULL;
  msg.flags = BITCHAT_FLAG_HAS_SIGNATURE | 0x10;  // sig + isRSR
  msg.setSenderId64(0x1122334455667788ULL);
  memcpy(msg.payload, "hi", 2);
  msg.payloadLength = 2;

  uint8_t preimage[512];
  size_t len = BitchatProtocol::signingPreimage(msg, preimage, sizeof(preimage));
  ASSERT_EQ(256u, len);  // PKCS#7 padded to 256-byte block
  EXPECT_EQ(0u, preimage[2]);  // TTL forced to 0
  EXPECT_EQ(0u, preimage[11] & (BITCHAT_FLAG_HAS_SIGNATURE | 0x10));
  // Original message must remain untouched
  EXPECT_EQ(7u, msg.ttl);
  EXPECT_TRUE(msg.hasSignature());
}

TEST(BitchatProtocol, FindTLVExtractsFields) {
  const uint8_t tlv[] = {
      BITCHAT_TLV_NICKNAME, 3, 'a', 'l', 'i',
      BITCHAT_TLV_NOISE_PUBKEY, 2, 0xDE, 0xAD,
  };
  uint8_t vlen = 0;
  const uint8_t* val = BitchatProtocol::findTLV(tlv, sizeof(tlv), BITCHAT_TLV_NICKNAME, &vlen);
  ASSERT_NE(nullptr, val);
  ASSERT_EQ(3u, vlen);
  EXPECT_EQ(0, memcmp(val, "ali", 3));

  val = BitchatProtocol::findTLV(tlv, sizeof(tlv), BITCHAT_TLV_NOISE_PUBKEY, &vlen);
  ASSERT_NE(nullptr, val);
  ASSERT_EQ(2u, vlen);
  EXPECT_EQ(0xDE, val[0]);
  EXPECT_EQ(0xAD, val[1]);

  EXPECT_EQ(nullptr, BitchatProtocol::findTLV(tlv, sizeof(tlv), BITCHAT_TLV_ED25519_PUBKEY, &vlen));
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
