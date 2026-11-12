#include <gtest/gtest.h>

#include <helpers/bitchat/BitchatTimeSync.h>

typedef BitchatTimeSyncPolicy P;

static const int64_t MIN = 60000;
static const int64_t HOUR = 3600000;

TEST(BitchatTimeSync, UntrustedClockAdoptsEvenUnverified) {
  P p;
  EXPECT_TRUE(p.shouldAdopt(P::CLOCK_UNTRUSTED, false, -5 * HOUR, 1, 0));
  EXPECT_TRUE(p.shouldAdopt(P::CLOCK_UNTRUSTED, false, 5 * HOUR, 1, 0));
}

TEST(BitchatTimeSync, UnverifiedNeverMovesWeakOrTrustedClock) {
  P p;
  EXPECT_FALSE(p.shouldAdopt(P::CLOCK_WEAK, false, 5 * MIN, 1, 0));
  EXPECT_FALSE(p.shouldAdopt(P::CLOCK_TRUSTED, false, 5 * MIN, 1, 0));
  EXPECT_FALSE(p.shouldAdopt(P::CLOCK_TRUSTED, false, -5 * MIN, 1, 0));
}

TEST(BitchatTimeSync, VerifiedReplacesWeakClockInEitherDirection) {
  P p;
  EXPECT_TRUE(p.shouldAdopt(P::CLOCK_WEAK, true, -3 * HOUR, 1, 0));
  EXPECT_TRUE(p.shouldAdopt(P::CLOCK_WEAK, true, 10, 1, 0));
}

TEST(BitchatTimeSync, TrustedClockIgnoresSmallDifferences) {
  P p;
  EXPECT_FALSE(p.shouldAdopt(P::CLOCK_TRUSTED, true, MIN, 1, 0));
  EXPECT_FALSE(p.shouldAdopt(P::CLOCK_TRUSTED, true, -MIN, 1, 0));
  EXPECT_FALSE(P::needsVerification(P::CLOCK_TRUSTED, 30000));
  EXPECT_TRUE(P::needsVerification(P::CLOCK_TRUSTED, 2 * MIN));
  EXPECT_TRUE(P::needsVerification(P::CLOCK_WEAK, 0));
  EXPECT_TRUE(P::needsVerification(P::CLOCK_UNTRUSTED, 0));
}

TEST(BitchatTimeSync, TrustedClockMovesForwardOnOneVerifiedPeer) {
  P p;
  EXPECT_TRUE(p.shouldAdopt(P::CLOCK_TRUSTED, true, 2 * MIN, 1, 0));
}

TEST(BitchatTimeSync, TrustedClockNeverJumpsBackOnOnePeer) {
  P p;
  for (uint32_t t = 0; t < 600000; t += 5000) {
    EXPECT_FALSE(p.shouldAdopt(P::CLOCK_TRUSTED, true, -10 * MIN, 1, t));
  }
}

TEST(BitchatTimeSync, BackwardsNeedsTwoAgreeingPeers) {
  P p;
  EXPECT_FALSE(p.shouldAdopt(P::CLOCK_TRUSTED, true, -10 * MIN, 1, 1000));
  EXPECT_TRUE(p.shouldAdopt(P::CLOCK_TRUSTED, true, -10 * MIN + 20000, 2, 20000));
  // the candidate is consumed
  EXPECT_FALSE(p.shouldAdopt(P::CLOCK_TRUSTED, true, -10 * MIN, 3, 30000));
}

TEST(BitchatTimeSync, BackwardsRejectedWhenPeersDisagree) {
  P p;
  EXPECT_FALSE(p.shouldAdopt(P::CLOCK_TRUSTED, true, -10 * MIN, 1, 1000));
  EXPECT_FALSE(p.shouldAdopt(P::CLOCK_TRUSTED, true, -30 * MIN, 2, 2000));
}

TEST(BitchatTimeSync, BackwardsConfirmationExpires) {
  P p;
  EXPECT_FALSE(p.shouldAdopt(P::CLOCK_TRUSTED, true, -10 * MIN, 1, 1000));
  EXPECT_FALSE(p.shouldAdopt(P::CLOCK_TRUSTED, true, -10 * MIN, 2, 1000 + P::CONFIRM_WINDOW_MS + 1));
}

TEST(BitchatTimeSync, ForwardAdoptClearsBackwardsCandidate) {
  P p;
  EXPECT_FALSE(p.shouldAdopt(P::CLOCK_TRUSTED, true, -10 * MIN, 1, 1000));
  EXPECT_TRUE(p.shouldAdopt(P::CLOCK_TRUSTED, true, 5 * MIN, 3, 2000));
  EXPECT_FALSE(p.shouldAdopt(P::CLOCK_TRUSTED, true, -10 * MIN, 2, 3000));
}

TEST(BitchatTimeSync, MillisWraparoundInConfirmWindow) {
  P p;
  EXPECT_FALSE(p.shouldAdopt(P::CLOCK_TRUSTED, true, -10 * MIN, 1, 0xFFFFF000u));
  EXPECT_TRUE(p.shouldAdopt(P::CLOCK_TRUSTED, true, -10 * MIN, 2, 0x00001000u));
}

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
