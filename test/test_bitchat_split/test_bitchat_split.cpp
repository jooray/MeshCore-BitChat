#include <gtest/gtest.h>

#include <string>
#include <vector>

#include <helpers/bitchat/BitchatTextSplit.h>

static std::vector<std::string> splitAll(const char* sender, const char* body, size_t maxPayload,
                                         bool* truncated = nullptr) {
  BitchatTextSplitter s(sender, body, maxPayload);
  std::vector<std::string> out;
  char buf[1100];
  size_t n;
  while ((n = s.next(buf)) > 0) {
    EXPECT_EQ(n, strlen(buf));
    out.push_back(std::string(buf, n));
  }
  EXPECT_EQ(s.count(), out.size());
  if (truncated) *truncated = s.truncated();
  return out;
}

static bool validUtf8(const std::string& s) {
  size_t i = 0;
  while (i < s.size()) {
    uint8_t c = (uint8_t)s[i];
    size_t n = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 0;
    if (n == 0 || i + n > s.size()) return false;
    for (size_t k = 1; k < n; k++) {
      if (((uint8_t)s[i + k] & 0xC0) != 0x80) return false;
    }
    i += n;
  }
  return true;
}

// Removes "<sender> " and an optional "[i/n] " marker
static std::string pieceOf(const std::string& chunk) {
  size_t p = chunk.find("> ");
  std::string rest = chunk.substr(p + 2);
  if (rest.size() >= 6 && rest[0] == '[' && rest[2] == '/' && rest[4] == ']' && rest[5] == ' ') {
    rest = rest.substr(6);
  }
  return rest;
}

TEST(BitchatTextSplit, DefaultLimitIsBelowIosCompressionThreshold) {
  EXPECT_LE(BITCHAT_MAX_PUBLIC_PAYLOAD, 99);
}

TEST(BitchatTextSplit, ShortMessageIsOneChunkWithoutMarker) {
  auto chunks = splitAll("alice", "hello world", 99);
  ASSERT_EQ(1u, chunks.size());
  EXPECT_EQ("<alice> hello world", chunks[0]);
}

TEST(BitchatTextSplit, ExactlyMaxFitsInOneChunk) {
  std::string body(99 - 8, 'x');  // "<alice> " is 8 bytes
  auto chunks = splitAll("alice", body.c_str(), 99);
  ASSERT_EQ(1u, chunks.size());
  EXPECT_EQ(99u, chunks[0].size());
}

TEST(BitchatTextSplit, OneByteOverSplitsInTwo) {
  std::string body(99 - 8 + 1, 'x');
  auto chunks = splitAll("alice", body.c_str(), 99);
  ASSERT_EQ(2u, chunks.size());
  EXPECT_EQ(0u, chunks[0].find("<alice> [1/2] "));
  EXPECT_EQ(0u, chunks[1].find("<alice> [2/2] "));
  EXPECT_EQ(body, pieceOf(chunks[0]) + pieceOf(chunks[1]));
}

TEST(BitchatTextSplit, EveryChunkWithinLimitAndKeepsSenderPrefix) {
  std::string body;
  for (int i = 0; i < 30; i++) body += "word" + std::to_string(i) + " ";
  for (size_t max : {32u, 60u, 99u, 150u}) {
    auto chunks = splitAll("Bob 42", body.c_str(), max);
    ASSERT_GE(chunks.size(), 2u);
    for (auto& c : chunks) {
      EXPECT_LE(c.size(), max);
      EXPECT_EQ(0u, c.find("<Bob 42> ["));  // older bridges recognise "<name> " and skip it
    }
  }
}

