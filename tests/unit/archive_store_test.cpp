#include "test_support.h"

#include "common/time.h"
#include "core/hash/hashers.h"
#include "test_repository.h"

#include <chrono>

namespace {

using test::TestRepository;

va::VerifyRunRecord run(std::string id, std::string result)
{
    va::VerifyRunRecord record;
    record.run_id = std::move(id);
    record.archive_uuid = "28ff645e-a220-4aac-ae0b-6c328bf1a2b3";
    record.revision = 1;
    record.mode = "FULL";
    record.started = "2026-10-08T00:00:00Z";
    record.finished = "2026-10-08T01:00:00Z";
    record.result = std::move(result);
    record.coverage_bytes = 123;
    record.counts_json = "{\"verified\":3}";
    return record;
}

void archives_revisions_and_labels()
{
    TestRepository repository;
    auto store = va::ArchiveStore::open(repository.context.repo);
    CHECK(store.has_value());
    if (!store) {
        return;
    }
    const auto first_label = store->next_label();
    const auto second_label = store->next_label();
    CHECK(first_label && *first_label == "ARCH-0001");
    CHECK(second_label && *second_label == "ARCH-0002");

    va::ArchiveRecord archive{"28ff645e-a220-4aac-ae0b-6c328bf1a2b3", "ARCH-0001",
        "2026-10-08T00:00:00Z", 1, "REGISTERED", "Backups/2026", ""};
    CHECK(store->insert_archive(archive));
    archive.status = "RETIRED";
    CHECK(store->update_archive(archive));
    const auto found = store->find_archive(archive.archive_uuid);
    CHECK(found && *found && (*found)->status == "RETIRED" && (*found)->root_subpath == "Backups/2026");
    CHECK(store->archives() && store->archives()->size() == 1);

    va::RevisionRecord revision{archive.archive_uuid, 1, "manifests/x/r0001.vam", "{}", "h", "b", "",
        "2026-10-08T00:00:00Z", ""};
    CHECK(store->insert_revision(revision));
    revision.revision = 2;
    CHECK(store->insert_revision(revision));
    const auto revisions = store->revisions(archive.archive_uuid);
    CHECK(revisions && revisions->size() == 2 && revisions->back().revision == 2);

    va::Entry entry;
    entry.path.p = "a.txt";
    entry.size = 5;
    entry.b3 = "b3";
    CHECK(store->insert_file_rows(archive.archive_uuid, 1, {entry}, 0));
    CHECK(store->insert_evidence({archive.archive_uuid, "", "2026-10-08T00:00:00Z", "{}", "MATCH", ""}));
}

// R-18 / R-90: an edited run is detected, and so is a deleted run that a later
// run follows.
void history_chain_detects_tampering()
{
    TestRepository repository;
    auto store = va::ArchiveStore::open(repository.context.repo);
    if (!store) {
        CHECK(false);
        return;
    }
    const auto& repository_uuid = repository.context.repository_uuid;
    const auto first = store->append_verify_run(run("run-1", "PASSED"), repository_uuid);
    CHECK(first && first->prev_hash == repository_uuid);
    const auto second = store->append_verify_run(run("run-2", "FAILED"), repository_uuid);
    CHECK(second && first && second->prev_hash == first->row_hash);
    CHECK(store->append_verify_run(run("run-3", "PASSED"), repository_uuid));

    auto check = store->check_history(repository_uuid);
    CHECK(check && check->runs == 3 && !check->first_broken_run);

    repository.execute("UPDATE verify_runs SET result = 'PASSED' WHERE run_id = 'run-2'");
    check = store->check_history(repository_uuid);
    CHECK(check && check->first_broken_run == std::optional<std::string>("run-2"));

    repository.execute("UPDATE verify_runs SET result = 'FAILED' WHERE run_id = 'run-2'");
    repository.execute("DELETE FROM verify_runs WHERE run_id = 'run-2'");
    check = store->check_history(repository_uuid);
    CHECK(check && check->first_broken_run == std::optional<std::string>("run-3"));
}

// What the chain cannot tell in V1, as `repo verify-history`, the GUI and the
// guides say (app::history_chain_limits): the newest run removed leaves a
// chain that holds, and so does a run changed by someone who computes its
// hash again (and every later run's); V1.1's signed checkpoints (R-76) are
// planned for that. Sample runs.
void history_chain_limits_in_v1()
{
    TestRepository repository;
    auto store = repository.store();
    const auto& repository_uuid = repository.context.repository_uuid;
    CHECK(store.append_verify_run(run("run-1", "PASSED"), repository_uuid));
    CHECK(store.append_verify_run(run("run-2", "FAILED"), repository_uuid));
    repository.execute("DELETE FROM verify_runs WHERE run_id = 'run-2'");
    auto check = store.check_history(repository_uuid);
    CHECK(check && check->runs == 1 && !check->first_broken_run);

    auto failed = store.append_verify_run(run("run-3", "FAILED"), repository_uuid);
    CHECK(failed.has_value());
    if (!failed) {
        return;
    }
    failed->result = "PASSED";
    const auto rehashed = va::blake3_hex(failed->prev_hash + va::verify_run_hash_input(*failed));
    repository.execute(
        ("UPDATE verify_runs SET result = 'PASSED', row_hash = '" + rehashed + "' WHERE run_id = 'run-3'").c_str());
    check = store.check_history(repository_uuid);
    CHECK(check && check->runs == 2 && !check->first_broken_run);
    const auto runs = store.verify_runs("28ff645e-a220-4aac-ae0b-6c328bf1a2b3");
    CHECK(runs && runs->size() == 2 && runs->back().result == "PASSED");
}

// The window's History (decision 0046): every archive's runs in the order
// they were recorded, and the media evidence recorded with a run, which its
// result page shows. Reading them leaves the chain as it was. Sample runs
// and archives (28ff645e-..., c0ffee00-...).
void history_across_archives()
{
    TestRepository repository;
    auto store = repository.store();
    const auto& repository_uuid = repository.context.repository_uuid;
    auto other = run("run-2", "FAILED");
    other.archive_uuid = "c0ffee00-0000-4000-8000-000000000002";
    CHECK(store.append_verify_run(run("run-1", "PASSED"), repository_uuid));
    CHECK(store.append_verify_run(other, repository_uuid));
    CHECK(store.append_verify_run(run("run-3", "INCOMPLETE"), repository_uuid));
    const auto all = store.all_verify_runs();
    CHECK(all && all->size() == 3);
    CHECK(all && all->size() == 3 && (*all)[0].run_id == "run-1" && (*all)[1].run_id == "run-2" &&
          (*all)[1].archive_uuid == other.archive_uuid && (*all)[2].result == "INCOMPLETE");
    const auto one = store.verify_runs("28ff645e-a220-4aac-ae0b-6c328bf1a2b3");
    CHECK(one && one->size() == 2);
    const auto check = store.check_history(repository_uuid);
    CHECK(check && check->runs == 3 && !check->first_broken_run);

    // The evidence of a run: the last that names it; none for another run.
    CHECK(store.insert_evidence({other.archive_uuid, "run-2", "2026-10-08T00:00:00Z", "{\"label\":\"SAMPLE-A\"}",
        "MATCH_WEAK", ""}));
    CHECK(store.insert_evidence({other.archive_uuid, "run-2", "2026-10-08T00:30:00Z", "{\"label\":\"SAMPLE-B\"}",
        "MATCH", "sample note"}));
    CHECK(store.insert_evidence({other.archive_uuid, "", "2026-10-08T00:40:00Z", "{}", "UNKNOWN", ""}));
    const auto evidence = store.run_evidence("run-2");
    CHECK(evidence && *evidence && (*evidence)->match_result == "MATCH" &&
          (*evidence)->volume_json == "{\"label\":\"SAMPLE-B\"}" && (*evidence)->operator_note == "sample note" &&
          (*evidence)->run_id == "run-2" && (*evidence)->archive_uuid == other.archive_uuid);
    const auto none = store.run_evidence("run-1");
    CHECK(none && !*none);
}

void items_and_progress()
{
    TestRepository repository;
    auto store = va::ArchiveStore::open(repository.context.repo);
    if (!store) {
        CHECK(false);
        return;
    }
    va::VerifyItemRecord item;
    item.run_id = "run-1";
    item.path.p = "a.txt";
    item.status = "Modified";
    CHECK(store->insert_verify_items({item}));
    const auto items = store->verify_items("run-1");
    CHECK(items && items->size() == 1 && items->front().status == "Modified");

    va::Entry entry;
    entry.path.p = "dir/b.txt";
    entry.path.raw = "ff";
    CHECK(store->add_progress("run-1", {va::entry_key(entry)}));
    auto progress = store->progress("run-1");
    CHECK(progress && progress->contains(va::entry_key(entry)));
    CHECK(store->clear_progress("run-1"));
    progress = store->progress("run-1");
    CHECK(progress && progress->empty());
}

va::Entry file_entry(std::string path, std::uint64_t size)
{
    va::Entry entry;
    entry.path.p = std::move(path);
    entry.size = size;
    entry.b3 = "b3";
    entry.sha256 = "sha256";
    return entry;
}

// Registrations that have not finished, as the register engine leaves them:
// the plan in settings, hashed files checkpointed under the new revision.
// Sample roots only (Z:, Y:), never a real volume's.
void pending_registrations_with_checkpoints()
{
    TestRepository repository;
    auto store = repository.store();
    const std::string whole = "c0ffee00-0000-4000-8000-000000000001";
    const std::string folder = "c0ffee00-0000-4000-8000-000000000002";
    CHECK(store.insert_archive({whole, "ARCH-0001", "2026-10-15T01:00:00Z", 0, "REGISTRATION_INCOMPLETE", "", ""}));
    CHECK(store.insert_archive({folder, "ARCH-0002", "2026-10-15T01:00:20Z", 2, "REGISTERED", "範例/Photos", ""}));
    CHECK(store.set_setting("registration:" + whole,
        R"({"archive_uuid":")" + whole +
            R"(","created":"2026-10-15T01:00:00Z","label":"ARCH-0001","new_archive":true,"revision":1,"root":"Z:\\"})"));
    // A new revision of a folder archive, from a plan that records its totals
    // and that its attempt stopped.
    CHECK(store.set_setting("registration:" + folder,
        R"({"archive_uuid":")" + folder +
            R"(","created":"2026-10-15T01:00:20Z","label":"ARCH-0002","new_archive":false,"revision":3,)"
            R"("root":"Y:\\範例\\Photos","files_total":10,"bytes_total":5000,"updated":"2026-10-15T02:00:00Z",)"
            R"("stopped":"2026-10-15T02:00:00Z"})"));
    // Damaged settings are passed over, not fatal.
    CHECK(store.set_setting("registration:broken", "not json"));
    CHECK(store.set_setting("registration:odd", R"({"archive_uuid":"x","revision":"1"})"));

    CHECK(store.upsert_file_rows(whole, 1, {file_entry("a.mkv", 1000), file_entry("b.mkv", 2500)}, 0));
    CHECK(store.upsert_file_rows(folder, 3, {file_entry("c.jpg", 40)}, 0));
    // The revision before, fully indexed: not part of the progress.
    CHECK(store.insert_file_rows(folder, 2, {file_entry("old.jpg", 999)}, 0));

    const auto pending = store.pending_registrations();
    CHECK(pending && pending->size() == 2);
    if (!pending || pending->size() != 2) {
        return;
    }
    const auto& first = pending->front();  // the older plan first
    CHECK(first.archive_uuid == whole && first.label == "ARCH-0001" && first.root == "Z:\\");
    CHECK(first.volume_root && first.revision == 1 && first.started == "2026-10-15T01:00:00Z");
    CHECK(first.done_counted && first.files_done == 2 && first.bytes_done == 3500);
    CHECK(!first.files_total && !first.bytes_total && first.updated.empty() && first.stopped.empty());
    const auto& second = pending->back();
    CHECK(second.archive_uuid == folder && !second.volume_root && second.revision == 3);
    CHECK(second.root == "Y:\\範例\\Photos");
    CHECK(second.files_done == 1 && second.bytes_done == 40);
    CHECK(second.files_total == std::optional<std::uint64_t>(10) &&
          second.bytes_total == std::optional<std::uint64_t>(5000));
    CHECK(second.updated == "2026-10-15T02:00:00Z" && second.stopped == "2026-10-15T02:00:00Z");

    // A finished registration removes its plan; nothing is pending then.
    CHECK(store.remove_setting("registration:" + whole));
    CHECK(store.remove_setting("registration:" + folder));
    const auto none = store.pending_registrations();
    CHECK(none && none->empty());
}

