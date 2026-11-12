#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

// Largest payload (bytes) of one public MESSAGE sent from MeshCore to Bitchat.
//
// iOS Bitchat up to at least 1.8.0 rebuilds the signature preimage with BinaryProtocol.encode(),
// which zlib-compresses any payload of 100 bytes or more (with < 90% unique bytes). The bridge
// signs the uncompressed bytes it sends, so iOS rejects every such message as wrongly signed
// and drops it. Keeping each payload at 99 bytes or less avoids that path. Raise this once iOS
// verifies over the received bytes (bitchat PR #1805; Android does since #863).
#ifndef BITCHAT_MAX_PUBLIC_PAYLOAD
  #define BITCHAT_MAX_PUBLIC_PAYLOAD 99
#endif

static_assert(BITCHAT_MAX_PUBLIC_PAYLOAD >= 32, "BITCHAT_MAX_PUBLIC_PAYLOAD too small");
static_assert(BITCHAT_MAX_PUBLIC_PAYLOAD <= 1024, "BITCHAT_MAX_PUBLIC_PAYLOAD too large");

/**
 * Splits a MeshCore #mesh message into Bitchat public MESSAGE payloads of at most
 * maxPayload bytes.
 *
 * A message that fits is sent as "<sender> text". Longer messages become
 * "<sender> [i/n] piece" chunks: every chunk repeats the sender, so readers can tell who
 * is talking and other bridges (including older versions) still recognise the chunk as a
 * MeshCore relay ("<name> " prefix) and do not echo it back to LoRa. Pieces are cut at the
 * last whitespace in the second half of the available room, otherwise at a UTF-8 code
 * point boundary. Text beyond MAX_CHUNKS chunks is dropped (truncated() reports it).
 */
class BitchatTextSplitter {
public:
    static const size_t MAX_CHUNKS = 9;    // keeps the "[i/n] " marker at 6 bytes
    static const size_t MARKER_LEN = 6;

    BitchatTextSplitter(const char* sender, const char* body, size_t maxPayload)
        : _body(body ? body : ""), _room(0), _count(0), _truncated(false),
          _single(false), _index(0), _pos(0) {
        if (sender == nullptr) sender = "";
        // "<" name "> " may use at most half of a payload; long names are cut on a code point
        size_t nameMax = maxPayload / 2 - 3;
        _nameLen = utf8Floor(sender, strlen(sender), nameMax);
        _sender = sender;
        _bodyLen = strlen(_body);

        size_t prefixLen = _nameLen + 3;
        size_t start = skipSpace(0);
        if (prefixLen + _bodyLen <= maxPayload) {
            _count = 1;
            _single = true;
            return;
        }
        _single = false;
        _room = maxPayload - prefixLen - MARKER_LEN;
        size_t pos = start;
        while (pos < _bodyLen && _count < MAX_CHUNKS) {
            size_t end, next;
            cut(pos, end, next);
            _count++;
            pos = next;
        }
        _truncated = pos < _bodyLen;
    }

    size_t count() const { return _count; }
    bool truncated() const { return _truncated; }

    /**
     * Write the next chunk, NUL-terminated, into out (capacity >= maxPayload + 1).
     * @return chunk length in bytes, 0 when all chunks were written
     */
    size_t next(char* out) {
        if (_index >= _count) return 0;
        size_t n = 0;
        out[n++] = '<';
        memcpy(out + n, _sender, _nameLen); n += _nameLen;
        out[n++] = '>';
        out[n++] = ' ';
        if (_single) {
            memcpy(out + n, _body, _bodyLen); n += _bodyLen;
            _index++;
            out[n] = '\0';
            return n;
        }
        if (_index == 0) _pos = skipSpace(0);
        out[n++] = '[';
        out[n++] = (char)('1' + _index);
        out[n++] = '/';
        out[n++] = (char)('0' + _count);
        out[n++] = ']';
        out[n++] = ' ';
        size_t end, next;
        cut(_pos, end, next);
        memcpy(out + n, _body + _pos, end - _pos); n += end - _pos;
        _pos = next;
        _index++;
        out[n] = '\0';
        return n;
    }

    /** Length of the longest prefix of s[0..len) that is <= maxBytes and ends on a code point boundary */
    static size_t utf8Floor(const char* s, size_t len, size_t maxBytes) {
        if (len <= maxBytes) return len;
        size_t c = maxBytes;
        // s[c] starts the remainder; back off while it is a continuation byte (max 3 for valid UTF-8)
        for (int i = 0; i < 3 && c > 0 && (((uint8_t)s[c]) & 0xC0) == 0x80; i++) c--;
        if ((((uint8_t)s[c]) & 0xC0) == 0x80) return maxBytes;  // malformed input: hard cut
        return c;
    }

private:
    const char* _sender;
    const char* _body;
    size_t _nameLen;
    size_t _bodyLen;
    size_t _room;
    size_t _count;
    bool _truncated;
    bool _single;
    size_t _index;
    size_t _pos;

    static bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

    size_t skipSpace(size_t p) const {
        while (p < _bodyLen && isSpace(_body[p])) p++;
        return p;
    }

    // Piece starting at pos: text is [pos, end), the following piece starts at next
    void cut(size_t pos, size_t& end, size_t& next) const {
        size_t left = _bodyLen - pos;
        size_t len = left;
        if (left > _room) {
            len = utf8Floor(_body + pos, left, _room);
            // prefer breaking at whitespace in the second half of the room
            for (size_t i = len; i > _room / 2; i--) {
                if (isSpace(_body[pos + i])) { len = i; break; }
            }
        }
        end = pos + len;
        next = skipSpace(end);
        while (end > pos && isSpace(_body[end - 1])) end--;
    }
};
