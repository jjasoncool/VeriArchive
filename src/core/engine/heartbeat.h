#pragma once
// The record an unfinished registration or verification keeps in the
// repository settings (a registration's plan, a verification's run; decision
// 0022), written again while the operation works, with the time in
// "updated". Another process, such as the GUI's Archive List, reads it to tell
// an operation that works from one that stopped: the checkpoints alone move
// only when a file finishes, and a large file takes minutes.
//
// Heartbeat sits between an engine and its progress sink and writes when a
// progress report comes and 30 seconds have passed. The reports come often
// enough: scanning reports at least once a second while it finds entries, and
// hashing about five times a second, also while one large file is read.
//
// A write is one small UPDATE, made on the engine's own thread between its
// transactions. It is best effort: a write that fails (the database is busy)
// is left out and the operation goes on.
//
// An attempt that ends without finishing can say so with stop(), so that its
// record reads as stopped at once ("stopped").
//
// A registration's plan is written only while the revision it creates is not
// recorded (only_while_unrecorded): another attempt of the same registration
// may record it first, and then the key may hold the plan of the archive's
// next registration (decision 0023).
#include "common/time.h"
#include "core/interfaces.h"
#include "core/repository/archive_store.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <exception>
#include <optional>
#include <string>
#include <utility>

namespace va {

class Heartbeat final : public IProgressSink {
public:
    using Clock = std::chrono::steady_clock;

    // `record`: the setting's value, which every write repeats with the time
    // in "updated". The setting `key` must exist already: only an existing
    // record is changed, so a write after the operation removed its record
    // (it finished) cannot bring it back. `forward`: where the reports go.
    Heartbeat(ArchiveStore& store, std::string key, nlohmann::json record, IProgressSink& forward,
        Clock::duration interval = std::chrono::seconds(30))
        : store_(store), key_(std::move(key)), record_(std::move(record)), forward_(forward), interval_(interval),
          last_(Clock::now())
    {
    }

    void on_progress(const Progress& progress) override
    {
        forward_.on_progress(progress);
        if (Clock::now() - last_ >= interval_) {
            beat();
        }
    }

    // Sets a member of the record (such as a total), to be written with the next beat.
    void set(const char* name, nlohmann::json value) { record_[name] = std::move(value); }

    // For a registration's plan: from now on every write changes the record
    // only while this revision of the archive is not recorded. Once it is,
    // by this attempt or by another attempt of the same registration, the
    // plan is done with (recording removes it), and the key may hold the plan
    // of the archive's next registration, which this record must not replace.
    void only_while_unrecorded(Uuid archive_uuid, std::uint32_t revision)
    {
        unrecorded_ = UnrecordedRevision{std::move(archive_uuid), revision};
    }

    // Writes the record now, with the current time.
    void beat() noexcept
    {
        last_ = Clock::now();
        // An exception must not reach the engine through on_progress (it would
        // leave the hash pipeline's threads running), and this write is not
        // worth failing the operation for.
        try {
            record_["updated"] = current_timestamp();
            if (unrecorded_) {
                (void)store_.update_setting_unless_recorded(
                    key_, record_.dump(), unrecorded_->archive_uuid, unrecorded_->revision);
            } else {
                (void)store_.update_setting(key_, record_.dump());
            }
        } catch (const std::exception&) {
        }
    }

    // Writes the record a last time with the time the operation stopped
    // ("stopped"), for an attempt that ends without finishing (cancelled,
    // its device removed, a failure): readers then see at once that it
    // stopped, instead of taking it for working until its "updated" is
    // minutes old. Not for an operation that finished: a registration's key
    // is its archive's, and once its record is removed the key may hold the
    // plan of the archive's next registration, which this record would
    // replace (update_setting only changes a record that exists, so with the
    // key empty it writes nothing; only_while_unrecorded keeps a plan's
    // record from it once the revision is recorded, by any attempt).
    void stop() noexcept
    {
        try {
            record_["stopped"] = current_timestamp();
        } catch (const std::exception&) {
            return;
        }
        beat();
    }

private:
    struct UnrecordedRevision {
        Uuid archive_uuid;
        std::uint32_t revision = 0;
    };

    ArchiveStore& store_;
    std::string key_;
    nlohmann::json record_;
    IProgressSink& forward_;
    Clock::duration interval_;
    Clock::time_point last_;
    std::optional<UnrecordedRevision> unrecorded_;
};

} // namespace va
