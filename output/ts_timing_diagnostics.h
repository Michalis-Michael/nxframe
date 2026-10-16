// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <ostream>

// Observational, sender-thread-only telemetry. No packet writes, allocations,
// buffering, sleeping or timestamp repairs. Times describe transport API calls,
// not NIC transmission or receiver delivery. PCR and PES timestamps come from
// the accepted TS payload, not the muxer's most recently submitted packet.
class TsTimingDiagnostics {
public:
    struct Sample {
        uint64_t session;
        int64_t scheduledNs; // -1 for the unpaced path
        int64_t beginNs, endNs;
        int64_t muxInputDts90k; // latest submitted video DTS, NOT emitted PCR
        size_t tsQueue, videoQueue, audioQueue;
    };

    TsTimingDiagnostics(int videoPid, int64_t rate) : video_pid_(videoPid), rate_(rate) {}

    void observe(const uint8_t* data, size_t size, const Sample& s, bool accepted)
    {
        if (!have_session_ || session_ != s.session) {
            session_ = s.session; have_session_ = true; resetClock(); ++events_;
        }
        ++calls_;
        if (!accepted) ++failed_;
        if (s.scheduledNs >= 0) lateness_.add(s.beginNs - s.scheduledNs);
        if (last_send_begin_ >= 0) {
            const int64_t gap = s.beginNs - last_send_begin_;
            send_gap_.add(gap);
            if (gap < 100000) ++short_gaps_;
            if (gap > 2000000) ++long_gaps_;
        }
        last_send_begin_ = s.beginNs;
        if (s.scheduledNs >= 0 && last_scheduled_ >= 0 && last_scheduled_begin_ >= 0) {
            // Change in API submission error from one scheduled datagram to the next.
            spacing_error_.add((s.beginNs - last_scheduled_begin_) -
                               (s.scheduledNs - last_scheduled_));
        }
        if (s.scheduledNs >= 0) {
            last_scheduled_ = s.scheduledNs;
            last_scheduled_begin_ = s.beginNs;
        }
        duration_.add(s.endNs - s.beginNs);
        input_dts_ = s.muxInputDts90k;
        ts_queue_ = s.tsQueue; video_queue_ = s.videoQueue; audio_queue_ = s.audioQueue;
        queue_max_ = std::max(queue_max_, s.tsQueue);
        queue_min_ = std::min(queue_min_, s.tsQueue);
        if (have_previous_queue_) {
            if (s.tsQueue > previous_queue_) {
                ++queue_growing_samples_;
                queue_max_growth_ = std::max(queue_max_growth_, s.tsQueue - previous_queue_);
            } else if (s.tsQueue < previous_queue_) {
                ++queue_draining_samples_;
                queue_max_drain_ = std::max(queue_max_drain_, previous_queue_ - s.tsQueue);
            } else {
                ++queue_flat_samples_;
            }
        }
        previous_queue_ = s.tsQueue;
        have_previous_queue_ = true;
        queue_samples_++;
        queue_sum_ += s.tsQueue;
        video_queue_max_ = std::max(video_queue_max_, s.videoQueue);
        audio_queue_max_ = std::max(audio_queue_max_, s.audioQueue);
        if (rate_ > 0) {
            // Estimated queued TS duration only; does not measure packet residence time.
            queue_delay_ns_.add(static_cast<int64_t>(
                static_cast<long double>(s.tsQueue) * 188 * 8 * 1000000000 / rate_));
        }
        if (!accepted) return; // a failed API call is not an accepted TS payload
        accepted_bytes_ += size;
        if (size % 188) ++unaligned_;
        for (size_t offset = 0; offset + 188 <= size; offset += 188) {
            const uint8_t* p = data + offset;
            if (p[0] != 0x47) { ++invalid_; continue; }
            if ((((p[1] & 31) << 8) | p[2]) != video_pid_) continue;
            const unsigned afc = (p[3] >> 4) & 3;
            if (!afc || (p[1] & 0x80) || (p[3] & 0xc0)) { ++invalid_; resetClock(); continue; }
            size_t payload = 4;
            if (afc & 2) {
                if (p[4] > 183) { ++invalid_; resetClock(); continue; }
                payload = 5 + p[4];
                if (p[4] && (p[5] & 0x80)) { resetClock(); ++events_; }
                if (p[4] && (p[5] & 0x10)) {
                    if (p[4] < 7 || (((p[10] & 1) << 8) | p[11]) >= 300) {
                        ++invalid_; resetClock();
                    } else {
                        const uint64_t base = (uint64_t{p[6]} << 25) | (uint64_t{p[7]} << 17) |
                            (uint64_t{p[8]} << 9) | (uint64_t{p[9]} << 1) | (p[10] >> 7);
                        const uint64_t value = base * 300 + (((p[10] & 1) << 8) | p[11]);
                        observePcr(value, bytes_ + offset + 11, s);
                    }
                }
            }
            if (!(afc & 1) || !(p[1] & 0x40) || payload + 14 > 188) continue;
            const uint8_t* q = p + payload;
            if (q[0] || q[1] || q[2] != 1 || (q[6] & 0xc0) != 0x80) continue;
            const unsigned flags = q[7] >> 6;
            if ((flags != 2 && flags != 3) || q[8] < 5 || !timestamp(q + 9, flags, pts_)) continue;
            dts_ = pts_;
            if (flags == 3 && (payload + 19 > 188 || q[8] < 10 || !timestamp(q + 14, 1, dts_))) {
                dts_ = -1; continue;
            }
            ++pes_;
        }
        bytes_ += size;
    }