const std::string verified_archive = "28ff645e-a220-4aac-ae0b-6c328bf1a2b3";

// An archive at `revision` whose revision 1 has three files and a stream
// (650 bytes in all), as unfinished verifications of it need.
void add_verified_archive(va::ArchiveStore& store, std::uint32_t revision = 1)
{
    CHECK(store.insert_archive({verified_archive, "ARCH-0007", "2026-10-08T00:00:00Z", revision, "REGISTERED", "", ""}));
    CHECK(store.insert_revision({verified_archive, 1, "manifests/x/r0001.vam",
        R"({"counts":{"files":3,"dirs":1,"links":0,"streams":1,"chunks":0,"bytes":650},"root_hint":"Z:\\範例"})",
        "h", "b", "", "2026-10-08T00:00:00Z", ""}));
    auto stream = file_entry("c.txt", 50);
    stream.type = 's';
    stream.stream = "Zone.Identifier";
    CHECK(store.insert_file_rows(verified_archive, 1,
        {file_entry("a.txt", 100), file_entry("b.txt", 200), file_entry("c.txt", 300), stream}, 0));
}

// The setting a verification of the sample archive's revision 1 leaves.
void add_run_setting(va::ArchiveStore& store, const std::string& run_id, const std::string& started,
    const std::string& more = "")
{
    CHECK(store.set_setting("run:" + run_id, R"({"archive_uuid":")" + verified_archive +
                                                  R"(","revision":1,"started":")" + started + R"(")" + more + "}"));
}

