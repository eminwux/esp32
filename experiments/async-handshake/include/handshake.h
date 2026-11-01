#pragma once
#include "frame.h"

namespace ook {
// Pure protocol state: no clocks, GPIO or radio calls. The application decides
// WHEN to send a queued frame; this class decides WHICH frame is allowed next.
class Handshake {
public:
    enum class State { Listening, ReplyQueued, AwaitingReply };
private:
    uint8_t localNode_;
    State state_ = State::Listening;
    uint32_t sessionId_ = 0, retiredSessionId_ = 0;
    uint16_t lastSentSequence_ = 0;
    Frame queuedFrame_ = {};

    bool sessionActive() const { return state_ != State::Listening; }
public:
    explicit Handshake(uint8_t localNode) : localNode_(localNode) {}
    State state() const { return state_; }
    bool hasQueuedReply() const { return state_ == State::ReplyQueued; }
    bool awaitingReply() const { return state_ == State::AwaitingReply; }
    const Frame& queuedFrame() const { return queuedFrame_; }

    // A alone initiates. A fresh nonzero epoch prevents old ACKs from being
    // mistaken for responses to sequence zero after timeout or reboot.
    void startSession(uint32_t newSessionId) {
        retireSession();
        if (localNode_ != 1) return;
        sessionId_ = newSessionId ? newSessionId : 1;
        if (sessionId_ == retiredSessionId_) {
            ++sessionId_;
            if (!sessionId_) sessionId_ = 1;
        }
        state_ = State::ReplyQueued;
        queuedFrame_ = {kPing, 1, 2, 0, 0xffff, sessionId_};
    }
    void retireSession() {
        if (sessionActive()) retiredSessionId_ = sessionId_;
        state_ = State::Listening;
    }
    // Call only after successful TX completion. Merely queueing a frame must
    // not make its ACK eligible, since it may wait several seconds for airtime.
    void onTransmitComplete() {
        lastSentSequence_ = queuedFrame_.seq;
        state_ = State::AwaitingReply;
    }
    // Input has already passed frame/CRC checks. Session/sequence validation is
    // a separate boundary: a structurally valid packet can still be stale.
    bool onFrameReceived(const Frame& frame) {
        if (frame.source != 3 - localNode_ || frame.destination != localNode_ || !frame.session)
            return false;
        if (localNode_ == 2 && frame.type == kPing && frame.seq == 0 && frame.ack == 0xffff) {
            if (frame.session == retiredSessionId_ || (sessionActive() && frame.session == sessionId_))
                return false;
            // A new PING may supersede B's old exchange after A reboots.
            if (sessionActive()) retiredSessionId_ = sessionId_;
            sessionId_ = frame.session;
        } else if (!awaitingReply() || frame.type != kPongPing ||
                   frame.session != sessionId_ || frame.ack != lastSentSequence_ ||
                   frame.seq != uint16_t(lastSentSequence_ + 1)) {
            return false;
        }
        // uint16_t conversion intentionally wraps sequence 65535 to zero.
        queuedFrame_ = {kPongPing, localNode_, uint8_t(3 - localNode_),
                        uint16_t(frame.seq + 1), frame.seq, sessionId_};
        state_ = State::ReplyQueued;
        return true;
    }
};
}  // namespace ook