TEST(BitchatTextSplit, PrefersWhitespaceAndLosesNoWords) {
  const char* body =
      "The quick brown fox jumps over the lazy dog while the MeshCore bridge carries the text "
      "across LoRa to people who are far away from any phone signal at all.";
  auto chunks = splitAll("carol", body, 99);
  ASSERT_GE(chunks.size(), 2u);
  std::string joined;
  for (size_t i = 0; i < chunks.size(); i++) {
    std::string p = pieceOf(chunks[i]);
    EXPECT_NE(' ', p.front());
    EXPECT_NE(' ', p.back());
    if (i > 0) joined += " ";
    joined += p;
  }
  EXPECT_EQ(std::string(body), joined);
}

TEST(BitchatTextSplit, NeverCutsInsideUtf8Sequence) {
  // No whitespace, multi-byte characters only: forces hard cuts
  std::string body;
  for (int i = 0; i < 40; i++) body += "\xC5\xBE\xE2\x82\xAC\xF0\x9F\x93\xA1";  // ž € 📡
  auto chunks = splitAll("dave", body.c_str(), 99);
  std::string joined;
  for (auto& c : chunks) {
    EXPECT_LE(c.size(), 99u);
    EXPECT_TRUE(validUtf8(c)) << c;
    joined += pieceOf(c);
  }
  EXPECT_EQ(body, joined);
}

TEST(BitchatTextSplit, LongSenderNameIsCutOnCodePoint) {
  std::string name;
  for (int i = 0; i < 40; i++) name += "\xC3\xA9";  // 80 bytes of é
  auto chunks = splitAll(name.c_str(), "hi", 99);
  ASSERT_EQ(1u, chunks.size());
  EXPECT_TRUE(validUtf8(chunks[0]));
  EXPECT_LE(chunks[0].size(), 99u);
  EXPECT_LE(chunks[0].find("> "), 99u / 2);
}

TEST(BitchatTextSplit, MeshcoreMaximumFitsWithoutTruncation) {
  std::string body(200, 'a');
  bool truncated = true;
  auto chunks = splitAll("0123456789012345678901234567890", body.c_str(), 99, &truncated);
  EXPECT_FALSE(truncated);
  EXPECT_LE(chunks.size(), 9u);
  std::string joined;
  for (auto& c : chunks) joined += pieceOf(c);
  EXPECT_EQ(body, joined);
}

TEST(BitchatTextSplit, TooLongTextIsTruncatedAtNineChunks) {
  std::string body(2000, 'b');
  bool truncated = false;
  auto chunks = splitAll("eve", body.c_str(), 99, &truncated);
  EXPECT_TRUE(truncated);
  EXPECT_EQ(9u, chunks.size());
  EXPECT_EQ(0u, chunks[8].find("<eve> [9/9] "));
}

TEST(BitchatTextSplit, NullAndEmptyInputs) {
  auto a = splitAll(nullptr, nullptr, 99);
  ASSERT_EQ(1u, a.size());
  EXPECT_EQ("<> ", a[0]);
  auto b = splitAll("x", "", 99);
  ASSERT_EQ(1u, b.size());
  EXPECT_EQ("<x> ", b[0]);
}

TEST(BitchatTextSplit, LeadingAndRepeatedSpacesDoNotProduceEmptyChunks) {
  std::string body = "   ";
  for (int i = 0; i < 20; i++) body += "abcdefgh      ";
  auto chunks = splitAll("f", body.c_str(), 40);
  for (auto& c : chunks) {
    EXPECT_LE(c.size(), 40u);
    EXPECT_FALSE(pieceOf(c).empty());
  }
}

TEST(BitchatTextSplit, Utf8FloorHelper) {
  const char* s = "a\xE2\x82\xAC" "b";  // a € b
  EXPECT_EQ(1u, BitchatTextSplitter::utf8Floor(s, 5, 2));
  EXPECT_EQ(1u, BitchatTextSplitter::utf8Floor(s, 5, 3));
  EXPECT_EQ(4u, BitchatTextSplitter::utf8Floor(s, 5, 4));
  EXPECT_EQ(5u, BitchatTextSplitter::utf8Floor(s, 5, 9));
}

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