// A recorded run of the sample archive, with `counts_json` when given.
void record_run(va::ArchiveStore& store, const TestRepository& repository, const std::string& run_id,
    const std::string& result, const std::string& started, const std::string& finished,
    const std::string& counts_json = "")
{
    auto record = run(run_id, result);
    record.started = started;
    record.finished = finished;
    if (!counts_json.empty()) {
        record.counts_json = counts_json;
    }
    CHECK(store.append_verify_run(record, repository.context.repository_uuid));
}

// The time the unfinished verifications are read at, unless a test says
// otherwise: long after the runs below said anything.
std::chrono::sys_seconds at(const char* timestamp)
{
    return va::parse_timestamp(timestamp).value_or(std::chrono::sys_seconds{});
}
const auto read_at = at("2026-10-20T12:00:00Z");

std::vector<std::string> unfinished_run_ids(va::ArchiveStore& store, std::chrono::sys_seconds now = read_at)
{
    std::vector<std::string> ids;
    if (const auto runs = store.unfinished_verifications(now)) {
        for (const auto& run : *runs) {
            ids.push_back(run.run_id);
        }
    } else {
        CHECK(false);
    }
    return ids;
}

// Verifications that have not finished: one running (no history row yet), one
// recorded INCOMPLETE, and one finished earlier whose leftover setting is
// passed over. The two unfinished ones overlap in time, so neither is a later
// attempt that supersedes the other.
void unfinished_verifications_with_checkpoints()
{
    TestRepository repository;
    auto store = repository.store();
    add_verified_archive(store);
    auto stream = file_entry("c.txt", 50);
    stream.type = 's';
    stream.stream = "Zone.Identifier";

    add_run_setting(store, "done-1", "2026-10-13T03:00:00Z");
    record_run(store, repository, "done-1", "PASSED", "2026-10-13T03:00:00Z", "2026-10-13T04:00:00Z");

    add_run_setting(store, "running-1", "2026-10-15T03:00:00Z");
    CHECK(store.add_progress("running-1", {va::entry_key(file_entry("a.txt", 0)), va::entry_key(stream)}));

    add_run_setting(store, "stopped-1", "2026-10-15T02:00:00Z", R"(,"root":"Y:\\範例")");
    CHECK(store.add_progress("stopped-1", {va::entry_key(file_entry("c.txt", 0))}));
    record_run(store, repository, "stopped-1", "INCOMPLETE", "2026-10-15T02:00:00Z", "2026-10-15T03:30:00Z");

    const auto runs = store.unfinished_verifications(read_at);
    CHECK(runs && runs->size() == 2);
    if (!runs || runs->size() != 2) {
        return;
    }
    const auto& interrupted = runs->front();  // started first
    CHECK(interrupted.run_id == "stopped-1" && interrupted.recorded_incomplete && interrupted.resumable);
    CHECK(interrupted.root == "Y:\\範例" && interrupted.label == "ARCH-0007");
    CHECK(interrupted.files_done == 1 && interrupted.bytes_done == 300);
    const auto& running = runs->back();
    CHECK(running.run_id == "running-1" && !running.recorded_incomplete && running.revision == 1);
    CHECK(running.resumable);
    CHECK(running.done_counted && running.files_done == 2 && running.bytes_done == 150);
    // No root and no totals in the setting (a run of an older version): they
    // are in its revision's manifest header, which the store does not read,
    // and never taken from the index's copy of it (R-14).
    CHECK(running.root.empty() && !running.totals_recorded && running.files_total == 0 &&
          running.bytes_total == 0);
    CHECK(!interrupted.totals_recorded); // its setting has a root, but no totals either

    // Recorded INCOMPLETE, it never changes: a count from an earlier read is
    // taken as it is, the checkpoints are not counted again. A running one is.
    const auto again =
        store.unfinished_verifications(read_at, {{"stopped-1", {7, 7000}}, {"running-1", {9, 9000}}});
    CHECK(again && again->size() == 2);
    if (again && again->size() == 2) {
        CHECK(again->front().files_done == 7 && again->front().bytes_done == 7000);
        CHECK(again->back().files_done == 2 && again->back().bytes_done == 150);
    }
}

// The record a verification of this version writes: its root and totals
// (the files whose content is read, decision 0022) and its "updated".
void unfinished_verification_totals_from_its_record()
{
    TestRepository repository;
    auto store = repository.store();
    add_verified_archive(store);
    add_run_setting(store, "run-1", "2026-10-15T03:00:00Z",
        R"(,"root":"Y:\\範例","files_total":3,"bytes_total":600,"updated":"2026-10-15T03:30:00Z")");
    const auto runs = store.unfinished_verifications(read_at);
    CHECK(runs && runs->size() == 1);
    if (runs && runs->size() == 1) {
        const auto& run = runs->front();
        CHECK(run.root == "Y:\\範例" && run.totals_recorded && run.files_total == 3 && run.bytes_total == 600);
        CHECK(run.updated == "2026-10-15T03:30:00Z");
    }
}

