#pragma once
#include "frame.h"

namespace ook {
// Stop-and-wait semantics shared with the host tests. A queued response is
// not an outstanding packet until sent(); duplicates cannot queue more RF.
class Handshake {
    uint8_t local_;
    bool active_ = false, waiting_ = false, queued_ = false;
    uint32_t session_ = 0, retired_ = 0;
    uint16_t last_ = 0;
    Frame next_ = {};
public:
    explicit Handshake(uint8_t local) : local_(local) {}
    bool pending() const { return queued_; }
    bool waiting() const { return waiting_; }
    const Frame& next() const { return next_; }
    void recover(uint32_t session) {
        timeout();
        if (local_ != 1) return;
        session_ = session ? session : 1;
        if (session_ == retired_) { ++session_; if (!session_) session_ = 1; }
        active_ = queued_ = true;
        next_ = {kPing, 1, 2, 0, 0xffff, session_};
    }
    void timeout() {
        if (active_) retired_ = session_;
        active_ = waiting_ = queued_ = false;
    }
    void sent() { last_ = next_.seq; queued_ = false; waiting_ = true; }
    bool receive(const Frame& f) {
        if (f.source != 3 - local_ || f.destination != local_ || !f.session) return false;
        if (local_ == 2 && f.type == kPing && f.seq == 0 && f.ack == 0xffff) {
            if (f.session == retired_ || (active_ && f.session == session_)) return false;
            if (active_) retired_ = session_;
            session_ = f.session; active_ = true;
        } else if (!active_ || !waiting_ || f.type != kPongPing ||
                   f.session != session_ || f.ack != last_ ||
                   f.seq != uint16_t(last_ + 1)) return false;
        next_ = {kPongPing, local_, uint8_t(3 - local_), uint16_t(f.seq + 1), f.seq, session_};
        queued_ = true; waiting_ = false;
        return true;
    }
};
}  // namespace ook