    // Explicit scheduler actions; observation only (never alters deadlines).
    void recovery(bool cbr, bool resync, int64_t lateNs) {
        if (cbr) ++cbr_recovery_; else ++vbr_recovery_;
        if (resync) ++resyncs_;
        recovery_lateness_.add(lateNs);
        // Count only excess lateness (the number of discarded deadlines is
        // not directly inferable from this interval-independent callback).
        if (lateNs > 0) recovery_delay_total_ns_ += static_cast<uint64_t>(lateNs);
    }

    void report(std::ostream& out)
    {
        out << "[OutputManager][TS-TIMING] session=" << session_
            << " scope=transport_api calls=" << calls_ << " failed=" << failed_
            << " accepted_bytes=" << accepted_bytes_ << " pcr_samples=" << pcrs_
            << " video_pes=" << pes_ << " clock_events=" << events_
            << " invalid=" << invalid_ << " unaligned=" << unaligned_;
        lateness_.write(out, "schedule_lateness_ns"); duration_.write(out, "call_duration_ns");
        pcr_gap_.write(out, "pcr_value_gap_ns"); call_gap_.write(out, "pcr_call_gap_ns");
        phase_.write(out, "pcr_vs_packet_clock_ns");
        send_gap_.write(out, "send_begin_gap_ns");
        spacing_error_.write(out, "send_spacing_error_ns");
        pcr_schedule_gap_.write(out, "pcr_schedule_gap_ns");
        pcr_send_error_.write(out, "pcr_call_vs_value_gap_error_ns");
        queue_delay_ns_.write(out, "estimated_ts_queue_delay_ns");
        recovery_lateness_.write(out, "recovery_lateness_ns");
        out << " send_gap_lt100us=" << short_gaps_
            << " send_gap_gt2ms=" << long_gaps_
            << " cbr_recovery=" << cbr_recovery_
            << " vbr_recovery=" << vbr_recovery_
            << " resyncs=" << resyncs_
            << " recovery_delay_total_ns=" << recovery_delay_total_ns_
            << " ts_queue_avg=" << (queue_samples_ ? queue_sum_ / queue_samples_ : 0)
            << " ts_queue_min=" << (queue_samples_ ? queue_min_ : 0)
            << " ts_queue_growing_samples=" << queue_growing_samples_
            << " ts_queue_draining_samples=" << queue_draining_samples_
            << " ts_queue_flat_samples=" << queue_flat_samples_
            << " ts_queue_max_growth=" << queue_max_growth_
            << " ts_queue_max_drain=" << queue_max_drain_
            << " video_queue_max=" << video_queue_max_
            << " audio_queue_max=" << audio_queue_max_;
        out << " last_pcr_27m=" << last_pcr_ << " pes_pts_90k=" << pts_
            << " pes_dts_90k=" << dts_ << " mux_input_dts_90k=" << input_dts_
            << " ts_queue=" << ts_queue_ << " ts_queue_max=" << queue_max_
            << " video_queue=" << video_queue_ << " audio_queue=" << audio_queue_ << '\n';
        calls_ = failed_ = accepted_bytes_ = pcrs_ = pes_ = events_ = invalid_ = unaligned_ = 0;
        queue_max_ = video_queue_max_ = audio_queue_max_ = 0;
        queue_min_ = static_cast<size_t>(-1);
        queue_growing_samples_ = queue_draining_samples_ = queue_flat_samples_ = 0;
        queue_max_growth_ = queue_max_drain_ = 0;
        queue_samples_ = queue_sum_ = short_gaps_ = long_gaps_ = 0;
        cbr_recovery_ = vbr_recovery_ = resyncs_ = recovery_delay_total_ns_ = 0;
        lateness_ = Range{}; duration_ = Range{}; pcr_gap_ = Range{}; call_gap_ = Range{}; phase_ = Range{};
        send_gap_ = Range{}; spacing_error_ = Range{}; pcr_schedule_gap_ = Range{};
        pcr_send_error_ = Range{}; queue_delay_ns_ = Range{}; recovery_lateness_ = Range{};
    }

private:
    struct Range {
        uint64_t count = 0; int64_t minimum = 0, maximum = 0;
        void add(int64_t value) {
            if (!count++) minimum = maximum = value;
            else { minimum = std::min(minimum, value); maximum = std::max(maximum, value); }
        }
        void write(std::ostream& out, const char* name) const {
            out << ' ' << name << "_count=" << count << ' ' << name << "_min=" << minimum
                << ' ' << name << "_max=" << maximum;
        }
    };
    static bool timestamp(const uint8_t* p, unsigned prefix, int64_t& value) {
        if ((p[0] >> 4) != prefix || !(p[0] & 1) || !(p[2] & 1) || !(p[4] & 1)) return false;
        value = (int64_t{(p[0] >> 1) & 7} << 30) | (int64_t{p[1]} << 22) |
                (int64_t{p[2] >> 1} << 15) | (int64_t{p[3]} << 7) | (p[4] >> 1);
        return true;
    }
    void resetClock() {
        anchored_ = false; bytes_ = 0; elapsed_ticks_ = 0;
        last_pcr_ = pts_ = dts_ = -1;
        last_pcr_schedule_ = -1;
    }
    void observePcr(uint64_t value, uint64_t position, const Sample& s) {
        constexpr uint64_t wrap = (uint64_t{1} << 33) * 300;
        ++pcrs_;
        if (anchored_) {
            const uint64_t delta = (value + wrap - static_cast<uint64_t>(last_pcr_)) % wrap;
            if (delta >= wrap / 2) { anchored_ = false; ++events_; }
            else {
                elapsed_ticks_ += delta;
                pcr_gap_.add(static_cast<int64_t>(static_cast<long double>(delta) * 1000000000 / 27000000));
                call_gap_.add(s.beginNs - last_call_);
                const int64_t valueGapNs = static_cast<int64_t>(
                    static_cast<long double>(delta) * 1000000000 / 27000000);
                pcr_send_error_.add((s.beginNs - last_call_) - valueGapNs);
                if (s.scheduledNs >= 0 && last_pcr_schedule_ >= 0)
                    pcr_schedule_gap_.add(s.scheduledNs - last_pcr_schedule_);
            }
        }
        if (!anchored_) {
            anchored_ = true; anchor_byte_ = position; elapsed_ticks_ = 0;
        }
        if (rate_ > 0) {
            const long double pcrNs = static_cast<long double>(elapsed_ticks_) * 1000000000 / 27000000;
            const long double packetNs = static_cast<long double>(position - anchor_byte_) * 8 * 1000000000 / rate_;
            phase_.add(static_cast<int64_t>(pcrNs - packetNs));
        }
        last_pcr_ = static_cast<int64_t>(value); last_call_ = s.beginNs;
        last_pcr_schedule_ = s.scheduledNs;
    }
    int video_pid_; int64_t rate_;
    bool have_session_ = false, anchored_ = false;
    uint64_t session_ = 0, bytes_ = 0, anchor_byte_ = 0, elapsed_ticks_ = 0;
    int64_t last_pcr_ = -1, pts_ = -1, dts_ = -1, input_dts_ = -1, last_call_ = 0;
    int64_t last_pcr_schedule_ = -1, last_send_begin_ = -1;
    int64_t last_scheduled_ = -1, last_scheduled_begin_ = -1;
    uint64_t calls_ = 0, failed_ = 0, accepted_bytes_ = 0, pcrs_ = 0, pes_ = 0;
    uint64_t events_ = 0, invalid_ = 0, unaligned_ = 0;
    size_t ts_queue_ = 0, video_queue_ = 0, audio_queue_ = 0, queue_max_ = 0;
    uint64_t short_gaps_ = 0, long_gaps_ = 0, cbr_recovery_ = 0, vbr_recovery_ = 0, resyncs_ = 0;
    uint64_t recovery_delay_total_ns_ = 0;
    uint64_t queue_samples_ = 0, queue_sum_ = 0;
    // Sampled deltas only; retain previous depth across interval reports.
    bool have_previous_queue_ = false;
    size_t previous_queue_ = 0, queue_min_ = static_cast<size_t>(-1);
    size_t queue_max_growth_ = 0, queue_max_drain_ = 0;
    uint64_t queue_growing_samples_ = 0, queue_draining_samples_ = 0, queue_flat_samples_ = 0;
    size_t video_queue_max_ = 0, audio_queue_max_ = 0;
    Range lateness_, duration_, pcr_gap_, call_gap_, phase_;
    Range send_gap_, spacing_error_, pcr_schedule_gap_, pcr_send_error_, queue_delay_ns_, recovery_lateness_;
};