// Stopped runs that nothing will finish are not listed (decision 0022).
void superseded_verifications_are_left_out()
{
    {
        // Cancelled, then a later run of the archive finished: the newer
        // result stands, and so does a later attempt that stopped as well.
        TestRepository repository;
        auto store = repository.store();
        add_verified_archive(store);
        add_run_setting(store, "old-1", "2026-10-10T00:00:00Z");
        record_run(store, repository, "old-1", "INCOMPLETE", "2026-10-10T00:00:00Z", "2026-10-10T01:00:00Z");
        record_run(store, repository, "full-1", "PASSED", "2026-10-11T00:00:00Z", "2026-10-11T05:00:00Z");
        add_run_setting(store, "old-2", "2026-10-12T00:00:00Z");
        record_run(store, repository, "old-2", "INCOMPLETE", "2026-10-12T00:00:00Z", "2026-10-12T01:00:00Z");
        add_run_setting(store, "old-3", "2026-10-13T00:00:00Z");
        record_run(store, repository, "old-3", "INCOMPLETE", "2026-10-13T00:00:00Z", "2026-10-13T01:00:00Z");
        CHECK(unfinished_run_ids(store) == std::vector<std::string>{"old-3"});

        // A verification started after the newest stopped one supersedes it
        // while it runs.
        add_run_setting(store, "now-1", "2026-10-14T00:00:00Z", R"(,"updated":"2026-10-14T00:10:00Z")");
        CHECK(unfinished_run_ids(store) == std::vector<std::string>{"now-1"});
    }
    {
        // A run of a revision the archive has left cannot be resumed (R-104).
        // Recorded INCOMPLETE, it is not listed. Never recorded, it is listed
        // while it may still work, though it could not be resumed if it
        // stopped; once its record has said nothing for 5 minutes (its process
        // was killed), it is not. A run of an older version never says it
        // works ("updated"), so it may still work and stays listed.
        TestRepository repository;
        auto store = repository.store();
        add_verified_archive(store, 2);
        add_run_setting(store, "old-revision", "2026-10-10T00:00:00Z");
        record_run(store, repository, "old-revision", "INCOMPLETE", "2026-10-10T00:00:00Z", "2026-10-10T01:00:00Z");
        add_run_setting(store, "working", "2026-10-15T00:00:00Z", R"(,"updated":"2026-10-15T00:20:00Z")");
        const auto runs = store.unfinished_verifications(at("2026-10-15T00:21:00Z"));
        CHECK(runs && runs->size() == 1);
        if (runs && runs->size() == 1) {
            CHECK(runs->front().run_id == "working" && !runs->front().resumable);
        }
        CHECK(unfinished_run_ids(store, at("2026-10-15T00:24:59Z")) == std::vector<std::string>{"working"});
        CHECK(unfinished_run_ids(store, at("2026-10-15T00:25:00Z")).empty());
        add_run_setting(store, "older-version", "2026-10-16T00:00:00Z");
        CHECK(unfinished_run_ids(store) == std::vector<std::string>{"older-version"});
    }
    {
        // Never recorded (its process was killed): superseded by a later
        // verification that started, worked or was recorded when it had been
        // silent for minutes; not by one that started, or even finished, while
        // it still said it was working.
        TestRepository repository;
        auto store = repository.store();
        add_verified_archive(store);
        add_run_setting(store, "killed", "2026-10-10T00:00:00Z", R"(,"updated":"2026-10-10T02:00:00Z")");
        add_run_setting(store, "alive", "2026-10-12T00:00:00Z", R"(,"updated":"2026-10-12T04:01:00Z")");
        record_run(store, repository, "beside", "PASSED", "2026-10-12T02:58:00Z", "2026-10-12T04:00:00Z");
        CHECK(unfinished_run_ids(store, at("2026-10-12T04:01:30Z")) == std::vector<std::string>{"alive"});
    }
    {
        // Killed at 00:30 (its console window closed) and started again two
        // minutes later. Until the killed run has been silent for 5 minutes
        // both are listed: either may be the one that works. Then the new
        // attempt is the one to follow, while it works and once it is
        // recorded, here PASSED at 05:00.
        TestRepository repository;
        auto store = repository.store();
        add_verified_archive(store);
        add_run_setting(store, "killed", "2026-10-10T00:00:00Z", R"(,"updated":"2026-10-10T00:30:00Z")");
        add_run_setting(store, "again", "2026-10-10T00:32:00Z", R"(,"updated":"2026-10-10T00:34:30Z")");
        CHECK(unfinished_run_ids(store, at("2026-10-10T00:34:40Z")) ==
              (std::vector<std::string>{"killed", "again"}));
        add_run_setting(store, "again", "2026-10-10T00:32:00Z", R"(,"updated":"2026-10-10T00:35:00Z")");
        CHECK(unfinished_run_ids(store, at("2026-10-10T00:35:10Z")) == std::vector<std::string>{"again"});
        CHECK(store.remove_setting("run:again"));
        record_run(store, repository, "again", "PASSED", "2026-10-10T00:32:00Z", "2026-10-10T05:00:00Z");
        CHECK(unfinished_run_ids(store).empty());
    }
    {
        // A run of an older version never says it works, so a verification
        // recorded long after it started says nothing of it: only one started
        // 5 minutes or more after it supersedes it.
        TestRepository repository;
        auto store = repository.store();
        add_verified_archive(store);
        add_run_setting(store, "older", "2026-10-10T00:00:00Z");
        record_run(store, repository, "before", "PASSED", "2026-10-09T23:58:00Z", "2026-10-10T03:00:00Z");
        CHECK(unfinished_run_ids(store) == std::vector<std::string>{"older"});
        record_run(store, repository, "later", "PASSED", "2026-10-10T00:05:00Z", "2026-10-10T04:00:00Z");
        CHECK(unfinished_run_ids(store).empty());
    }
    {
        // A verification recorded within minutes of the kill, such as one
        // that ended at once, did not do the killed run's work: it stays.
        TestRepository repository;
        auto store = repository.store();
        add_verified_archive(store);
        add_run_setting(store, "killed", "2026-10-10T00:00:00Z", R"(,"updated":"2026-10-10T00:30:00Z")");
        record_run(store, repository, "brief", "FAILED", "2026-10-10T00:31:00Z", "2026-10-10T00:34:00Z");
        CHECK(unfinished_run_ids(store) == std::vector<std::string>{"killed"});
    }
    {
        // Killed at 00:30 and started again two minutes later; the restart, of
        // a small archive, PASSED at 00:34, before the killed run had been
        // silent for 5 minutes. Once it has, the restart that passed
        // supersedes it, a month and a year later too.
        TestRepository repository;
        auto store = repository.store();
        add_verified_archive(store);
        add_run_setting(store, "killed", "2026-10-10T00:00:00Z", R"(,"updated":"2026-10-10T00:30:00Z")");
        record_run(store, repository, "again", "PASSED", "2026-10-10T00:32:00Z", "2026-10-10T00:34:00Z");
        CHECK(unfinished_run_ids(store, at("2026-10-10T00:34:59Z")) == std::vector<std::string>{"killed"});
        CHECK(unfinished_run_ids(store, at("2026-10-10T00:35:00Z")).empty());
        CHECK(unfinished_run_ids(store, at("2026-11-10T00:35:00Z")).empty());
        CHECK(unfinished_run_ids(store, at("2027-10-10T00:35:00Z")).empty());
    }
    {
        // One that passed but started before the killed run last said
        // anything ran beside it, and did not take it up: it stays. One that
        // passed with warnings after the kill supersedes it.
        TestRepository repository;
        auto store = repository.store();
        add_verified_archive(store);
        add_run_setting(store, "killed", "2026-10-10T00:00:00Z", R"(,"updated":"2026-10-10T00:30:00Z")");
        record_run(store, repository, "beside", "PASSED", "2026-10-10T00:29:00Z", "2026-10-10T00:33:00Z");
        CHECK(unfinished_run_ids(store) == std::vector<std::string>{"killed"});
        record_run(store, repository, "warned", "PASSED_WITH_WARNINGS", "2026-10-10T00:31:00Z",
            "2026-10-10T00:33:30Z");
        CHECK(unfinished_run_ids(store).empty());
    }
    // A verification that read no media, whose counts say so (decision 0028:
    // it failed on its manifest before it looked for the media), did none of
    // an unfinished run's work and is no newer attempt to follow: it
    // supersedes neither a run recorded INCOMPLETE before it nor one whose
    // process was killed long before it ended. A later verification that read
    // the media, whatever its result, does. Sample data only.
    const std::string read_no_media = R"({"items":{},"media_read":false})";
    {
        TestRepository repository;
        auto store = repository.store();
        add_verified_archive(store);
        add_run_setting(store, "interrupted", "2026-10-10T00:00:00Z", R"(,"updated":"2026-10-10T01:00:00Z")");
        record_run(store, repository, "interrupted", "INCOMPLETE", "2026-10-10T00:00:00Z", "2026-10-10T01:00:00Z");
        record_run(store, repository, "unread", "FAILED", "2026-10-11T00:00:00Z", "2026-10-11T00:00:01Z",
            read_no_media);
        CHECK(unfinished_run_ids(store) == std::vector<std::string>{"interrupted"});
        record_run(store, repository, "read", "FAILED", "2026-10-12T00:00:00Z", "2026-10-12T00:10:00Z");
        CHECK(unfinished_run_ids(store).empty());
    }
    {
        TestRepository repository;
        auto store = repository.store();
        add_verified_archive(store);
        add_run_setting(store, "killed", "2026-10-10T00:00:00Z", R"(,"updated":"2026-10-10T00:30:00Z")");
        record_run(store, repository, "unread", "FAILED", "2026-10-10T02:00:00Z", "2026-10-10T02:00:01Z",
            read_no_media);
        CHECK(unfinished_run_ids(store) == std::vector<std::string>{"killed"});
        record_run(store, repository, "read", "FAILED", "2026-10-10T03:00:00Z", "2026-10-10T03:10:00Z");
        CHECK(unfinished_run_ids(store).empty());
    }
}

