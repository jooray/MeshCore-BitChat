#pragma once

#include <stdint.h>

/**
 * Decides when the bridge may take its clock from a received Bitchat ANNOUNCE.
 *
 * Only directly received announces (TTL == 7) are offered to this policy; relayed packets and
 * sync replies carry old timestamps. iOS drops live packets more than 2 minutes off its clock
 * and (bitchat PR #1846) keeps the newest announce per peer by timestamp, so a wrong jump,
 * especially backwards, makes the bridge's own announces stale. The rules:
 *
 * - A clock that is not trustworthy (no plausible RTC, never synced) adopts any direct
 *   announce, even before its signature is checked. Any time from a phone is better than a
 *   1970 or seed clock, which no app accepts.
 * - A clock synced only from such an unverified announce adopts the next verified one.
 * - A trustworthy clock (plausible RTC or verified sync) ignores differences up to
 *   TOLERANCE_MS, moves forward on one verified announce, and moves backwards only when two
 *   different verified peers agree (within TOLERANCE_MS) within CONFIRM_WINDOW_MS.
 */
class BitchatTimeSyncPolicy {
public:
    enum Trust : uint8_t {
        CLOCK_UNTRUSTED = 0,  // RTC implausible and not synced from Bitchat
        CLOCK_WEAK = 1,       // synced from an unverified announce only
        CLOCK_TRUSTED = 2     // plausible RTC or synced from a verified announce
    };

    static const int64_t TOLERANCE_MS = 60000;
    static const uint32_t CONFIRM_WINDOW_MS = 600000;

    BitchatTimeSyncPolicy() : _hasCandidate(false), _candPeer(0), _candDelta(0), _candAt(0) {}

    /**
     * @param trust    trust level of the clock currently in use
     * @param verified announce signature and peer-ID binding were verified
     * @param deltaMs  announce timestamp minus the bridge's current time
     * @param peerId   announcing peer
     * @param nowMs    millis()
     * @return true if the bridge should adopt the announce timestamp
     */
    bool shouldAdopt(Trust trust, bool verified, int64_t deltaMs, uint64_t peerId, uint32_t nowMs) {
        if (!verified) return trust == CLOCK_UNTRUSTED;
        if (trust != CLOCK_TRUSTED) { _hasCandidate = false; return true; }
        if (deltaMs >= -TOLERANCE_MS && deltaMs <= TOLERANCE_MS) return false;
        if (deltaMs > 0) { _hasCandidate = false; return true; }

        // Backwards: needs a second, different verified peer reporting the same offset
        if (_hasCandidate && _candPeer != peerId && (uint32_t)(nowMs - _candAt) <= CONFIRM_WINDOW_MS) {
            int64_t diff = deltaMs - _candDelta;
            if (diff >= -TOLERANCE_MS && diff <= TOLERANCE_MS) {
                _hasCandidate = false;
                return true;
            }
        }
        _hasCandidate = true;
        _candPeer = peerId;
        _candDelta = deltaMs;
        _candAt = nowMs;
        return false;
    }

    /** True if a trusted clock would need a verified announce to act on this offset */
    static bool needsVerification(Trust trust, int64_t deltaMs) {
        if (trust != CLOCK_TRUSTED) return true;
        return deltaMs < -TOLERANCE_MS || deltaMs > TOLERANCE_MS;
    }

private:
    bool _hasCandidate;
    uint64_t _candPeer;
    int64_t _candDelta;
    uint32_t _candAt;
};