// A retired archive can still be verified, and its interrupted verifications
// resumed (decision 0003); only where its latest manifest is lost can they
// never be resumed, which read_unfinished_operations judges from that manifest
// (decision 0022, point 5). So each kind of unfinished run of the sample
// archive, alone, is kept once the archive is retired, marked as a retired
// archive's: recorded INCOMPLETE, killed (its record silent for minutes),
// working (its record said so a minute ago), and of an older version (no
// "updated"). Its record and checkpoints stay as they were, and another
// archive's run is kept as before, unmarked. Sample data only.
void a_retired_archives_verifications_are_kept_and_marked()
{
    const auto now = at("2026-10-10T02:01:00Z");
    const std::string other = "c0ffee00-0000-4000-8000-000000000003";
    // The runs kept, each with whether its archive is retired.
    using Kept = std::vector<std::pair<std::string, bool>>;
    const auto kept = [&](va::ArchiveStore& store) {
        Kept runs;
        if (const auto unfinished = store.unfinished_verifications(now)) {
            for (const auto& run : *unfinished) {
                runs.emplace_back(run.run_id, run.archive_retired);
            }
        } else {
            CHECK(false);
        }
        return runs;
    };
    using AddRun = void (*)(va::ArchiveStore&, const TestRepository&);
    const AddRun kinds[] = {
        [](va::ArchiveStore& store, const TestRepository& repository) {
            add_run_setting(store, "run-1", "2026-10-10T00:00:00Z", R"(,"updated":"2026-10-10T01:00:00Z")");
            record_run(store, repository, "run-1", "INCOMPLETE", "2026-10-10T00:00:00Z", "2026-10-10T01:00:00Z");
        },
        [](va::ArchiveStore& store, const TestRepository&) {
            add_run_setting(store, "run-1", "2026-10-10T00:00:00Z", R"(,"updated":"2026-10-10T01:00:00Z")");
        },
        [](va::ArchiveStore& store, const TestRepository&) {
            add_run_setting(store, "run-1", "2026-10-10T00:00:00Z", R"(,"updated":"2026-10-10T02:00:00Z")");
        },
        [](va::ArchiveStore& store, const TestRepository&) {
            add_run_setting(store, "run-1", "2026-10-10T00:00:00Z");
        },
    };
    for (const auto add_run : kinds) {
        TestRepository repository;
        auto store = repository.store();
        add_verified_archive(store);
        add_run(store, repository);
        CHECK(store.add_progress("run-1", {va::entry_key(file_entry("a.txt", 0))}));
        CHECK(store.insert_archive({other, "ARCH-0003", "2026-10-08T00:00:00Z", 1, "REGISTERED", "", ""}));
        CHECK(store.set_setting("run:other-1",
            R"({"archive_uuid":")" + other + R"(","revision":1,"started":"2026-10-10T00:30:00Z"})"));
        CHECK(kept(store) == (Kept{{"run-1", false}, {"other-1", false}}));
        const auto record = store.setting("run:run-1");
        const auto checkpoints = store.progress("run-1");
        CHECK(record && !record->empty() && checkpoints && checkpoints->size() == 1);

        auto archive = store.find_archive(verified_archive);
        CHECK(archive && *archive);
        if (!archive || !*archive) {
            continue;
        }
        (*archive)->status = "RETIRED";
        CHECK(store.update_archive(**archive));
        CHECK(kept(store) == (Kept{{"run-1", true}, {"other-1", false}}));
        CHECK(record && store.setting("run:run-1").value_or("") == *record);
        CHECK(checkpoints && store.progress("run-1").value_or(std::set<std::string>{}) == *checkpoints);
    }
}

// The mark of what the Archive List shows changes with everything the list
// shows of the archives, and not with the operations' own records and
// checkpoints, which the section of unfinished operations shows. Sample data
// only.
void archive_list_mark_follows_the_archives()
{
    TestRepository repository;
    auto store = repository.store();
    std::string last;
    const auto changed = [&] {
        const auto mark = store.archive_list_mark();
        CHECK(mark && !mark->empty());
        const bool different = mark && *mark != last;
        last = mark.value_or("");
        return different;
    };
    CHECK(changed());
    const std::string archive_uuid = "c0ffee00-0000-4000-8000-000000000001";
    va::ArchiveRecord archive{archive_uuid, "ARCH-0001", "2026-10-15T01:00:00Z", 0, "REGISTRATION_INCOMPLETE",
        "範例", ""};
    CHECK(store.insert_archive(archive));
    CHECK(changed());
    CHECK(!changed());

    CHECK(store.set_setting("registration:" + archive_uuid, R"({"archive_uuid":")" + archive_uuid + R"("})"));
    CHECK(store.upsert_file_rows(archive_uuid, 1, {file_entry("a.txt", 1)}, 0));
    CHECK(store.set_setting("run:run-1", "{}"));
    CHECK(store.add_progress("run-1", {va::entry_key(file_entry("a.txt", 1))}));
    CHECK(!changed());

    // Recorded: the revision, then the archive at it.
    CHECK(store.insert_revision(
        {archive_uuid, 1, "manifests/x/r0001.vam", "{}", "h", "b", "", "2026-10-15T02:00:00Z", ""}));
    CHECK(changed());
    archive.current_revision = 1;
    archive.status = "REGISTERED";
    CHECK(store.update_archive(archive));
    CHECK(changed());
    // A verification recorded, whatever its result.
    auto recorded = run("run-1", "INCOMPLETE");
    recorded.archive_uuid = archive_uuid;
    CHECK(store.append_verify_run(recorded, repository.context.repository_uuid));
    CHECK(changed());
    auto passed = run("run-2", "PASSED");
    passed.archive_uuid = archive_uuid;
    CHECK(store.append_verify_run(passed, repository.context.repository_uuid));
    CHECK(changed());
    // Retired; another label.
    archive.status = "RETIRED";
    CHECK(store.update_archive(archive));
    CHECK(changed());
    archive.label = "ARCH-0009";
    CHECK(store.update_archive(archive));
    CHECK(changed());
    // Another root.
    archive.root_subpath = "範例/Photos";
    CHECK(store.update_archive(archive));
    CHECK(changed());
}

// One read transaction for several reads: they all see the database as it was
// at the first, also when one of them takes its own (pending_registrations
// here), while another connection commits in between. In the rollback-journal
// mode, the reads hold no lock between queries: the writer commits at once,
// and the reads see it. Sample data only.
void reads_in_one_transaction()
{
    for (const bool write_ahead : {true, false}) {
        TestRepository repository;
        if (!write_ahead) {
            repository.execute("PRAGMA journal_mode=DELETE;");
        }
        auto writer = repository.store();
        auto reader = va::ArchiveStore::open_read_only(repository.context.repo);
        CHECK(reader.has_value());
        if (!reader) {
            continue;
        }
        std::string before;
        std::string after;
        bool written = false;
        const auto read = reader->read_transaction([&]() -> std::expected<void, va::IoError> {
            before = reader->archive_list_mark().value_or("");
            CHECK(reader->pending_registrations().has_value());
            written = writer
                          .insert_archive({"c0ffee00-0000-4000-8000-000000000002", "ARCH-0002",
                              "2026-10-15T01:00:00Z", 1, "REGISTERED", "", ""})
                          .has_value();
            after = reader->archive_list_mark().value_or("");
            return {};
        });
        CHECK(read.has_value() && written);
        CHECK((before == after) == write_ahead);
        CHECK(reader->archive_list_mark().value_or("") != before);
    }
}

// Watching opens the database read only: one that is missing reads as an
// error, and no empty database is left behind.
void read_only_open_creates_nothing()
{
    TestRepository repository;
    auto missing = repository.context.repo;
    missing.root = missing.root.parent_path() / "no-repository";
    std::filesystem::create_directories(missing.root);
    const auto opened = va::ArchiveStore::open_read_only(missing);
    CHECK(!opened);
    CHECK(!std::filesystem::exists(missing.database()));

    // The real one opens, and reads.
    auto store = va::ArchiveStore::open_read_only(repository.context.repo);
    CHECK(store.has_value());
    if (store) {
        CHECK(store->pending_registrations().has_value());
        CHECK(store->unfinished_verifications(read_at).has_value());
        // Writing is refused.
        CHECK(!store->set_setting("x", "y"));
    }
}

// In the rollback-journal mode (a database where WAL could not be turned on)
// the reads go without a snapshot, query by query, and still read it all but
// the checkpoints: counting them would hold the reader's lock for as long as
// it takes, and keep the operations from committing (decision 0022).
void reads_without_write_ahead_log()
{
    TestRepository repository;
    repository.execute("PRAGMA journal_mode=DELETE;");
    auto store = repository.store();
    add_verified_archive(store);
    add_run_setting(store, "run-1", "2026-10-15T03:00:00Z", R"(,"files_total":4,"bytes_total":650)");
    CHECK(store.add_progress("run-1", {va::entry_key(file_entry("a.txt", 0))}));
    CHECK(store.set_setting("registration:" + verified_archive,
        R"({"archive_uuid":")" + verified_archive + R"(","revision":2,"created":"2026-10-15T01:00:00Z"})"));
    CHECK(store.upsert_file_rows(verified_archive, 2, {file_entry("new.txt", 40)}, 0));
    auto watching = va::ArchiveStore::open_read_only(repository.context.repo);
    CHECK(watching.has_value());
    if (watching) {
        const auto registrations = watching->pending_registrations();
        CHECK(registrations && registrations->size() == 1);
        if (registrations && registrations->size() == 1) {
            const auto& registration = registrations->front();
            CHECK(!registration.done_counted && registration.files_done == 0 && registration.bytes_done == 0);
        }
        const auto runs = watching->unfinished_verifications(read_at);
        CHECK(runs && runs->size() == 1);
        if (runs && runs->size() == 1) {
            const auto& run = runs->front();
            CHECK(!run.done_counted && run.files_done == 0 && run.bytes_done == 0);
            CHECK(run.files_total == 4 && run.bytes_total == 650);  // the totals still show
        }
    }
    // Nothing is left open: the writer commits at once.
    CHECK(store.write_transaction([&] { return store.set_setting("after", "1"); }).has_value());
}

// A transaction whose writes fail is rolled back: nothing of it stays, and
// the next one can begin (a failed checkpoint must not leave it open).
void failed_write_transaction_is_rolled_back()
{
    TestRepository repository;
    auto store = repository.store();
    const std::string archive_uuid = "c0ffee00-0000-4000-8000-000000000002";
    const auto failed = store.write_transaction([&]() -> std::expected<void, va::IoError> {
        if (auto written = store.set_setting("checkpoint", "1"); !written) {
            return written;
        }
        // The same file twice: the primary key refuses the second.
        return store.insert_file_rows(archive_uuid, 1, {file_entry("a", 1), file_entry("a", 1)}, 0);
    });
    CHECK(!failed);
    CHECK(store.setting("checkpoint") == std::string{});
    const auto next = store.write_transaction([&] { return store.set_setting("checkpoint", "2"); });
    CHECK(next.has_value());
    CHECK(store.setting("checkpoint") == std::string("2"));
}

// update_setting changes a setting that exists and never creates one: a late
// "updated" from an operation that has finished cannot bring its record back.
void update_setting_never_creates()
{
    TestRepository repository;
    auto store = repository.store();
    CHECK(store.update_setting("run:gone", "{}"));
    CHECK(store.setting("run:gone") == std::string{});
    CHECK(store.set_setting("run:here", "1"));
    CHECK(store.update_setting("run:here", "2"));
    CHECK(store.setting("run:here") == std::string("2"));
}

// Decision 0030: an operation made for revision N of an archive (a
// registration creating N+1, a relocation against N) can never finish once
// the archive has a later revision than N while N+1 is not recorded, as after
// an import of N+2. Not while the archive is at N, nor once N+1 is recorded
// (the operation's revision was taken, decision 0023), nor for an archive
// without a record. Sample data: revision records without manifests.
void later_revision_of_an_archive()
{
    TestRepository repository;
    auto store = repository.store();
    const va::Uuid uuid = "c0ffee00-0000-4000-8000-000000000030";
    const auto record_revision = [&](std::uint32_t revision) {
        CHECK(store.insert_revision({uuid, revision, "manifests/" + uuid + "/r000" + std::to_string(revision) + ".vam",
            "{}", "h", "b", "", "2026-10-16T01:00:00Z", ""}));
        CHECK(store.update_archive({uuid, "ARCH-0030", "2026-10-16T00:00:00Z", revision, "REGISTERED", "", ""}));
    };
    const auto later = [&](const va::Uuid& archive_uuid, std::uint32_t made_for) {
        const auto found = store.later_revision(archive_uuid, made_for);
        CHECK(found.has_value());
        return found ? *found : std::optional<std::uint32_t>(999);
    };
    CHECK(later(uuid, 0) == std::nullopt);
    CHECK(store.insert_archive({uuid, "ARCH-0030", "2026-10-16T00:00:00Z", 0, "REGISTRATION_INCOMPLETE", "", ""}));
    record_revision(1);
    CHECK(later(uuid, 1) == std::nullopt);
    CHECK(later(uuid, 0) == std::nullopt); // revision 1 is recorded
    record_revision(3);
    CHECK(later(uuid, 1) == 3u);
    CHECK(later(uuid, 2) == std::nullopt); // revision 3 is recorded
    CHECK(later(uuid, 3) == std::nullopt);
    CHECK(later("c0ffee00-0000-4000-8000-0000000000ff", 0) == std::nullopt);
}

// Decision 0036: an automatic label skips the numbers of the labels that the
// repository has (sample archives): ARCH-0003 typed, ARCH-00004 imported (the
// same number as ARCH-0004), ARCH-0006 typed; ARCH-12X, which names no
// number, skips nothing. Labels stay without a uniqueness constraint (R-33):
// a label typed twice is recorded twice.
void automatic_labels_skip_the_labels_in_use()
{
    TestRepository repository;
    auto store = va::ArchiveStore::open(repository.context.repo);
    CHECK(store.has_value());
    if (!store) {
        return;
    }
    CHECK(store->next_label() == "ARCH-0001");
    for (const auto& [uuid, label] : std::vector<std::pair<std::string, std::string>>{
             {"c0ffee00-0000-4000-8000-000000000003", "ARCH-0003"},
             {"c0ffee00-0000-4000-8000-000000000004", "ARCH-00004"},
             {"c0ffee00-0000-4000-8000-000000000006", "ARCH-0006"},
             {"c0ffee00-0000-4000-8000-000000000016", "ARCH-0006"},
             {"c0ffee00-0000-4000-8000-000000000012", "ARCH-12X"}}) {
        CHECK(store->insert_archive({uuid, label, "2026-10-27T00:00:00Z", 1, "REGISTERED", "", ""}));
    }
    CHECK(store->next_label() == "ARCH-0002");
    CHECK(store->next_label() == "ARCH-0005");
    CHECK(store->next_label() == "ARCH-0007");
    CHECK(store->next_label() == "ARCH-0008");
}

// ---- Chunk hashes in the index (decision 0032) -----------------------------

constexpr std::uint64_t test_chunk_size = 64 * 1024;

// A file of `size` bytes as an index row, with `chunks` chunk hashes made up
// from `seed` (sample data: the store keeps them as given).
va::Entry chunked_entry(std::string path, std::uint64_t size, std::size_t chunks, const std::string& seed)
{
    auto entry = file_entry(std::move(path), size);
    entry.b3 = va::blake3_hex(seed);
    for (std::size_t idx = 0; idx < chunks; ++idx) {
        entry.chunks.push_back(va::blake3_hex(seed + "/" + std::to_string(idx)));
    }
    return entry;
}

std::string chunk_rows(const TestRepository& repository, const std::string& where)
{
    return repository.query("SELECT COUNT(*) FROM chunks WHERE " + where);
}

// A file's chunk hashes are indexed with its row, one row per chunk with its
// offset and length, and read back with it in idx order; every way that
// removes a file's row removes its chunk rows, and no other file's.
void chunk_rows_go_with_their_files()
{
    TestRepository repository;
    auto store = repository.store();
    const std::string archive_uuid = "c0ffee00-0000-4000-8000-000000000032";
    const auto big = chunked_entry("video/big.bin", 2 * test_chunk_size + 7, 3, "big");
    const auto other = chunked_entry("video/other.bin", test_chunk_size + 1, 2, "other");
    auto small = file_entry("small.txt", 5);
    small.b3 = va::blake3_hex("small");
    va::Entry folder;
    folder.type = 'd';
    folder.path.p = "empty";
    CHECK(store.insert_file_rows(archive_uuid, 1, {folder, small, big, other}, test_chunk_size));
    CHECK(chunk_rows(repository, "revision = 1") == "5");
    CHECK(repository.query("SELECT group_concat(idx || ':' || offset || ':' || length || ':' || b3, ' ') FROM "
                           "(SELECT * FROM chunks WHERE path = 'video/big.bin' ORDER BY idx)") ==
          "0:0:65536:" + big.chunks[0] + " 1:65536:65536:" + big.chunks[1] + " 2:131072:7:" + big.chunks[2]);
    CHECK(repository.query("SELECT COUNT(*) FROM chunks WHERE stream = '' AND raw_hex = '' AND "
                           "last_verified_at IS NULL") == "5");
    auto rows = store.file_rows(archive_uuid, 1);
    CHECK(rows && rows->size() == 4);
    if (rows && rows->size() == 4) {
        for (const auto& row : *rows) {
            const auto& expected = row.path.p == big.path.p ? big.chunks
                                   : row.path.p == other.path.p ? other.chunks
                                                                : std::vector<std::string>{};
            CHECK(row.chunks == expected);
        }
    }

    // A checkpoint again: the rows are replaced, not added.
    auto changed = big;
    changed.chunks[1] = va::blake3_hex("changed");
    CHECK(store.upsert_file_rows(archive_uuid, 1, {changed}, test_chunk_size));
    CHECK(chunk_rows(repository, "revision = 1") == "5");
    CHECK(chunk_rows(repository, "idx = 1 AND b3 = '" + changed.chunks[1] + "'") == "1");

    // A file whose chunk rows do not run from idx 0 without a gap is read
    // back without chunk hashes, as one that needs reading again.
    repository.execute("DELETE FROM chunks WHERE path = 'video/big.bin' AND idx = 1");
    rows = store.file_rows(archive_uuid, 1);
    CHECK(rows && rows->size() == 4);
    for (const auto& row : rows.value_or(std::vector<va::Entry>{})) {
        if (row.path.p == big.path.p) {
            CHECK(row.chunks.empty());
        }
        if (row.path.p == other.path.p) {
            CHECK(row.chunks == other.chunks);
        }
    }

    // Deleting rows by entry takes their chunk rows, and leaves the others.
    CHECK(store.insert_file_rows(archive_uuid, 2, {big}, test_chunk_size));
    CHECK(store.delete_file_rows(archive_uuid, 1, std::vector<va::Entry>{big}));
    CHECK(chunk_rows(repository, "revision = 1 AND path = 'video/big.bin'") == "0");
    CHECK(chunk_rows(repository, "revision = 1 AND path = 'video/other.bin'") == "2");
    CHECK(chunk_rows(repository, "revision = 2") == "3");
    // By revision: every row of it, of that revision only.
    CHECK(store.delete_file_rows(archive_uuid, 1));
    CHECK(chunk_rows(repository, "revision = 1") == "0");
    CHECK(chunk_rows(repository, "revision = 2") == "3");
}

// The chunk hashes of exactly the files that have chunk lines at the chunk
// size, all of them (decision 0031), or nothing is written.
void chunk_rows_fit_their_files()
{
    TestRepository repository;
    auto store = repository.store();
    const std::string archive_uuid = "c0ffee00-0000-4000-8000-000000000033";
    const auto big = chunked_entry("big.bin", 2 * test_chunk_size + 7, 3, "big");
    auto short_of_one = big;
    short_of_one.chunks.pop_back();
    const auto small_with_chunks = chunked_entry("small.bin", test_chunk_size, 1, "small");
    auto big_without = big;
    big_without.chunks.clear();
    const auto refused = [&](const std::vector<va::Entry>& entries, std::uint64_t chunk_size,
                             const std::string& message) {
        const auto written = store.insert_file_rows(archive_uuid, 1, entries, chunk_size);
        CHECK(!written && written.error().message == message);
        CHECK(repository.query("SELECT COUNT(*) FROM files") == "0");
        CHECK(chunk_rows(repository, "1") == "0");
    };
    refused({short_of_one}, test_chunk_size, "chunk hashes of big.bin: 2 given, 3 expected");
    refused({small_with_chunks}, test_chunk_size, "chunk hashes of small.bin: 1 given, 0 expected");
    refused({file_entry("a.txt", 1), big_without}, test_chunk_size, "chunk hashes of big.bin: 0 given, 3 expected");
    refused({big}, 0, "chunk hashes of big.bin: 3 given, 0 expected");
    refused({file_entry("a.txt", 1)}, 1000, "chunk_size 1000 is not 0 or a multiple of 65536 up to 1073741824");
    CHECK(store.upsert_file_rows(archive_uuid, 1, {big_without}, 0));
}

// archive_revisions.merkle_root holds the revision's Merkle root, and NULL
// for a revision without one (a manifest without a chunk size).
void merkle_roots_are_recorded()
{
    TestRepository repository;
    auto store = repository.store();
    const std::string archive_uuid = "c0ffee00-0000-4000-8000-000000000034";
    const auto root = va::blake3_hex("root");
    CHECK(store.insert_revision(
        {archive_uuid, 1, "manifests/x/r0001.vam", "{}", "h", "b", "", "2026-10-16T00:00:00Z", ""}));
    CHECK(store.insert_revision(
        {archive_uuid, 2, "manifests/x/r0002.vam", "{}", "h", "b", "", "2026-10-16T00:00:00Z", root}));
    const auto revisions = store.revisions(archive_uuid);
    CHECK(revisions && revisions->size() == 2);
    if (revisions && revisions->size() == 2) {
        CHECK(revisions->front().merkle_root.empty() && revisions->back().merkle_root == root);
    }
    CHECK(repository.query("SELECT COUNT(*) FROM archive_revisions WHERE merkle_root IS NULL") == "1");
}

// rebuild-index empties chunks with the rest of the index, except the rows
// of the revisions it keeps: a registration's saved progress (decision 0026),
// and a revision whose record it keeps, its manifest unread (decision 0028).
void clear_index_empties_chunks()
{
    TestRepository repository;
    auto store = repository.store();
    const std::string kept = "c0ffee00-0000-4000-8000-000000000035";
    const std::string gone = "c0ffee00-0000-4000-8000-000000000036";
    const auto big = chunked_entry("big.bin", 2 * test_chunk_size, 2, "big");
    CHECK(store.insert_file_rows(kept, 2, {big}, test_chunk_size));
    CHECK(store.insert_file_rows(gone, 1, {big}, test_chunk_size));
    // Chunk rows without a file row of their own (sample data).
    repository.execute("INSERT INTO chunks(archive_uuid, revision, path, idx, offset, length, b3) VALUES("
                       "'c0ffee00-0000-4000-8000-000000000037', 1, 'lost.bin', 0, 0, 1, 'x')");
    CHECK(store.clear_index({}, {{kept, 2}}));
    CHECK(chunk_rows(repository, "archive_uuid = '" + kept + "'") == "2");
    CHECK(chunk_rows(repository, "archive_uuid <> '" + kept + "'") == "0");
    CHECK(store.insert_file_rows(gone, 1, {big}, test_chunk_size));
    CHECK(store.clear_index({{gone, 1}}));
    CHECK(chunk_rows(repository, "archive_uuid = '" + gone + "'") == "2");
    CHECK(chunk_rows(repository, "archive_uuid <> '" + gone + "'") == "0");
    CHECK(store.clear_index());
    CHECK(chunk_rows(repository, "1") == "0");
}

} // namespace

void archive_store_tests()
{
    archives_revisions_and_labels();
    automatic_labels_skip_the_labels_in_use();
    history_chain_detects_tampering();
    history_chain_limits_in_v1();
    history_across_archives();
    items_and_progress();
    pending_registrations_with_checkpoints();
    unfinished_verifications_with_checkpoints();
    unfinished_verification_totals_from_its_record();
    superseded_verifications_are_left_out();
    a_retired_archives_verifications_are_kept_and_marked();
    archive_list_mark_follows_the_archives();
    reads_in_one_transaction();
    read_only_open_creates_nothing();
    reads_without_write_ahead_log();
    failed_write_transaction_is_rolled_back();
    update_setting_never_creates();
    later_revision_of_an_archive();
    chunk_rows_go_with_their_files();
    chunk_rows_fit_their_files();
    merkle_roots_are_recorded();
    clear_index_empties_chunks();
}
