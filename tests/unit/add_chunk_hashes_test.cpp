// Adding chunk hashes (decision 0033): a FULL verify of an archive registered
// without chunk lines, as V1 registered it, that hashes the chunks of its
// files in the same read and records the next revision with the same entry
// lines, their chunk lines and Merkle root, when every entry matched. All the
// archives here are sample data, registered with 64 KiB chunks where a real
// archive has 64 MiB.
#include "test_support.h"

#include "engine_fixture.h"
#include "manifest_samples.h"

#include "app/adding_chunk_hashes.h"
#include "app/refusal_in_window.h"
#include "app/unfinished_operations.h"
#include "core/engine/abandon_registration.h"
#include "core/engine/abandon_verification.h"
#include "core/engine/add_chunk_hashes.h"
#include "core/engine/rebuild_index.h"
#include "core/engine/recorded_manifest.h"
#include "core/engine/relocate_engine.h"
#include "core/hash/hashers.h"
#include "core/manifest/manifest_reader.h"
#include "core/manifest/manifest_writer.h"
#include "core/scan/scanner.h"
#include "platform/file_system.h"
#include "platform/file_writer.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <optional>
#include <semaphore>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

constexpr std::uint64_t test_chunk_size = 64 * 1024;

std::string read_file(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

// An archive registered without chunk lines, as V1 registered it: a.bin of
// three chunks at 64 KiB, the last 100 bytes long; b.txt; c.bin of exactly two
// chunks; an empty folder; and the files of `more` (sample data).
struct RegisteredWithout : test::EngineFixture {
    va::Uuid archive_uuid;
    std::string a = test::patterned(2 * test_chunk_size + 100, 1);
    std::string b = "b (sample data)";
    std::string c = test::patterned(2 * test_chunk_size, 2);

    explicit RegisteredWithout(const std::vector<std::pair<std::string, std::string>>& more = {})
    {
        archive.add_file("a.bin", a);
        archive.add_file("b.txt", b);
        archive.add_file("c.bin", c);
        archive.add_directory("empty");
        for (const auto& [p, content] : more) {
            archive.add_file(p, content);
        }
        va::RegisterRequest request;
        request.root = archive.root();
        request.chunk_size = 0;
        const auto registered = register_root(request);
        CHECK(registered.has_value());
        archive_uuid = registered ? registered->archive_uuid : va::Uuid{};
    }

    // A FULL verify of the archive, by its uuid, that adds chunk hashes at
    // 64 KiB, or the resume of `run`.
    std::expected<va::VerifyOutcome, va::VerifyStop> add(std::optional<std::string> run = std::nullopt)
    {
        va::VerifyRequest request;
        request.archive_uuid = archive_uuid;
        request.add_chunk_hashes = !run;
        request.chunk_size = test_chunk_size;
        request.resume_run = std::move(run);
        return verify(request);
    }

    std::filesystem::path manifest(std::uint32_t revision) const
    {
        return va::manifest_path(repository.context.repo.manifests(), archive_uuid, revision);
    }

    std::uint32_t current_revision()
    {
        const auto found = store.find_archive(archive_uuid);
        return found && *found ? (*found)->current_revision : 0;
    }

    // The files in the archive's folder of the repository.
    std::size_t manifest_files() const
    {
        std::size_t count = 0;
        for ([[maybe_unused]] const auto& file :
            std::filesystem::directory_iterator(manifest(1).parent_path())) {
            ++count;
        }
        return count;
    }
};

// The chunk lines of `p`, with `content`, as a manifest with 64 KiB chunks
// holds them.
std::vector<std::string> chunk_lines(const std::string& p, const std::string& content)
{
    std::vector<std::string> lines;
    const auto hashes = test::chunk_hashes(content, test_chunk_size);
    for (std::uint64_t idx = 0; idx < hashes.size(); ++idx) {
        auto chunk = va::chunk_of(p, content.size(), idx, test_chunk_size);
        chunk.b3 = hashes[idx];
        lines.push_back(va::chunk_line_to_json(chunk));
    }
    return lines;
}

// The read that verifies every file gives the chunk hashes of the larger
// ones, each file read once (by bytes); every entry matched, so revision 2 is
// recorded: revision 1's entry lines byte for byte, then the chunk lines, and
// the header's chunk size, number of chunks and Merkle root. The archive is
// at revision 2, whose index holds the chunk rows and the Merkle root; the
// run is the FULL verification of revision 2, from revision 1, and leaves no
// record or checkpoints. A FULL verify of revision 2 compares by chunk, and
// adding chunk hashes again is refused.
void adds_the_chunk_hashes_of_an_archive_registered_without_them()
{
    RegisteredWithout fixture;
    fixture.archive.bytes_read = 0;
    const auto outcome = fixture.add();
    CHECK(outcome && outcome->result == "PASSED" && outcome->revision == 2 && outcome->added_revision == 2 &&
          outcome->adding_chunk_size == test_chunk_size && outcome->added_chunks == 5 && outcome->not_added.empty());
    if (!outcome) {
        std::cerr << "  stopped: " << outcome.error().message << '\n';
        return;
    }
    CHECK(fixture.archive.bytes_read == fixture.a.size() + fixture.b.size() + fixture.c.size());
    CHECK(fixture.current_revision() == 2);

    const auto first = test::split_manifest(read_file(fixture.manifest(1)));
    const auto second = test::split_manifest(read_file(fixture.manifest(2)));
    CHECK(first.header.chunk_size == 0 && first.header.merkle_root.empty());
    CHECK(second.header.revision == 2 && second.header.supersedes == 1u &&
          second.header.chunk_size == test_chunk_size && second.header.counts.chunks == 5 &&
          second.header.merkle_root == outcome->added_merkle_root && outcome->added_merkle_root.size() == 64);
    CHECK(second.header.counts.files == first.header.counts.files &&
          second.header.counts.bytes == first.header.counts.bytes && second.header.label == first.header.label);
    std::vector<std::string> expected = first.lines;
    for (const auto& line : chunk_lines("a.bin", fixture.a)) {
        expected.push_back(line);
    }
    for (const auto& line : chunk_lines("c.bin", fixture.c)) {
        expected.push_back(line);
    }
    CHECK(second.lines == expected);
    CHECK(fixture.manifest_files() == 2); // the side files of the writer are gone

    // The manifest is the one recorded, and loads with its chunk hashes.
    const auto loaded = va::load_recorded_manifest(
        fixture.store, *fixture.repository.files, fixture.repository.context.repo, fixture.archive_uuid, 2);
    CHECK(loaded.has_value());
    const auto revision = fixture.store.revision(fixture.archive_uuid, 2);
    CHECK(revision && *revision && (*revision)->merkle_root == outcome->added_merkle_root);
    const auto rows = fixture.store.file_rows(fixture.archive_uuid, 2);
    CHECK(rows && rows->size() == 4);
    if (rows) {
        for (const auto& row : *rows) {
            const auto* content = row.path.p == "a.bin" ? &fixture.a : row.path.p == "c.bin" ? &fixture.c : nullptr;
            CHECK(row.chunks == (content ? test::chunk_hashes(*content, test_chunk_size) : std::vector<std::string>{}));
        }
    }

    const auto runs = fixture.store.verify_runs(fixture.archive_uuid);
    CHECK(runs && runs->size() == 1);
    if (runs && runs->size() == 1) {
        const auto& run = runs->front();
        const auto counts = nlohmann::json::parse(run.counts_json, nullptr, false);
        CHECK(run.run_id == outcome->run_id && run.revision == 2 && run.mode == "FULL" && run.result == "PASSED");
        CHECK(counts.value("chunk_hashes_added_from_revision", 0) == 1);
    }
    const auto left = fixture.store.settings_with_prefix("run:");
    CHECK(left && left->empty());
    const auto saved = fixture.store.progress(outcome->run_id);
    CHECK(saved && saved->empty());

    // Revision 2 is verified by chunk, as any revision with chunk lines.
    auto changed = fixture.a;
    changed[test_chunk_size + 7] = static_cast<char>(changed[test_chunk_size + 7] ^ 0x01);
    fixture.archive.node("a.bin").content = changed;
    const auto verified = fixture.verify(fixture.archive.root());
    CHECK(verified && verified->revision == 2 && verified->result == "FAILED" && verified->items.size() == 1 &&
          verified->items.front().detail == "content differs: chunk 1 of 3 differs, from byte 65,536");
    fixture.archive.node("a.bin").content = fixture.a;

    // It has chunk hashes now: adding them again is refused.
    const auto again = fixture.add();
    CHECK(!again && again.error().kind == va::VerifyStop::Kind::Refused &&
          again.error().message == "revision 2 of archive " + fixture.archive_uuid +
                                       " has chunk hashes already; a FULL verify compares them, and there are none "
                                       "to add");
}

// Any difference records the verification alone (decision 0033), as a FULL
// verify records it: a changed file fails it, a changed mtime makes it
// PASSED_WITH_WARNINGS, a new file fails it. The archive stays at revision 1,
// no manifest of revision 2 is written or left, and the outcome says why.
void a_difference_records_the_verification_alone()
{
    RegisteredWithout fixture;
    const auto stays = [&](const va::VerifyOutcome& outcome) {
        CHECK(outcome.added_revision == 0 && outcome.revision == 1 && fixture.current_revision() == 1);
        CHECK(!std::filesystem::exists(fixture.manifest(2)) && fixture.manifest_files() == 1);
        const auto runs = fixture.store.verify_runs(fixture.archive_uuid);
        CHECK(runs && !runs->empty() && runs->back().run_id == outcome.run_id && runs->back().revision == 1 &&
              runs->back().result == outcome.result &&
              !nlohmann::json::parse(runs->back().counts_json).contains("chunk_hashes_added_from_revision"));
        const auto left = fixture.store.settings_with_prefix("run:");
        CHECK(left && left->empty());
    };

    fixture.archive.node("b.txt").content = "B (sample data)";
    auto outcome = fixture.add();
    CHECK(outcome && outcome->result == "FAILED" &&
          outcome->not_added == "the verification did not pass; revision 1 stays without chunk hashes");
    if (outcome) {
        stays(*outcome);
    }
    fixture.archive.node("b.txt").content = fixture.b;

    fixture.archive.node("b.txt").mtime_ns += 5'000'000'000;
    outcome = fixture.add();
    CHECK(outcome && outcome->result == "PASSED_WITH_WARNINGS" && outcome->items.size() == 1 &&
          outcome->items.front().status == "MetadataMismatch" &&
          outcome->not_added == "not every entry matches revision 1 as recorded; revision 1 stays without chunk hashes "
                                "(`register --new-revision` records the archive as it is now, with chunk hashes)");
    if (outcome) {
        stays(*outcome);
    }
    fixture.archive.node("b.txt").mtime_ns -= 5'000'000'000;

    fixture.archive.add_file("d.txt", "new (sample data)");
    outcome = fixture.add();
    CHECK(outcome && outcome->result == "FAILED" && outcome->not_added.starts_with("the verification did not pass"));
    if (outcome) {
        stays(*outcome);
    }
}

// Decision 0010: entries recorded unreadable at registration are carried into
// revision 2 as revision 1 recorded them, without hashes, whether they can be
// read now (Unbaselined) or not; they make the run PASSED_WITH_WARNINGS, and
// revision 2 is recorded.
void entries_unreadable_since_registration_are_carried()
{
    for (const bool readable_now : {false, true}) {
        test::EngineFixture fixture;
        const auto big = test::patterned(3 * test_chunk_size, 3);
        fixture.archive.add_file("big.bin", big);
        fixture.archive.add_file("locked.bin", test::patterned(2 * test_chunk_size, 4)).open_error =
            va::IoError{va::IoErrorKind::PermissionDenied, 5, "access denied"};
        va::RegisterRequest request;
        request.root = fixture.archive.root();
        request.chunk_size = 0;
        const auto registered = fixture.register_root(request);
        CHECK(registered && registered->unreadable.size() == 1);
        if (!registered) {
            continue;
        }
        if (readable_now) {
            fixture.archive.node("locked.bin").open_error.reset();
        }
        va::VerifyRequest adding;
        adding.root = fixture.archive.root();
        adding.add_chunk_hashes = true;
        adding.chunk_size = test_chunk_size;
        const auto outcome = fixture.verify(adding);
        CHECK(outcome && outcome->result == "PASSED_WITH_WARNINGS" && outcome->added_revision == 2 &&
              outcome->added_chunks == 3 && outcome->items.size() == 1);
        if (!outcome || outcome->items.size() != 1) {
            continue;
        }
        CHECK(outcome->items.front().status == (readable_now ? "Unbaselined" : "Unreadable(permission)"));
        const auto manifests = fixture.repository.context.repo.manifests();
        const auto first = test::split_manifest(
            read_file(va::manifest_path(manifests, registered->archive_uuid, 1)));
        const auto second = test::split_manifest(
            read_file(va::manifest_path(manifests, registered->archive_uuid, 2)));
        CHECK(second.lines.size() == first.lines.size() + 3 &&
              std::equal(first.lines.begin(), first.lines.end(), second.lines.begin()));
        CHECK(first.lines.size() == 2 && first.lines[1].contains("\"unreadable\":\"permission\""));
    }
}

// Refused before anything is read or written (decision 0033): an archive whose
// revision has chunk hashes, a retired archive, chunk size 0, and adding them
// in the resume of a run that adds none.
void refusals()
{
    test::EngineFixture chunked;
    chunked.archive.add_file("big.bin", test::patterned(2 * test_chunk_size + 1, 5));
    va::RegisterRequest request;
    request.root = chunked.archive.root();
    request.chunk_size = test_chunk_size;
    const auto registered = chunked.register_root(request);
    CHECK(registered.has_value());
    va::VerifyRequest adding;
    adding.root = chunked.archive.root();
    adding.add_chunk_hashes = true;
    adding.chunk_size = test_chunk_size;
    chunked.archive.bytes_read = 0;
    auto refused = chunked.verify(adding);
    CHECK(!refused && refused.error().kind == va::VerifyStop::Kind::Refused && registered &&
          refused.error().message == "revision 1 of archive " + registered->archive_uuid +
                                         " has chunk hashes already; a FULL verify compares them, and there are none "
                                         "to add");
    CHECK(chunked.archive.bytes_read == 0);
    if (registered) {
        const auto runs = chunked.store.verify_runs(registered->archive_uuid);
        CHECK(runs && runs->empty());
    }
    const auto records = chunked.store.settings_with_prefix("run:");
    CHECK(records && records->empty());

    RegisteredWithout retired;
    if (auto record = retired.store.find_archive(retired.archive_uuid); record && *record) {
        auto changed = **record;
        changed.status = "RETIRED";
        CHECK(retired.store.update_archive(changed).has_value());
    }
    refused = retired.add();
    CHECK(!refused && refused.error().kind == va::VerifyStop::Kind::Refused &&
          refused.error().message == "a retired archive takes no new revisions");

    RegisteredWithout fixture;
    va::VerifyRequest zero;
    zero.archive_uuid = fixture.archive_uuid;
    zero.add_chunk_hashes = true;
    zero.chunk_size = 0;
    refused = fixture.verify(zero);
    CHECK(!refused && refused.error().kind == va::VerifyStop::Kind::Refused &&
          refused.error().message == "chunk hashes cannot be added at chunk size 0");

    // A run that adds none is not resumed adding them.
    fixture.archive.node("c.bin").open_error = va::IoError{va::IoErrorKind::DeviceRemoved, 0, "unplugged"};
    const auto interrupted = fixture.verify(fixture.archive.root());
    CHECK(interrupted && interrupted->result == "INCOMPLETE" && interrupted->adding_chunk_size == 0);
    fixture.archive.node("c.bin").open_error.reset();
    if (interrupted) {
        va::VerifyRequest resume;
        resume.root = fixture.archive.root();
        resume.resume_run = interrupted->run_id;
        resume.add_chunk_hashes = true;
        refused = fixture.verify(resume);
        CHECK(!refused && refused.error().kind == va::VerifyStop::Kind::Refused &&
              refused.error().message == "run " + interrupted->run_id +
                                             " adds no chunk hashes; resume it without --add-chunk-hashes, or start a "
                                             "new verification with it");
    }
}

// Writes the repository through `inner`, and runs `hook` when the manifest
// of a revision has been written and flushed, before the transaction that
// records it.
struct HookedVerify {
    test::StagingHookWriter writer;
    va::VerifyServices services;

    HookedVerify(RegisteredWithout& fixture, std::function<void()> hook)
        : writer(*fixture.repository.writer, std::move(hook)),
          services{fixture.archive, *fixture.repository.files, fixture.media, fixture.store,
              fixture.repository.context.repo, fixture.repository.context.repository_uuid, fixture.host, writer,
              fixture.processes}
    {
    }
};

// The record checks again, in its transaction, what the read could not
// (decision 0033): the archive still at revision 1, as another operation may
// have recorded a revision meanwhile (an import, decision 0030), and no
// registration with saved files of revision 2, which keeps that revision
// (decision 0023). Otherwise the run records the verification alone, says
// why, and leaves no manifest of revision 2.
void the_record_leaves_the_revision_to_another_operation()
{
    {
        RegisteredWithout fixture;
        const auto sql = "UPDATE archives SET current_revision = 2 WHERE archive_uuid = '" + fixture.archive_uuid + "'";
        HookedVerify hooked(fixture, [&] { fixture.repository.execute(sql.c_str()); });
        va::VerifyRequest request;
        request.archive_uuid = fixture.archive_uuid;
        request.add_chunk_hashes = true;
        request.chunk_size = test_chunk_size;
        const auto outcome = va::verify_full(hooked.services, request, fixture.cancel, fixture.progress);
        CHECK(outcome && outcome->result == "PASSED" && outcome->added_revision == 0 && outcome->revision == 1 &&
              outcome->not_added == "archive " + fixture.archive_uuid +
                                        " is at revision 2 now, recorded by another operation while this verification "
                                        "ran");
        CHECK(!std::filesystem::exists(fixture.manifest(2)) && fixture.manifest_files() == 1);
        const auto revisions = fixture.store.revisions(fixture.archive_uuid);
        CHECK(revisions && revisions->size() == 1);
        const auto runs = fixture.store.verify_runs(fixture.archive_uuid);
        CHECK(runs && runs->size() == 1 && runs->front().revision == 1 && runs->front().result == "PASSED");
    }
    {
        RegisteredWithout fixture;
        // A row of a registration's saved progress of revision 2, as an
        // attempt that is not this version's would have left it.
        const auto sql = "INSERT INTO files(archive_uuid, revision, path, type, size) VALUES('" +
                         fixture.archive_uuid + "', 2, 'a.bin', 'f', 1)";
        HookedVerify hooked(fixture, [&] { fixture.repository.execute(sql.c_str()); });
        va::VerifyRequest request;
        request.archive_uuid = fixture.archive_uuid;
        request.add_chunk_hashes = true;
        request.chunk_size = test_chunk_size;
        const auto outcome = va::verify_full(hooked.services, request, fixture.cancel, fixture.progress);
        CHECK(outcome && outcome->result == "PASSED" && outcome->added_revision == 0 &&
              outcome->not_added == "an unfinished registration of archive " + fixture.archive_uuid +
                                        " (root \"\") is creating revision 2; finish it with `register --resume`, or "
                                        "abandon it with `archive abandon-registration " +
                                        fixture.archive_uuid + "`");
        CHECK(fixture.current_revision() == 1 && !std::filesystem::exists(fixture.manifest(2)));
    }
    {
        RegisteredWithout fixture;
        const auto sql = "UPDATE archives SET status = 'RETIRED' WHERE archive_uuid = '" + fixture.archive_uuid + "'";
        HookedVerify hooked(fixture, [&] { fixture.repository.execute(sql.c_str()); });
        va::VerifyRequest request;
        request.archive_uuid = fixture.archive_uuid;
        request.add_chunk_hashes = true;
        request.chunk_size = test_chunk_size;
        const auto outcome = va::verify_full(hooked.services, request, fixture.cancel, fixture.progress);
        CHECK(outcome && outcome->result == "PASSED" && outcome->added_revision == 0 &&
              outcome->not_added == "archive " + fixture.archive_uuid +
                                        " was retired while this verification ran; a retired archive takes no new "
                                        "revisions");
        CHECK(fixture.current_revision() == 1 && !std::filesystem::exists(fixture.manifest(2)));
    }
}

// The record of revision 2 waits for the write lock as the record of a
// registration or a relocation does (decision 0023): another process holds it
// for longer than a statement waits (scaled down: the run's statements wait
// 100 ms, the lock is held for 1 s from when the manifest is written), and the
// record goes ahead once it is free.
void the_record_waits_for_the_write_lock()
{
    RegisteredWithout fixture;
    std::thread holder;
    std::binary_semaphore held{0};
    test::StagingHookWriter writer(*fixture.repository.writer, [&] {
        holder = std::thread([&] {
            auto other = fixture.repository.store();
            CHECK(other.begin());
            held.release();
            std::this_thread::sleep_for(std::chrono::seconds(1));
            CHECK(other.commit());
        });
        CHECK(held.try_acquire_for(std::chrono::seconds(60)));
    });
    auto store = fixture.repository.store(std::chrono::milliseconds(100));
    va::VerifyServices services{fixture.archive, *fixture.repository.files, fixture.media, store,
        fixture.repository.context.repo, fixture.repository.context.repository_uuid, fixture.host, writer,
        fixture.processes};
    va::VerifyRequest request;
    request.archive_uuid = fixture.archive_uuid;
    request.add_chunk_hashes = true;
    request.chunk_size = test_chunk_size;
    const auto outcome = va::verify_full(services, request, fixture.cancel, fixture.progress);
    if (holder.joinable()) {
        holder.join();
    }
    CHECK(outcome && outcome->added_revision == 2);
    if (!outcome) {
        std::cerr << "  stopped: " << outcome.error().message << '\n';
    }
    CHECK(fixture.current_revision() == 2);
}

// Once revision 2 is recorded, a registration and a relocation that create
// it can never finish (decisions 0023, 0026): the record removes the plan of
// one that has saved nothing for it, and the record and saved progress of the
// other, as their own records would, here written while the run read.
void the_record_removes_what_can_no_longer_finish()
{
    RegisteredWithout fixture;
    const auto& uuid = fixture.archive_uuid;
    const std::string relocation_run = "c0ffee00-0000-4000-8000-0000000000aa";
    const auto plan = "INSERT INTO settings(key, value) VALUES('registration:" + uuid + "', '{\"archive_uuid\":\"" +
                      uuid + "\",\"revision\":2,\"label\":\"ARCH-0001\",\"created\":\"2026-10-16T00:00:00Z\"," +
                      "\"new_archive\":false,\"root\":\"/vol\"}')";
    const auto relocation = "INSERT INTO settings(key, value) VALUES('relocation:" + uuid + "', '{\"archive_uuid\":\"" +
                            uuid + "\",\"revision\":1,\"run_id\":\"" + relocation_run + "\",\"root\":\"/sample\"}')";
    const auto matched = "INSERT INTO verify_progress(run_id, path) VALUES('" + relocation_run + "', 'b.txt')";
    HookedVerify hooked(fixture, [&] {
        for (const auto* sql : {plan.c_str(), relocation.c_str(), matched.c_str()}) {
            fixture.repository.execute(sql);
        }
    });
    va::VerifyRequest request;
    request.archive_uuid = uuid;
    request.add_chunk_hashes = true;
    request.chunk_size = test_chunk_size;
    const auto outcome = va::verify_full(hooked.services, request, fixture.cancel, fixture.progress);
    CHECK(outcome && outcome->added_revision == 2);
    CHECK(fixture.store.setting("registration:" + uuid) == std::string{});
    CHECK(fixture.store.setting(va::relocation_key(uuid)) == std::string{});
    const auto left = fixture.store.progress(relocation_run);
    CHECK(left && left->empty());
}

// The entry lines of revision 2 are those of revision 1 byte for byte, or it
// is not recorded: a manifest whose entry lines are valid but not in the
// canonical form (R-64), which this program would write otherwise (sample
// data, written by hand and indexed by rebuild-index), keeps its revision
// without chunk hashes, and the run records the verification alone.
void entry_lines_that_cannot_be_written_again_add_nothing()
{
    RegisteredWithout fixture;
    auto parts = test::split_manifest(read_file(fixture.manifest(1)));
    auto& line = parts.lines.at(1); // b.txt
    CHECK(line.starts_with("{\"t\":\"f\",\"p\":\"b.txt\","));
    line = "{\"p\":\"b.txt\",\"t\":\"f\"," + line.substr(std::string("{\"t\":\"f\",\"p\":\"b.txt\",").size());
    {
        std::ofstream output(fixture.manifest(1), std::ios::binary | std::ios::trunc);
        output << test::assemble_manifest(parts);
    }
    const auto rebuilt = va::rebuild_index(fixture.store, *fixture.repository.files, fixture.repository.context.repo,
        test::not_cancelled(), test::no_progress());
    CHECK(rebuilt && rebuilt->revisions == 1 && rebuilt->skipped.empty());
    const auto outcome = fixture.add();
    CHECK(outcome && outcome->result == "PASSED" && outcome->added_revision == 0 &&
          outcome->not_added == "the entry lines of revision 1 cannot be written again as they are; revision 1 stays "
                                "without chunk hashes (`register --new-revision` records them)");
    CHECK(fixture.current_revision() == 1 && !std::filesystem::exists(fixture.manifest(2)) &&
          fixture.manifest_files() == 1);
}

// The archive of the interruption tests: a.bin, b.txt, c.bin, then e.txt,
// f.bin and g.bin (sample data), the bytes before e.txt about half of them.
struct Halves : RegisteredWithout {
    std::string e = "e (sample data)";
    std::string f = test::patterned(2 * test_chunk_size + 100, 6);
    std::string g = test::patterned(2 * test_chunk_size, 7);

    Halves() : RegisteredWithout({{"e.txt", "e (sample data)"}, {"f.bin", f_content()}, {"g.bin", g_content()}}) {}

    static std::string f_content() { return test::patterned(2 * test_chunk_size + 100, 6); }
    static std::string g_content() { return test::patterned(2 * test_chunk_size, 7); }

    // Interrupted where it reaches e.txt, as when its device goes away: a.bin,
    // b.txt and c.bin are checkpointed.
    std::optional<va::VerifyOutcome> interrupted()
    {
        archive.node("e.txt").open_error = va::IoError{va::IoErrorKind::DeviceRemoved, 0, "unplugged"};
        auto outcome = add();
        archive.node("e.txt").open_error.reset();
        CHECK(outcome && outcome->result == "INCOMPLETE" && outcome->added_revision == 0 &&
              outcome->not_added.empty() && outcome->adding_chunk_size == test_chunk_size);
        return outcome ? std::optional<va::VerifyOutcome>(std::move(*outcome)) : std::nullopt;
    }

    // Revision 2: revision 1's entry lines, then the chunk lines of a.bin,
    // c.bin, f.bin and g.bin, from their content.
    void check_revision_2()
    {
        const auto first = test::split_manifest(read_file(manifest(1)));
        auto expected = first.lines;
        for (const auto& [p, content] :
            std::vector<std::pair<std::string, const std::string*>>{{"a.bin", &a}, {"c.bin", &c}, {"f.bin", &f},
                {"g.bin", &g}}) {
            for (const auto& line : chunk_lines(p, *content)) {
                expected.push_back(line);
            }
        }
        CHECK(test::split_manifest(read_file(manifest(2))).lines == expected);
    }
};

// R-104: a run that adds chunk hashes and is interrupted at about half, here
// as its device goes away, is recorded INCOMPLETE with its record saying that
// it adds them, at which chunk size, and its checkpoints keep the chunk hashes
// of the files they hold (decision 0033). Its resume, without
// --add-chunk-hashes, goes on adding them and reads only the files not
// checkpointed (by bytes): nothing is read twice. It records revision 2 with
// every chunk hash, and leaves no checkpoints or kept chunk hashes.
void an_interrupted_run_resumes_adding()
{
    Halves fixture;
    const auto interrupted = fixture.interrupted();
    if (!interrupted) {
        return;
    }
    // Its row in the history says that it stopped; its record, as the run
    // left it, carries no stop mark.
    const auto record = nlohmann::json::parse(fixture.store.setting("run:" + interrupted->run_id).value_or(""),
        nullptr, false);
    CHECK(va::adding_chunk_size(record) == test_chunk_size && !record.contains("stopped"));
    const auto saved = fixture.store.progress(interrupted->run_id);
    CHECK(saved && saved->size() == 3);
    va::Entry a;
    a.path.p = "a.bin";
    va::Entry c;
    c.path.p = "c.bin";
    const auto kept = fixture.store.progress_chunks(interrupted->run_id);
    CHECK(kept && *kept == (std::map<std::string, std::vector<std::string>>{
                               {va::entry_key(a), test::chunk_hashes(fixture.a, test_chunk_size)},
                               {va::entry_key(c), test::chunk_hashes(fixture.c, test_chunk_size)}}));
    CHECK(fixture.current_revision() == 1);
    // Half of the bytes were matched before the interruption.
    CHECK(interrupted->coverage_bytes == fixture.a.size() + fixture.b.size() + fixture.c.size());

    fixture.archive.bytes_read = 0;
    const auto resumed = fixture.add(interrupted->run_id);
    CHECK(resumed && resumed->result == "PASSED" && resumed->resumed_from == interrupted->run_id &&
          resumed->added_revision == 2 && resumed->added_chunks == 10);
    CHECK(fixture.archive.bytes_read == fixture.e.size() + fixture.f.size() + fixture.g.size());
    fixture.check_revision_2();
    const auto left = fixture.store.settings_with_prefix("run:");
    CHECK(left && left->empty());
    if (resumed) {
        const auto progress = fixture.store.progress(resumed->run_id);
        const auto chunks = fixture.store.progress_chunks(resumed->run_id);
        CHECK(progress && progress->empty() && chunks && chunks->empty());
    }
    const auto old = fixture.store.progress_chunks(interrupted->run_id);
    CHECK(old && old->empty());
}

// Decision 0034: a FULL verify that adds chunk hashes whose device goes away
// halfway through a large file, its reads failing with I/O errors alone and
// its volume no longer found, is interrupted as any FULL verify is: it ends
// INCOMPLETE, lists no entry unreadable and records no revision. Its resume,
// once the device is attached again, goes on adding chunk hashes and records
// revision 2 with every one of them (sample data).
void a_device_that_goes_away_interrupts_the_run_that_adds_them()
{
    Halves fixture;
    test::RemovableDevice device(fixture.archive);
    device.file = "f.bin";
    device.at_offset = test_chunk_size + 10;
    device.on_removed = [&fixture] {
        fixture.media.error = va::IoError{va::IoErrorKind::NotFound, 2, "no volume at the root (sample)"};
    };
    fixture.media_files = &device;
    const auto interrupted = fixture.add();
    CHECK(interrupted && interrupted->result == "INCOMPLETE" && interrupted->items.empty() &&
          interrupted->failure_classes.empty() && interrupted->added_revision == 0 &&
          interrupted->not_added.empty() && interrupted->adding_chunk_size == test_chunk_size &&
          interrupted->warnings ==
              std::vector<std::string>{"the device that holds the archive went away while it was read (unplugged, or "
                                       "no longer answering); attach it again, then resume this run"});
    CHECK(fixture.current_revision() == 1 && !std::filesystem::exists(fixture.manifest(2)));
    if (!interrupted) {
        return;
    }

    device.attach_again();
    fixture.media.error.reset();
    const auto resumed = fixture.add(interrupted->run_id);
    CHECK(resumed && resumed->result == "PASSED" && resumed->resumed_from == interrupted->run_id &&
          resumed->added_revision == 2 && resumed->added_chunks == 10);
    fixture.check_revision_2();
}

// Interrupted twice, the run's resume takes over the checkpoints and the kept
// chunk hashes of the run it resumes, and the next resume those of both:
// nothing is read twice, though a.bin and c.bin were read by the first run.
void a_run_interrupted_twice_reads_nothing_twice()
{
    Halves fixture;
    const auto first = fixture.interrupted();
    if (!first) {
        return;
    }
    fixture.archive.node("f.bin").open_error = va::IoError{va::IoErrorKind::DeviceRemoved, 0, "unplugged"};
    const auto second = fixture.add(first->run_id);
    fixture.archive.node("f.bin").open_error.reset();
    CHECK(second && second->result == "INCOMPLETE" && second->resumed_from == first->run_id);
    if (!second) {
        return;
    }
    const auto moved = fixture.store.progress_chunks(second->run_id);
    CHECK(moved && moved->size() == 2);
    fixture.archive.bytes_read = 0;
    const auto third = fixture.add(second->run_id);
    CHECK(third && third->result == "PASSED" && third->added_revision == 2 && third->added_chunks == 10);
    CHECK(fixture.archive.bytes_read == fixture.f.size() + fixture.g.size());
    fixture.check_revision_2();
}

// The chunk hashes kept with a run's checkpoints, in the repository: each
// file's in place of those kept before; a file whose rows have a gap gets
// none; clearing the run's checkpoints removes them, and no other run's.
void kept_chunk_hashes_in_the_repository()
{
    test::TestRepository repository;
    auto store = repository.store();
    const std::string run = "c0ffee00-0000-4000-8000-0000000000b1";
    const std::string other = "c0ffee00-0000-4000-8000-0000000000b2";
    va::Entry one;
    one.path.p = "one.bin";
    va::Entry two;
    two.path.p = "x\xEF\xBF\xBD";
    two.path.raw = "78ff";
    const std::map<std::string, std::vector<std::string>> first{
        {va::entry_key(one), {va::blake3_hex("1/0"), va::blake3_hex("1/1"), va::blake3_hex("1/2")}},
        {va::entry_key(two), {va::blake3_hex("2/0"), va::blake3_hex("2/1")}}};
    CHECK(store.add_progress_chunks(run, first).has_value());
    CHECK(store.add_progress_chunks(other, first).has_value());
    CHECK(store.progress_chunks(run) == first);

    const std::map<std::string, std::vector<std::string>> again{
        {va::entry_key(one), {va::blake3_hex("1/0'"), va::blake3_hex("1/1'"), va::blake3_hex("1/2'")}}};
    CHECK(store.add_progress_chunks(run, again).has_value());
    auto expected = first;
    expected[va::entry_key(one)] = again.at(va::entry_key(one));
    CHECK(store.progress_chunks(run) == expected);

    repository.execute(("UPDATE verify_progress_chunks SET idx = 3 WHERE run_id = '" + run +
                        "' AND path = 'one.bin' AND idx = 2")
                           .c_str());
    expected.erase(va::entry_key(one));
    CHECK(store.progress_chunks(run) == expected);

    CHECK(store.clear_progress(run).has_value());
    const auto cleared = store.progress_chunks(run);
    CHECK(cleared && cleared->empty());
    CHECK(store.progress_chunks(other) == first);
}

// A checkpointed file whose kept chunk hashes are not all there is read again
// by the resume, which records revision 2 with its chunk hashes read again
// (sample data): a.bin, whose rows have a gap (the middle one of its three
// removed), and c.bin, whose rows are fewer than its size gives (the last of
// its two removed).
void a_file_without_all_its_kept_chunk_hashes_is_read_again()
{
    Halves fixture;
    const auto interrupted = fixture.interrupted();
    if (!interrupted) {
        return;
    }
    for (const auto* row : {"path = 'a.bin' AND idx = 1", "path = 'c.bin' AND idx = 1"}) {
        fixture.repository.execute(
            ("DELETE FROM verify_progress_chunks WHERE run_id = '" + interrupted->run_id + "' AND " + row).c_str());
    }
    fixture.archive.bytes_read = 0;
    const auto resumed = fixture.add(interrupted->run_id);
    CHECK(resumed && resumed->added_revision == 2);
    CHECK(fixture.archive.bytes_read ==
          fixture.a.size() + fixture.c.size() + fixture.e.size() + fixture.f.size() + fixture.g.size());
    fixture.check_revision_2();
}

// The rule by which a verification refuses to add chunk hashes, and by which
// the GUI offers to add them (decision 0033): an archive registered without
// them takes them; one whose revision has them, or that is retired, does not,
// saying why; one whose manifest cannot be read fails.
void what_takes_chunk_hashes()
{
    const auto why = [](test::EngineFixture& fixture, const va::Uuid& archive_uuid) {
        const auto archive = fixture.store.find_archive(archive_uuid);
        CHECK(archive && *archive);
        return va::why_chunk_hashes_cannot_be_added(
            fixture.store, *fixture.repository.files, fixture.repository.context.repo, **archive);
    };
    RegisteredWithout without;
    const auto takes = why(without, without.archive_uuid);
    CHECK(takes && !*takes);

    test::EngineFixture with;
    with.archive.add_file("big.bin", test::patterned(2 * test_chunk_size + 1, 9));
    va::RegisterRequest request;
    request.root = with.archive.root();
    request.chunk_size = test_chunk_size;
    const auto registered = with.register_root(request);
    CHECK(registered.has_value());
    if (registered) {
        const auto has_them = why(with, registered->archive_uuid);
        CHECK(has_them && *has_them == "revision 1 of archive " + registered->archive_uuid +
                                           " has chunk hashes already; a FULL verify compares them, and there are "
                                           "none to add");
    }

    without.repository.execute(
        ("UPDATE archives SET status = 'RETIRED' WHERE archive_uuid = '" + without.archive_uuid + "'").c_str());
    const auto retired = why(without, without.archive_uuid);
    CHECK(retired && *retired == "a retired archive takes no new revisions");
    without.repository.execute(
        ("UPDATE archives SET status = 'REGISTERED' WHERE archive_uuid = '" + without.archive_uuid + "'").c_str());

    auto away = without.manifest(1);
    away += ".away";
    std::filesystem::rename(without.manifest(1), away);
    const auto gone = why(without, without.archive_uuid);
    CHECK(!gone && gone.error().message.contains("the manifest of revision 1 (manifests/" + without.archive_uuid +
                                                   "/r0001.vam) is missing"));
    std::filesystem::rename(away, without.manifest(1));
}

// The header of revision 1 of the sample archive of the golden case below:
// written without a chunk size, as V1 wrote every revision.
va::ManifestHeader golden_first_header()
{
    va::ManifestHeader header;
    header.archive_uuid = "c0ffee00-0000-4000-8000-000000000033";
    header.label = "ARCH-0033";
    header.revision = 1;
    header.created = "2026-10-16T05:00:00Z";
    header.app_version = "0.1.0";
    header.os = "windows";
    header.host = "SAMPLE-PC";
    header.identity.filesystem_type = "ntfs";
    header.identity.filesystem_uuid = "c0ffee00c0ffee33";
    header.identity.filesystem_uuid_short = "c0ffee33";
    header.identity.volume_label = "SAMPLE-VOLUME";
    header.identity.capacity_bytes = 1099511627776;
    va::PhysicalDeviceInfo device;
    device.model = "SAMPLE-DEVICE";
    device.serial = "SAMPLE0001";
    device.capacity_bytes = 1099511627776;
    device.bus = "usb";
    header.identity.devices.push_back(device);
    header.root_hint = "X:\\";
    header.exclusions = va::default_exclusions();
    return header;
}

// Writes `entries` with `chunk_size` to `destination` and gives the manifest
// its name; its bytes.
std::string write_golden(const std::filesystem::path& destination, std::uint64_t chunk_size,
    const std::vector<va::Entry>& entries, const va::ManifestHeader& header, va::IFileWriter& writer,
    va::IFileSystemAccess& files)
{
    std::filesystem::create_directories(destination.parent_path());
    va::ManifestWriter manifest(writer, files, destination, chunk_size);
    for (const auto& entry : entries) {
        CHECK(manifest.add(entry).has_value());
    }
    auto written = manifest.finish(header);
    CHECK(written && written->file->commit());
    return read_file(destination);
}

// CLAUDE.md rule 10, decision 0033: the golden manifest of a revision with
// chunk hashes added, byte for byte on every OS (sample data). Revision 1 of
// a sample archive holds the entries of the golden manifest with chunk lines
// (golden_chunks_r0003.vam: files smaller than a chunk, of exactly one and of
// several, two that share their p, a large stream and a large unreadable
// file, a directory and a link) without a chunk size, as V1 wrote it.
// Revision 2 adds their chunk hashes at 64 KiB: its entry lines are revision
// 1's byte for byte, and its body is that of the golden manifest with chunk
// lines, which a registration at that chunk size wrote; so are its body hash,
// counts and Merkle root.
void golden_revision_with_chunk_hashes_added()
{
    const auto golden = std::filesystem::path(VERIARCHIVE_TEST_DATA_DIR);
    const auto files = va::platform::make_file_system_access();
    const auto writer = va::platform::make_file_writer();
    // The entries of the golden manifest with chunk lines, each file with its
    // chunk hashes.
    std::vector<va::Entry> chunked;
    auto reader = va::ManifestReader::open(*files, golden / "golden_chunks_r0003.vam");
    CHECK(reader.has_value());
    while (reader) {
        auto next = reader->next();
        CHECK(next.has_value());
        if (!next || !*next) {
            break;
        }
        chunked.push_back(std::move(**next));
    }
    CHECK(chunked.size() == 9);
    std::vector<va::Entry> recorded;
    std::map<std::string, std::vector<std::string>> read;
    for (auto entry : chunked) {
        if (!entry.chunks.empty()) {
            read[va::entry_key(entry)] = entry.chunks;
        }
        entry.chunks.clear();
        recorded.push_back(std::move(entry));
    }

    const auto root = std::filesystem::temp_directory_path() /
                      ("va-golden-added-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const auto first_header = golden_first_header();
    const auto first = write_golden(
        va::manifest_path(root, first_header.archive_uuid, 1), 0, recorded, first_header, *writer, *files);
    const auto entries = va::entries_with_chunk_hashes(recorded, read, test_chunk_size);
    CHECK(entries.has_value());
    const va::HostInfo host{"windows", "SAMPLE-PC", "1.1.0"};
    const auto second_header = va::header_with_chunk_hashes(
        first_header, "2026-10-16T06:00:00Z", host, first_header.identity, first_header.root_subpath, "X:\\");
    const auto second = entries ? write_golden(va::manifest_path(root, first_header.archive_uuid, 2), test_chunk_size,
                                      *entries, second_header, *writer, *files)
                                : std::string{};

    const auto expected = read_file(golden / "golden_chunks_added_r0002.vam");
    CHECK(second == expected);
    if (second != expected) {
        std::ofstream(root.parent_path() / "va-golden-chunks-added-actual.vam", std::ios::binary) << second;
        std::cerr << "golden mismatch; actual bytes written to "
                  << va::path_to_utf8(root.parent_path() / "va-golden-chunks-added-actual.vam") << '\n';
    }
    const auto before = test::split_manifest(first);
    const auto after = test::split_manifest(second);
    const auto registered = test::split_manifest(read_file(golden / "golden_chunks_r0003.vam"));
    CHECK(before.header.chunk_size == 0 && before.lines.size() == 9);
    CHECK(after.lines.size() == 17 && std::equal(before.lines.begin(), before.lines.end(), after.lines.begin()));
    CHECK(after.lines == registered.lines);
    CHECK(after.header.revision == 2 && after.header.supersedes == 1u && after.header.chunk_size == test_chunk_size &&
          after.header.counts.chunks == 8 && after.header.merkle_root == registered.header.merkle_root &&
          after.header.body_blake3 == registered.header.body_blake3 &&
          after.header.body_length == registered.header.body_length && after.header.counts.files == 6 &&
          after.header.counts.bytes == registered.header.counts.bytes);
    std::error_code error;
    std::filesystem::remove_all(root, error);
}

// The entries and header of the revision that adds chunk hashes.
void the_revision_that_adds_them()
{
    const auto big = test::patterned(2 * test_chunk_size + 1, 8);
    auto file = test::sample_file("big.bin", big, 0);
    va::Entry folder;
    folder.type = 'd';
    folder.path.p = "empty";
    const std::vector<va::Entry> recorded{file, folder};
    std::map<std::string, std::vector<std::string>> chunks{{va::entry_key(file), test::chunk_hashes(big, test_chunk_size)}};
    const auto entries = va::entries_with_chunk_hashes(recorded, chunks, test_chunk_size);
    CHECK(entries && entries->size() == 2 && (*entries)[0].chunks == test::chunk_hashes(big, test_chunk_size) &&
          (*entries)[1].chunks.empty());
    chunks.begin()->second.pop_back();
    const auto short_of_one = va::entries_with_chunk_hashes(recorded, chunks, test_chunk_size);
    CHECK(!short_of_one && short_of_one.error().message == "chunk hashes of big.bin: 2 read, 3 expected");
    const auto none = va::entries_with_chunk_hashes(recorded, {}, test_chunk_size);
    CHECK(!none && none.error().message == "chunk hashes of big.bin: 0 read, 3 expected");

    va::ManifestHeader previous;
    previous.archive_uuid = "c0ffee00-0000-4000-8000-000000000001";
    previous.label = "ARCH-0001";
    previous.revision = 3;
    previous.supersedes = 2;
    previous.copy_of = "c0ffee00-0000-4000-8000-000000000002";
    previous.exclusions = {"SAMPLE/"};
    va::VolumeInfo volume;
    volume.filesystem_uuid = "c0ffee0000000003";
    const va::HostInfo host{"linux", "sample-host", "1.1.0"};
    const auto header = va::header_with_chunk_hashes(previous, "2026-10-16T00:00:00Z", host, volume, "SAMPLE", "/mnt/x");
    CHECK(header.revision == 4 && header.supersedes == 3u && header.created == "2026-10-16T00:00:00Z" &&
          header.os == "linux" && header.host == "sample-host" && header.app_version == "1.1.0" &&
          header.identity.filesystem_uuid == "c0ffee0000000003" && header.root_subpath == "SAMPLE" &&
          header.root_hint == "/mnt/x" && header.label == "ARCH-0001" && header.copy_of == previous.copy_of &&
          header.exclusions == previous.exclusions && header.archive_uuid == previous.archive_uuid);

    // The record's member says at which chunk size a run adds them.
    CHECK(va::adding_chunk_size(nlohmann::json{{"add_chunk_hashes", test_chunk_size}}) == test_chunk_size);
    CHECK(va::adding_chunk_size(nlohmann::json{{"revision", 1}}) == 0);
    CHECK(va::adding_chunk_size(nlohmann::json{{"add_chunk_hashes", std::uint64_t{1000}}}) == 0);
    CHECK(va::adding_chunk_size(nlohmann::json{{"add_chunk_hashes", std::uint64_t{0}}}) == 0);
    CHECK(va::adding_chunk_size(nlohmann::json{{"add_chunk_hashes", "65536"}}) == 0);
}

// ---- One operation creating an archive's next revision at a time ----------

// The refusal of an operation that would create the next revision while the
// verification that adds chunk hashes `run` is unfinished.
std::string waits_for(const va::Uuid& uuid, const std::string& run, std::uint32_t creates = 2)
{
    return "an unfinished verification of archive " + uuid + " that adds chunk hashes (run " + run +
           ") is creating revision " + std::to_string(creates) +
           "; nothing was changed (finish it with `verify --full " + uuid + " --resume " + run +
           "` first, or abandon it with `archive abandon-verification " + uuid + "`)";
}

// The ways on that the row of the operation that refuses offers in the
// Archive List once it has stopped, which the windows name first (decision
// 0033): Resume (decision 0022, point 7) and Abandon....
const std::string on_its_row = "with Resume, or abandon it with Abandon..., on its row in the Archive List";

// The same refusal of a verification in Verify Archive, which names the
// window's own ways on rather than command lines (decision 0033), once the
// verification that refuses has stopped: the list's, then the view's Resume
// interrupted run.
std::string waits_for_in_window(const va::Uuid& uuid, const std::string& run)
{
    return "an unfinished verification of archive " + uuid + " that adds chunk hashes (run " + run +
           ") is creating revision 2; nothing was changed (finish it first " + on_its_row +
           "; or finish it with Resume interrupted run once the archive is chosen above)";
}

// The same while the verification that refuses still runs, or is held up,
// when the window offers neither Resume nor Abandon... for it.
std::string runs_in_window(const va::Uuid& uuid, const std::string& run)
{
    return "an unfinished verification of archive " + uuid + " that adds chunk hashes (run " + run +
           ") is creating revision 2 and is still running; nothing was changed (wait for it to finish; if it stops, "
           "finish it " +
           on_its_row + "; or finish it with Resume interrupted run once the archive is chosen above)";
}

// The same refusal of a registration in Register Archive (decision 0033),
// while the verification that refuses runs (`runs`), and otherwise.
std::string waits_for_in_register_archive(const va::Uuid& uuid, const std::string& run, bool runs)
{
    const auto refusal = "an unfinished verification of archive " + uuid + " that adds chunk hashes (run " + run +
                         ") is creating revision 2";
    const auto ways = on_its_row + "; or finish it with Resume interrupted run in Verify Archive once the archive is "
                                   "chosen there)";
    return runs ? refusal + " and is still running; nothing was changed (wait for it to finish; if it stops, finish "
                            "it " +
                      ways
                : refusal + "; nothing was changed (finish it first " + ways;
}

// A verification refused while the verification that adds chunk hashes `run`
// is unfinished: the command line's words, that verification, and the
// window's words while it runs and otherwise.
bool refused_by(const va::VerifyStop& stop, const va::Uuid& uuid, const std::string& run)
{
    const bool said = stop.kind == va::VerifyStop::Kind::Refused && stop.message == waits_for(uuid, run) &&
                      stop.refusing && stop.refusing->kind == va::RefusingOperation::Kind::AddingVerification &&
                      stop.refusing->run_id == run && stop.refusing->archive_uuid == uuid &&
                      stop.refusing->revision == 1 &&
                      va::app::stop_message_in_window(stop, false) == waits_for_in_window(uuid, run) &&
                      va::app::stop_message_in_window(stop, true) == runs_in_window(uuid, run);
    if (!said) {
        std::cerr << "  refused with: " << stop.message
                  << "\n  in the window: " << va::app::stop_message_in_window(stop, false)
                  << "\n  while it runs: " << va::app::stop_message_in_window(stop, true) << '\n';
    }
    return said;
}

// Another process: its own connection to the repository, its own view of the
// media (through which a test may step in while it reads), and the process
// table it sees, in which the engines' own process (EngineFixture::processes)
// runs too.
struct OtherProcess {
    va::ArchiveStore store;
    test::FakeProcessTable processes;
    va::CancelToken cancel;
    test::NullProgress progress;

    explicit OtherProcess(test::EngineFixture& fixture) : store(fixture.repository.store())
    {
        processes.self = {5151, "boot-1:2000", "veriarchive-gui.exe"};
        processes.others = {fixture.processes.self};
    }

    std::expected<va::RegisterOutcome, va::RegisterFailure> register_new_revision(
        test::EngineFixture& fixture, va::IFileSystemAccess& files)
    {
        va::RegisterServices services{files, *fixture.repository.files, *fixture.repository.writer,
            fixture.media_writer, fixture.media, store, fixture.repository.context.repo, fixture.host, processes};
        va::RegisterRequest request;
        request.root = fixture.archive.root();
        request.new_revision = true;
        request.chunk_size = test_chunk_size;
        return va::register_archive(services, request, cancel, progress);
    }

    std::expected<va::VerifyOutcome, va::VerifyStop> add(RegisteredWithout& fixture, va::IFileSystemAccess& files,
        std::optional<std::string> run = std::nullopt)
    {
        va::VerifyServices services{files, *fixture.repository.files, fixture.media, store,
            fixture.repository.context.repo, fixture.repository.context.repository_uuid, fixture.host,
            *fixture.repository.writer, processes};
        va::VerifyRequest request;
        request.archive_uuid = fixture.archive_uuid;
        request.add_chunk_hashes = !run;
        request.chunk_size = test_chunk_size;
        request.resume_run = std::move(run);
        return va::verify_full(services, request, cancel, progress);
    }

    std::expected<va::AbandonableVerification, va::AbandonFailure> abandon(
        const va::Uuid& archive_uuid, std::string host = "test-host")
    {
        va::AbandonServices services{store, processes, std::move(host)};
        return va::abandon_verification(services, {archive_uuid, std::nullopt});
    }
};

// One order of the race with `register --new-revision` (decision 0033): a
// verification that adds chunk hashes is unfinished, stopped or still
// running, and a registration of the archive's next revision is refused at
// once, naming how to finish or abandon the verification, and changes
// nothing: no plan, nothing read. Once the verification has recorded revision
// 2, the registration goes ahead and records revision 3.
void a_registration_waits_for_a_verification_that_adds_chunk_hashes()
{
    {
        Halves fixture;
        const auto interrupted = fixture.interrupted();
        if (!interrupted) {
            return;
        }
        OtherProcess other(fixture);
        fixture.archive.bytes_read = 0;
        const auto refused = other.register_new_revision(fixture, fixture.archive);
        CHECK(!refused && refused.error().kind == va::RegisterFailure::Kind::Refused &&
              refused.error().message == waits_for(fixture.archive_uuid, interrupted->run_id));
        CHECK(fixture.archive.bytes_read == 0 && fixture.store.setting("registration:" + fixture.archive_uuid) == "" &&
              fixture.current_revision() == 1);
        // Register Archive names its own ways on: the verification that
        // refuses is recorded INCOMPLETE, so it has stopped.
        CHECK(!refused && refused.error().refusing &&
              refused.error().refusing->kind == va::RefusingOperation::Kind::AddingVerification &&
              refused.error().refusing->run_id == interrupted->run_id &&
              va::app::register_message_in_window(refused.error(), false) ==
                  waits_for_in_register_archive(fixture.archive_uuid, interrupted->run_id, false) &&
              va::app::register_message_in_window(refused.error(), true) ==
                  waits_for_in_register_archive(fixture.archive_uuid, interrupted->run_id, true));
        if (!refused && refused.error().refusing) {
            const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
            const auto runs = va::app::refusing_operation_runs(
                other.store, *refused.error().refusing, {&other.processes, fixture.host.host, {}}, now);
            CHECK(runs && !*runs);
        }

        const auto resumed = fixture.add(interrupted->run_id);
        CHECK(resumed && resumed->added_revision == 2);
        const auto next = other.register_new_revision(fixture, fixture.archive);
        CHECK(next && next->revision == 3 && fixture.current_revision() == 3);
    }
    {
        RegisteredWithout fixture;
        OtherProcess other(fixture);
        std::string running;
        std::optional<std::expected<va::RegisterOutcome, va::RegisterFailure>> meanwhile;
        std::optional<std::expected<bool, va::IoError>> runs_meanwhile;
        test::HookedFiles hooked(fixture.archive, "c.bin", [&] {
            const auto runs = fixture.repository.query("SELECT substr(key, 5) FROM settings WHERE key LIKE 'run:%'");
            running = runs;
            meanwhile = other.register_new_revision(fixture, fixture.archive);
            // Judged as the Archive List judges its row: its process, which
            // the other process sees running, reads.
            if (!*meanwhile && (*meanwhile).error().refusing) {
                const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
                runs_meanwhile = va::app::refusing_operation_runs(
                    other.store, *(*meanwhile).error().refusing, {&other.processes, fixture.host.host, {}}, now);
            }
        });
        va::VerifyServices services{hooked, *fixture.repository.files, fixture.media, fixture.store,
            fixture.repository.context.repo, fixture.repository.context.repository_uuid, fixture.host,
            *fixture.repository.writer, fixture.processes};
        va::VerifyRequest request;
        request.archive_uuid = fixture.archive_uuid;
        request.add_chunk_hashes = true;
        request.chunk_size = test_chunk_size;
        const auto outcome = va::verify_full(services, request, fixture.cancel, fixture.progress);
        CHECK(meanwhile && !*meanwhile && (*meanwhile).error().message == waits_for(fixture.archive_uuid, running));
        CHECK(runs_meanwhile && *runs_meanwhile && **runs_meanwhile);
        CHECK(meanwhile && !*meanwhile &&
              va::app::register_message_in_window((*meanwhile).error(), true) ==
                  waits_for_in_register_archive(fixture.archive_uuid, running, true));
        CHECK(outcome && outcome->run_id == running && outcome->added_revision == 2);
    }
}

// The other order: a registration of the archive's next revision is
// unfinished, stopped with saved files or still hashing, and a verification
// that adds chunk hashes is refused at once in the words of a registration
// through another root (decision 0025), changing nothing and reading
// nothing; Verify Archive names Resume and Abandon... on its row in the
// Archive List, then Register Archive's way to finish it, or, while it
// hashes, says so and to wait. Its plan as a version before decision 0022
// wrote it, which says nothing of its state (sample data), cannot be judged
// from one read, and Verify Archive names its ways on. Once the stopped
// registration is abandoned, the verification adds them; once the running
// one records revision 2, which has chunk hashes, the verification has none
// to add.
void a_verification_that_adds_chunk_hashes_waits_for_a_registration()
{
    const auto registration_refusal = [](const RegisteredWithout& fixture) {
        return "an unfinished registration of archive " + fixture.archive_uuid + " (root \"" +
               va::path_to_utf8(fixture.archive.root()) +
               "\") is creating revision 2; nothing was changed (finish it with `register --resume` first, or abandon "
               "it with `archive abandon-registration " +
               fixture.archive_uuid + "`)";
    };
    const auto registration_in_window = [](const RegisteredWithout& fixture, bool runs) {
        const auto creating = "an unfinished registration of archive " + fixture.archive_uuid + " (root \"" +
                              va::path_to_utf8(fixture.archive.root()) + "\") is creating revision 2";
        const auto ways = on_its_row + "; or finish it in Register Archive by entering its root, ticking Resume an "
                                       "interrupted registration of this folder or volume and pressing Register)";
        return runs ? creating + " and is still running; nothing was changed (wait for it to finish; if it stops, "
                                 "finish it " +
                          ways
                    : creating + "; nothing was changed (finish it first " + ways;
    };
    const auto now = [] { return std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()); };
    {
        RegisteredWithout fixture;
        OtherProcess other(fixture);
        test::HookedFiles hooked(fixture.archive, "c.bin", [&] { other.cancel.cancel(); });
        const auto stopped = other.register_new_revision(fixture, hooked);
        CHECK(!stopped && stopped.error().kind == va::RegisterFailure::Kind::Incomplete);
        CHECK(fixture.store.has_file_rows(fixture.archive_uuid, 2) == true);

        fixture.archive.bytes_read = 0;
        const auto runs_before = fixture.store.verify_runs(fixture.archive_uuid);
        const auto refused = fixture.add();
        CHECK(!refused && refused.error().kind == va::VerifyStop::Kind::Refused &&
              refused.error().message == registration_refusal(fixture));
        // The window names its own ways on (decision 0033); the registration
        // was cancelled, so its plan says that it stopped.
        CHECK(!refused && refused.error().refusing &&
              refused.error().refusing->kind == va::RefusingOperation::Kind::Registration &&
              refused.error().refusing->revision == 2 && !refused.error().refusing->this_root &&
              va::app::stop_message_in_window(refused.error(), false) == registration_in_window(fixture, false) &&
              va::app::stop_message_in_window(refused.error(), true) == registration_in_window(fixture, true));
        if (!refused && refused.error().refusing) {
            fixture.processes.others = {other.processes.self};
            const auto runs = va::app::refusing_operation_runs(
                fixture.store, *refused.error().refusing, {&fixture.processes, fixture.host.host, {}}, now());
            CHECK(runs && !*runs);
            fixture.processes.others.clear();
        }
        const auto key = "registration:" + fixture.archive_uuid;
        const auto plan = fixture.store.setting(key).value_or("");
        const auto written = nlohmann::json::parse(plan, nullptr, false);
        nlohmann::json older = nlohmann::json::object();
        for (const char* member : {"archive_uuid", "revision", "label", "created", "new_archive", "root", "copy_of"}) {
            if (written.is_object() && written.contains(member)) {
                older[member] = written[member];
            }
        }
        CHECK(older.size() >= 6 && fixture.store.set_setting(key, older.dump()));
        const auto by_older = fixture.add();
        CHECK(!by_older && by_older.error().refusing &&
              va::app::stop_message_in_window(by_older.error(), fixture.store, fixture.processes, fixture.host.host,
                  now()) == registration_in_window(fixture, false));
        CHECK(fixture.store.set_setting(key, plan));
        CHECK(fixture.archive.bytes_read == 0);
        const auto records = fixture.store.settings_with_prefix("run:");
        CHECK(records && records->empty());
        const auto runs_after = fixture.store.verify_runs(fixture.archive_uuid);
        CHECK(runs_before && runs_after && runs_after->size() == runs_before->size());

        va::AbandonServices services{fixture.store, fixture.processes, fixture.host.host};
        CHECK(va::abandon_registration(services, {fixture.archive_uuid, std::nullopt, std::nullopt}).has_value());
        const auto added = fixture.add();
        CHECK(added && added->added_revision == 2);
    }
    {
        RegisteredWithout fixture;
        OtherProcess other(fixture);
        // This fixture's process, the window's, sees the registration's run.
        fixture.processes.others = {other.processes.self};
        std::optional<std::expected<va::VerifyOutcome, va::VerifyStop>> meanwhile;
        std::optional<std::expected<bool, va::IoError>> runs_meanwhile;
        test::HookedFiles hooked(fixture.archive, "c.bin", [&] {
            meanwhile = fixture.add();
            if (!*meanwhile && (*meanwhile).error().refusing) {
                runs_meanwhile = va::app::refusing_operation_runs(fixture.store,
                    *(*meanwhile).error().refusing, {&fixture.processes, fixture.host.host, {}}, now());
            }
        });
        const auto registered = other.register_new_revision(fixture, hooked);
        CHECK(meanwhile && !*meanwhile && (*meanwhile).error().message == registration_refusal(fixture));
        CHECK(runs_meanwhile && *runs_meanwhile && **runs_meanwhile);
        CHECK(meanwhile && !*meanwhile &&
              va::app::stop_message_in_window((*meanwhile).error(), true) == registration_in_window(fixture, true));
        CHECK(registered && registered->revision == 2);
        const auto refused = fixture.add();
        CHECK(!refused && refused.error().message ==
                              "revision 2 of archive " + fixture.archive_uuid +
                                  " has chunk hashes already; a FULL verify compares them, and there are none to add");
    }
}

// A relocation and a verification that adds chunk hashes wait for each other
// the same way: a relocation is refused while the verification is unfinished,
// before it reads anything of the new root; the verification is refused while
// a relocation is unfinished, in the relocation's own words, also for one
// whose new carrier differs, and goes ahead once it is gone. Verify Archive
// names its ways on for a relocation's record that says nothing of its state,
// as one read cannot tell whether it runs.
void a_relocation_and_a_verification_that_adds_chunk_hashes_wait_for_each_other()
{
    {
        Halves fixture;
        const auto interrupted = fixture.interrupted();
        if (!interrupted) {
            return;
        }
        va::RelocateServices services{fixture.archive, *fixture.repository.files, *fixture.repository.writer,
            fixture.media, fixture.store, fixture.repository.context.repo, fixture.repository.context.repository_uuid,
            fixture.host, fixture.processes};
        fixture.archive.bytes_read = 0;
        const auto relocated = va::relocate_archive(
            services, {fixture.archive_uuid, fixture.archive.root() / "copy", std::nullopt}, fixture.cancel,
            fixture.progress);
        CHECK(!relocated && relocated.error().kind == va::RelocateFailure::Kind::Refused &&
              relocated.error().message == waits_for(fixture.archive_uuid, interrupted->run_id));
        CHECK(fixture.archive.bytes_read == 0 && fixture.store.setting(va::relocation_key(fixture.archive_uuid)) == "");
    }
    {
        RegisteredWithout fixture;
        const auto& uuid = fixture.archive_uuid;
        // The record of an unfinished relocation to another disk (sample data).
        const auto record = "INSERT INTO settings(key, value) VALUES('relocation:" + uuid + "', '{\"archive_uuid\":\"" +
                            uuid + "\",\"revision\":1,\"run_id\":\"c0ffee00-0000-4000-8000-0000000000c1\"," +
                            "\"root\":\"K:/SAMPLE\"}')";
        fixture.repository.execute(record.c_str());
        auto refused = fixture.add();
        CHECK(!refused && refused.error().kind == va::VerifyStop::Kind::Refused &&
              refused.error().message == "an unfinished relocation of archive " + uuid +
                                             " (to \"K:/SAMPLE\") exists; resume it with `archive relocate " + uuid +
                                             " K:/SAMPLE --resume`, or abandon it with `archive abandon-relocation " +
                                             uuid + "`");
        // Verify Archive names the Archive List's Abandon...; a relocation
        // has no window, so its command line goes on naming how to continue
        // it (decision 0033).
        const auto resume_or_abandon = "resume it on the command line with `archive relocate " + uuid +
                                       " K:/SAMPLE --resume`, or abandon it with Abandon... in the Archive List";
        CHECK(!refused && refused.error().refusing &&
              refused.error().refusing->kind == va::RefusingOperation::Kind::Relocation &&
              refused.error().refusing->run_id == "c0ffee00-0000-4000-8000-0000000000c1" &&
              !refused.error().refusing->differs &&
              va::app::stop_message_in_window(refused.error(), false) ==
                  "an unfinished relocation of archive " + uuid + " (to \"K:/SAMPLE\") exists; " + resume_or_abandon &&
              va::app::stop_message_in_window(refused.error(), true) ==
                  "an unfinished relocation of archive " + uuid +
                      " (to \"K:/SAMPLE\") exists and is still running; wait for it to finish; if it stops, " +
                      resume_or_abandon);
        // Judged from its record as the Archive List judges its row from one
        // read: this one says nothing of its state, so one read cannot tell
        // whether it runs, and Verify Archive names its ways on; one that
        // said a moment ago that it works, on another computer, runs; one
        // that says it stopped does not.
        const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
        if (!refused && refused.error().refusing) {
            auto refusing = *refused.error().refusing;
            const auto runs = [&] {
                const auto judged = va::app::refusing_operation_runs(
                    fixture.store, refusing, {&fixture.processes, fixture.host.host, {}}, now);
                CHECK(judged.has_value());
                return judged && *judged;
            };
            CHECK(!runs());
            CHECK(va::app::stop_message_in_window(refused.error(), fixture.store, fixture.processes,
                      fixture.host.host, now) ==
                  "an unfinished relocation of archive " + uuid + " (to \"K:/SAMPLE\") exists; " + resume_or_abandon);
            refusing.record["updated"] = std::format("{:%Y-%m-%dT%H:%M:%SZ}", now - std::chrono::seconds(10));
            va::record_attempt(refusing.record, {fixture.processes.self, "another-computer"});
            CHECK(runs());
            refusing.record["stopped"] = "2026-10-16T00:00:00Z";
            CHECK(!runs());
        }
        fixture.repository.execute(("UPDATE settings SET value = json_set(value, '$.differs', "
                                    "'2026-10-16T00:00:00Z') WHERE key = 'relocation:" +
                                    uuid + "'")
                                       .c_str());
        refused = fixture.add();
        CHECK(!refused && refused.error().message ==
                              "the unfinished relocation of archive " + uuid +
                                  " (to \"K:/SAMPLE\") found that the new carrier does not hold the recorded content "
                                  "and cannot be resumed; abandon it with `archive abandon-relocation " +
                                  uuid + "`, then relocate again");
        const auto found = "the unfinished relocation of archive " + uuid +
                           " (to \"K:/SAMPLE\") found that the new carrier does not hold the recorded content";
        CHECK(!refused && refused.error().refusing && refused.error().refusing->differs &&
              va::app::stop_message_in_window(refused.error(), false) ==
                  found + " and cannot be resumed; abandon it with Abandon... in the Archive List, then relocate again" &&
              va::app::stop_message_in_window(refused.error(), true) ==
                  found + " and is still running; wait for it to end; if it is then still listed in the Archive "
                          "List, abandon it there with Abandon..., then relocate again");
        fixture.repository.execute(("DELETE FROM settings WHERE key = 'relocation:" + uuid + "'").c_str());
        const auto added = fixture.add();
        CHECK(added && added->added_revision == 2);
    }
}

// Two at once (decision 0033): a second verification that adds chunk hashes
// to the archive, started while the first reads, is refused at once, naming
// the first; so is a second resume of a run, started while the first resume
// reads, naming the run that the first resume took it over with; and so is a
// resume while the process of the run before still runs on this computer
// (decision 0024). The first of each goes on and records revision 2.
void two_at_once()
{
    {
        RegisteredWithout fixture;
        OtherProcess other(fixture);
        std::string running;
        std::optional<std::expected<va::VerifyOutcome, va::VerifyStop>> second;
        test::HookedFiles hooked(fixture.archive, "c.bin", [&] {
            running = fixture.repository.query("SELECT substr(key, 5) FROM settings WHERE key LIKE 'run:%'");
            second = fixture.add();
        });
        const auto first = other.add(fixture, hooked);
        CHECK(second && !*second && refused_by((*second).error(), fixture.archive_uuid, running));
        CHECK(first && first->added_revision == 2 && first->run_id == running);
    }
    {
        Halves fixture;
        const auto interrupted = fixture.interrupted();
        if (!interrupted) {
            return;
        }
        OtherProcess other(fixture);
        std::string taken_over;
        std::optional<std::expected<va::VerifyOutcome, va::VerifyStop>> second;
        test::HookedFiles hooked(fixture.archive, "f.bin", [&] {
            taken_over = fixture.repository.query("SELECT substr(key, 5) FROM settings WHERE key LIKE 'run:%'");
            second = fixture.add(interrupted->run_id);
        });
        const auto first = other.add(fixture, hooked, interrupted->run_id);
        CHECK(!taken_over.empty() && taken_over != interrupted->run_id);
        CHECK(second && !*second && refused_by((*second).error(), fixture.archive_uuid, taken_over));
        CHECK(first && first->run_id == taken_over && first->added_revision == 2);
    }
    {
        // The run before is not recorded INCOMPLETE: it still reads, in this
        // fixture's process, which the other process sees running.
        RegisteredWithout fixture;
        OtherProcess other(fixture);
        std::optional<std::expected<va::VerifyOutcome, va::VerifyStop>> resumed;
        test::HookedFiles hooked(fixture.archive, "c.bin", [&] {
            const auto running =
                fixture.repository.query("SELECT substr(key, 5) FROM settings WHERE key LIKE 'run:%'");
            resumed = other.add(fixture, fixture.archive, running);
        });
        va::VerifyServices services{hooked, *fixture.repository.files, fixture.media, fixture.store,
            fixture.repository.context.repo, fixture.repository.context.repository_uuid, fixture.host,
            *fixture.repository.writer, fixture.processes};
        va::VerifyRequest request;
        request.archive_uuid = fixture.archive_uuid;
        request.add_chunk_hashes = true;
        request.chunk_size = test_chunk_size;
        const auto first = va::verify_full(services, request, fixture.cancel, fixture.progress);
        CHECK(resumed && !*resumed &&
              (*resumed).error().message == "this verification is still running in process 4242 (veriarchive-cli.exe) "
                                            "on this computer; resume it once that process has ended");
        CHECK(first && first->added_revision == 2);
    }
}

// Abandoning a stopped verification that adds chunk hashes (decision 0033)
// says what it removes, and removes its record, its checkpoints and the chunk
// hashes kept with them, and nothing else: the archive stays at revision 1,
// the history keeps its INCOMPLETE run. A registration of the next revision
// then goes ahead. Nothing to abandon says so.
void abandoning_a_verification_that_adds_chunk_hashes()
{
    Halves fixture;
    const auto interrupted = fixture.interrupted();
    if (!interrupted) {
        return;
    }
    va::AbandonServices services{fixture.store, fixture.processes, fixture.host.host};
    const auto shown = va::abandonable_verification(services, {fixture.archive_uuid, std::nullopt});
    CHECK(shown && shown->run_id == interrupted->run_id && shown->revision == 1 && shown->files == 3 &&
          shown->bytes == fixture.a.size() + fixture.b.size() + fixture.c.size() && shown->label == "ARCH-0001" &&
          shown->root == va::path_to_utf8(fixture.archive.root()));
    const auto other_run =
        va::abandonable_verification(services, {fixture.archive_uuid, "c0ffee00-0000-4000-8000-0000000000d1"});
    CHECK(!other_run && other_run.error().kind == va::AbandonFailure::Kind::Refused &&
          other_run.error().message == "the unfinished verification of archive " + fixture.archive_uuid +
                                           " that adds chunk hashes is no longer the one shown (it was resumed, "
                                           "finished or abandoned meanwhile); nothing was changed");

    // Recorded INCOMPLETE, it has stopped, though its process still runs in
    // the eyes of the other process that abandons it (the GUI's goes on
    // after a cancel).
    OtherProcess other(fixture);
    va::AbandonServices elsewhere{other.store, other.processes, fixture.host.host};
    const auto abandoned = va::abandon_verification(elsewhere, {fixture.archive_uuid, interrupted->run_id});
    CHECK(abandoned && abandoned->run_id == interrupted->run_id);
    CHECK(fixture.store.setting("run:" + interrupted->run_id) == "");
    const auto progress = fixture.store.progress(interrupted->run_id);
    const auto kept = fixture.store.progress_chunks(interrupted->run_id);
    CHECK(progress && progress->empty() && kept && kept->empty());
    const auto runs = fixture.store.verify_runs(fixture.archive_uuid);
    CHECK(runs && runs->size() == 1 && runs->front().result == "INCOMPLETE");
    CHECK(fixture.current_revision() == 1);

    const auto nothing = va::abandon_verification(services, {fixture.archive_uuid, std::nullopt});
    CHECK(!nothing && nothing.error().kind == va::AbandonFailure::Kind::Refused &&
          nothing.error().message ==
              "archive " + fixture.archive_uuid + " has no unfinished verification that adds chunk hashes");
    // Its resume is no more.
    const auto resumed = fixture.add(interrupted->run_id);
    CHECK(!resumed && resumed.error().message == "run " + interrupted->run_id + " is not an interrupted run");

    const auto registered = other.register_new_revision(fixture, fixture.archive);
    CHECK(registered && registered->revision == 2);
}

// Never while an attempt of it runs (decision 0024): refused while its process
// runs on this computer, and, where the process cannot be checked (another
// computer's), while its record said less than 3 minutes ago that it works.
// An attempt abandoned where its computer could not see it (here its record
// removed while it reads, as an abandon from another computer would) saves
// nothing more and records the verification alone, saying why, and leaves no
// checkpoints.
void a_verification_that_adds_chunk_hashes_is_not_abandoned_while_it_runs()
{
    {
        RegisteredWithout fixture;
        OtherProcess other(fixture);
        std::optional<std::expected<va::AbandonableVerification, va::AbandonFailure>> here;
        std::optional<std::expected<va::AbandonableVerification, va::AbandonFailure>> elsewhere;
        test::HookedFiles hooked(fixture.archive, "c.bin", [&] {
            here = other.abandon(fixture.archive_uuid);
            elsewhere = other.abandon(fixture.archive_uuid, "another-computer");
        });
        va::VerifyServices services{hooked, *fixture.repository.files, fixture.media, fixture.store,
            fixture.repository.context.repo, fixture.repository.context.repository_uuid, fixture.host,
            *fixture.repository.writer, fixture.processes};
        va::VerifyRequest request;
        request.archive_uuid = fixture.archive_uuid;
        request.add_chunk_hashes = true;
        request.chunk_size = test_chunk_size;
        const auto outcome = va::verify_full(services, request, fixture.cancel, fixture.progress);
        CHECK(here && !*here &&
              (*here).error().message == "the verification of archive " + fixture.archive_uuid +
                                             " that adds chunk hashes is still running in process 4242 "
                                             "(veriarchive-cli.exe) on this computer; abandon it once that process "
                                             "has ended");
        CHECK(elsewhere && !*elsewhere &&
              (*elsewhere).error().message == "an attempt of the verification of archive " + fixture.archive_uuid +
                                                  " that adds chunk hashes said it was working less than 3 minutes "
                                                  "ago; abandon it once it has stopped");
        CHECK(outcome && outcome->added_revision == 2);
    }
    {
        Halves fixture;
        std::string run;
        test::HookedFiles hooked(fixture.archive, "e.txt", [&] {
            run = fixture.repository.query("SELECT substr(key, 5) FROM settings WHERE key LIKE 'run:%'");
            fixture.repository.execute(("DELETE FROM settings WHERE key = 'run:" + run + "'").c_str());
        });
        va::VerifyServices services{hooked, *fixture.repository.files, fixture.media, fixture.store,
            fixture.repository.context.repo, fixture.repository.context.repository_uuid, fixture.host,
            *fixture.repository.writer, fixture.processes};
        va::VerifyRequest request;
        request.archive_uuid = fixture.archive_uuid;
        request.add_chunk_hashes = true;
        request.chunk_size = test_chunk_size;
        const auto outcome = va::verify_full(services, request, fixture.cancel, fixture.progress);
        CHECK(outcome && outcome->result == "PASSED" && outcome->added_revision == 0 &&
              outcome->not_added == "the verification was abandoned while it ran");
        CHECK(fixture.current_revision() == 1 && !std::filesystem::exists(fixture.manifest(2)));
        const auto progress = fixture.store.progress(run);
        const auto kept = fixture.store.progress_chunks(run);
        CHECK(progress && progress->empty() && kept && kept->empty());
    }
    {
        // A resume abandoned while it reads, and then interrupted: the
        // checkpoints it took over go too, with their kept chunk hashes.
        Halves fixture;
        const auto interrupted = fixture.interrupted();
        if (!interrupted) {
            return;
        }
        std::string run;
        test::HookedFiles hooked(fixture.archive, "f.bin", [&] {
            run = fixture.repository.query("SELECT substr(key, 5) FROM settings WHERE key LIKE 'run:%'");
            fixture.repository.execute(("DELETE FROM settings WHERE key = 'run:" + run + "'").c_str());
        });
        fixture.archive.node("g.bin").open_error = va::IoError{va::IoErrorKind::DeviceRemoved, 0, "unplugged"};
        va::VerifyServices services{hooked, *fixture.repository.files, fixture.media, fixture.store,
            fixture.repository.context.repo, fixture.repository.context.repository_uuid, fixture.host,
            *fixture.repository.writer, fixture.processes};
        va::VerifyRequest request;
        request.archive_uuid = fixture.archive_uuid;
        request.resume_run = interrupted->run_id;
        const auto outcome = va::verify_full(services, request, fixture.cancel, fixture.progress);
        CHECK(outcome && outcome->result == "INCOMPLETE" && outcome->run_id == run);
        const auto progress = fixture.store.progress(run);
        const auto kept = fixture.store.progress_chunks(run);
        CHECK(progress && progress->empty() && kept && kept->empty());
    }
}

// A run abandoned while it reads saves no checkpoint from then on (here 1 100
// small files besides those of the archive, sample data, and its record
// removed when the 100th is read, as an abandon from another computer would):
// the checkpoint of the first 1 000 files it matched saves nothing.
void a_run_abandoned_while_it_reads_saves_no_checkpoint()
{
    std::vector<std::pair<std::string, std::string>> many;
    for (int index = 0; index < 1100; ++index) {
        many.emplace_back(std::format("many/f{:04}.txt", index), "sample " + std::to_string(index));
    }
    RegisteredWithout fixture(many);
    std::string run;
    std::string saved_later;
    test::HookedFiles abandon(fixture.archive, "f0100.txt", [&] {
        run = fixture.repository.query("SELECT substr(key, 5) FROM settings WHERE key LIKE 'run:%'");
        fixture.repository.execute(("DELETE FROM settings WHERE key = 'run:" + run + "'").c_str());
    });
    test::HookedFiles look(abandon, "f1050.txt", [&] {
        saved_later = fixture.repository.query("SELECT COUNT(*) FROM verify_progress WHERE run_id = '" + run + "'");
    });
    va::VerifyServices services{look, *fixture.repository.files, fixture.media, fixture.store,
        fixture.repository.context.repo, fixture.repository.context.repository_uuid, fixture.host,
        *fixture.repository.writer, fixture.processes};
    va::VerifyRequest request;
    request.archive_uuid = fixture.archive_uuid;
    request.add_chunk_hashes = true;
    request.chunk_size = test_chunk_size;
    const auto outcome = va::verify_full(services, request, fixture.cancel, fixture.progress);
    CHECK(outcome && outcome->result == "PASSED" && outcome->not_added == "the verification was abandoned while it ran");
    CHECK(!run.empty() && saved_later == "0");
}

// Writes the repository through `inner`, except the files of a manifest,
// which cannot be created (a sample failure, as a full disk would be).
class UnwritableManifests final : public va::IFileWriter {
public:
    explicit UnwritableManifests(va::IFileWriter& inner) : inner_(inner) {}

    std::expected<std::unique_ptr<va::IOutputFile>, va::IoError> create(
        const std::filesystem::path& destination) override
    {
        if (va::path_to_utf8(destination.filename()).contains(".vam")) {
            return std::unexpected(va::IoError{va::IoErrorKind::Other, 0, "disk full (sample)"});
        }
        return inner_.create(destination);
    }
    std::expected<void, va::IoError> remove(const std::filesystem::path& file) override
    {
        return inner_.remove(file);
    }
    std::expected<void, va::IoError> create_directories(const std::filesystem::path& directory) override
    {
        return inner_.create_directories(directory);
    }

private:
    va::IFileWriter& inner_;
};

// A run that adds chunk hashes and cannot record itself, as its revision's
// manifest or its record cannot be written (decision 0033), says last in its
// record that it stopped, as a registration's plan does (decisions 0022,
// 0024): its process may go on, as the GUI's window does, and another
// process, which sees that process running, takes the run for stopped. There
// the Archive List shows it stopped, with Abandon..., abandoning it would go
// ahead, and its resume records revision 2 reading nothing again. The run is
// in no row of the history. Here the run's process is this fixture's, which
// the other process sees running; the record fails at a trigger (sample data).
void a_run_that_cannot_record_itself_says_that_it_stopped()
{
    for (const bool manifest_fails : {true, false}) {
        RegisteredWithout fixture;
        UnwritableManifests unwritable(*fixture.repository.writer);
        if (!manifest_fails) {
            fixture.repository.execute("CREATE TRIGGER sample_failure BEFORE INSERT ON archive_revisions "
                                       "BEGIN SELECT RAISE(ABORT, 'sample failure'); END;");
        }
        va::IFileWriter& writer = manifest_fails ? static_cast<va::IFileWriter&>(unwritable)
                                                 : *fixture.repository.writer;
        va::VerifyServices services{fixture.archive, *fixture.repository.files, fixture.media, fixture.store,
            fixture.repository.context.repo, fixture.repository.context.repository_uuid, fixture.host, writer,
            fixture.processes};
        va::VerifyRequest request;
        request.archive_uuid = fixture.archive_uuid;
        request.add_chunk_hashes = true;
        request.chunk_size = test_chunk_size;
        const auto failed = va::verify_full(services, request, fixture.cancel, fixture.progress);
        if (!manifest_fails) {
            fixture.repository.execute("DROP TRIGGER sample_failure;");
        }
        const auto run = fixture.repository.query("SELECT substr(key, 5) FROM settings WHERE key LIKE 'run:%'");
        const auto resume = "; resume the run with `verify --full " + fixture.archive_uuid + " --resume " + run +
                            "` to record it";
        CHECK(!failed && failed.error().kind == va::VerifyStop::Kind::Repository &&
              failed.error().message ==
                  (manifest_fails ? "verification finished as PASSED but its revision with chunk hashes could not be "
                                    "written: disk full (sample)"
                                  : "verification finished as PASSED but could not be recorded: cannot run SQL: "
                                    "sample failure") +
                      resume);
        const auto record =
            nlohmann::json::parse(fixture.store.setting("run:" + run).value_or(""), nullptr, false);
        CHECK(record.is_object() && record.contains("stopped") && va::recorded_attempt(record).has_value());
        const auto runs = fixture.store.verify_runs(fixture.archive_uuid);
        CHECK(runs && runs->empty() && fixture.current_revision() == 1);

        OtherProcess other(fixture);
        const va::app::AttemptCheck attempts{&other.processes, fixture.host.host, {}};
        const auto read = va::app::read_unfinished_operations(fixture.repository.context.repo, {},
            [](const std::filesystem::path&) -> std::optional<std::uint64_t> { return std::nullopt; }, attempts);
        CHECK(read && read->operations.size() == 1);
        if (read && read->operations.size() == 1) {
            const auto& operation = read->operations.front();
            const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
            const va::app::OperationWatch::Clock::time_point start{};
            va::app::OperationWatch watch;
            watch.update(operation, start, now);
            const auto row = va::app::describe_operation(va::app::Text{}, watch, start, now);
            CHECK(operation.run_id == run && row.state == "stopped; it can be resumed" && row.abandonable);
        }
        va::AbandonServices elsewhere{other.store, other.processes, fixture.host.host};
        const auto abandonable = va::abandonable_verification(elsewhere, {fixture.archive_uuid, run});
        CHECK(abandonable && abandonable->run_id == run);

        fixture.archive.bytes_read = 0;
        const auto resumed = other.add(fixture, fixture.archive, run);
        CHECK(resumed && resumed->added_revision == 2 && fixture.archive.bytes_read == 0);
        if (!resumed) {
            std::cerr << "  stopped: " << resumed.error().message << '\n';
        }
    }
}

// The verification that adds chunk hashes which Verify Archive's Resume
// continues once its archive is chosen (decision 0033), also after the window
// that ran it was closed: the archive's unfinished one when the Archive List
// would show it stopped, as when it was recorded INCOMPLETE, though the
// process that cancelled it goes on (the GUI's window), its record says that
// it stopped, the process it names has ended, or, on another computer, its
// record has said nothing for minutes. Not one that runs, here in the command
// line or in this window while the window runs it, or is held up, its process
// running; nor one that adds no chunk hashes, or adds them to an earlier
// revision. The window is a process of its own, in whose eyes this fixture's
// process (the command line's) runs. Records written by hand are sample data,
// as the engine writes them.
void what_resume_continues()
{
    using namespace std::chrono_literals;
    {
        Halves fixture;
        const auto interrupted = fixture.interrupted();
        if (!interrupted) {
            return;
        }
        test::FakeProcessTable window;
        window.self = {5151, "boot-1:2000", "veriarchive-gui.exe"};
        window.others = {fixture.processes.self};
        const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
        const auto found = va::app::resumable_adding_verification(
            fixture.store, fixture.archive_uuid, {&window, fixture.host.host, {}}, now);
        CHECK(found && *found && (*found)->run_id == interrupted->run_id && !(*found)->root &&
              (*found)->archive_uuid == fixture.archive_uuid);
    }

    RegisteredWithout fixture;
    const auto& uuid = fixture.archive_uuid;
    const std::string run = "c0ffee00-0000-4000-8000-0000000000e1";
    const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    const va::ProcessIdentity command_line{1000, "boot-1:1000", "veriarchive-cli.exe"};
    test::FakeProcessTable window;
    window.self = {5151, "boot-1:2000", "veriarchive-gui.exe"};
    window.others = {command_line};
    bool window_runs = false;
    const va::app::AttemptCheck attempts{&window, "test-host", [&] { return window_runs; }};
    const auto timestamp = [](std::chrono::sys_seconds time) { return std::format("{:%Y-%m-%dT%H:%M:%SZ}", time); };
    // The record of a run of revision `revision` that adds chunk hashes (or
    // none), last working `silent` ago in `process` on `host`.
    const auto record = [&](std::chrono::seconds silent, const va::ProcessIdentity& process, std::string host,
                            bool stopped = false, std::uint32_t revision = 1, bool adds = true) {
        nlohmann::json value{{"archive_uuid", uuid}, {"revision", revision},
            {"root", va::path_to_utf8(fixture.archive.root())}, {"started", timestamp(now - 1h)},
            {"updated", timestamp(now - silent)}, {"files_total", 3}, {"bytes_total", 1000}};
        if (adds) {
            value["add_chunk_hashes"] = test_chunk_size;
            va::record_attempt(value, {process, std::move(host)});
        }
        if (stopped) {
            value["stopped"] = timestamp(now - silent);
        }
        CHECK(fixture.store.set_setting("run:" + run, value.dump()));
    };
    const auto resumes = [&]() -> std::string {
        const auto found = va::app::resumable_adding_verification(fixture.store, uuid, attempts, now);
        CHECK(found.has_value());
        if (!found || !*found) {
            return "none";
        }
        CHECK(!(*found)->root && (*found)->archive_uuid == uuid);
        return (*found)->run_id;
    };

    CHECK(resumes() == "none");
    record(10s, command_line, "test-host");
    CHECK(resumes() == "none");  // it runs
    record(10min, command_line, "test-host");
    CHECK(resumes() == "none");  // held up: its process runs
    record(10s, command_line, "test-host", true);
    CHECK(resumes() == run);  // it says that it stopped
    record(10s, command_line, "another-computer");
    CHECK(resumes() == "none");  // it may run there
    record(10min, command_line, "another-computer");
    CHECK(resumes() == run);  // silent for minutes there
    record(10s, window.self, "test-host");
    window_runs = true;
    CHECK(resumes() == "none");  // this window runs it
    window_runs = false;
    CHECK(resumes() == run);  // the window runs nothing: it stopped
    window.others.clear();
    record(10s, command_line, "test-host");
    CHECK(resumes() == run);  // its process has ended
    record(10min, command_line, "test-host", true, 0);
    CHECK(resumes() == "none");  // of an earlier revision: it can never finish
    record(10min, command_line, "test-host", true, 1, false);
    CHECK(resumes() == "none");  // it adds none
}

// The windows' refusal while another operation that creates the archive's
// next revision is unfinished (decision 0033) says whether that operation
// still runs, judged as the Archive List judges its row from one read
// (refusing_operation_runs), as Resume and Abandon... are offered for it only
// once it has stopped. The window is a process of its own. A second
// verification, refused while the first reads in a process that the window
// sees running, finds it running; one refused by a run recorded INCOMPLETE
// finds it stopped, though the process that recorded it goes on. Records
// written by hand (sample data, as the engines write them) give a
// verification, a registration's plan and a relocation's record that run,
// are held up, silent for minutes while their process runs, say that they
// stopped, or whose process has ended; and one on another computer, silent
// for a moment and for minutes. A plan and a record that say nothing of
// their state, as a version before decision 0022 wrote them, cannot be
// judged from one read, and are not taken for running.
void what_the_refusal_in_the_window_says()
{
    using namespace std::chrono_literals;
    const auto now = [] { return std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()); };
    {
        RegisteredWithout fixture;
        OtherProcess other(fixture);
        fixture.processes.others = {other.processes.self};
        std::string running;
        std::optional<std::expected<va::VerifyOutcome, va::VerifyStop>> second;
        std::optional<std::expected<bool, va::IoError>> runs;
        test::HookedFiles hooked(fixture.archive, "c.bin", [&] {
            running = fixture.repository.query("SELECT substr(key, 5) FROM settings WHERE key LIKE 'run:%'");
            second = fixture.add();
            if (second && !*second && (*second).error().refusing) {
                runs = va::app::refusing_operation_runs(
                    fixture.store, *(*second).error().refusing, {&fixture.processes, fixture.host.host, {}}, now());
            }
        });
        const auto first = other.add(fixture, hooked);
        CHECK(second && !*second && refused_by((*second).error(), fixture.archive_uuid, running));
        CHECK(runs && *runs && **runs);
        CHECK(first && first->added_revision == 2);
    }
    Halves fixture;
    const auto interrupted = fixture.interrupted();
    if (!interrupted) {
        return;
    }
    test::FakeProcessTable window;
    window.self = {5151, "boot-1:2000", "veriarchive-gui.exe"};
    window.others = {fixture.processes.self};
    const va::app::AttemptCheck attempts{&window, fixture.host.host, {}};
    const auto refused = fixture.add();
    CHECK(!refused && refused_by(refused.error(), fixture.archive_uuid, interrupted->run_id));
    if (refused || !refused.error().refusing) {
        return;
    }
    const auto recorded_incomplete =
        va::app::refusing_operation_runs(fixture.store, *refused.error().refusing, attempts, now());
    CHECK(recorded_incomplete && !*recorded_incomplete);

    // The refusing operation as another read finds its record, last working
    // `silent` ago in the command line's process on `host`, with or without
    // a stop mark; a verification not recorded.
    const auto timestamp = [](std::chrono::sys_seconds time) { return std::format("{:%Y-%m-%dT%H:%M:%SZ}", time); };
    const auto runs = [&](va::RefusingOperation refusing, std::chrono::seconds silent, const std::string& host,
                          bool stopped_mark) {
        refusing.record["updated"] = timestamp(now() - silent);
        refusing.record.erase("process");
        va::record_attempt(refusing.record, {fixture.processes.self, host});
        if (stopped_mark) {
            refusing.record["stopped"] = timestamp(now() - silent);
        }
        const auto judged = va::app::refusing_operation_runs(fixture.store, refusing, attempts, now());
        CHECK(judged.has_value());
        return judged && *judged;
    };
    auto verification = *refused.error().refusing;
    verification.run_id = "c0ffee00-0000-4000-8000-0000000000e2";
    auto registration = va::refusing_registration(fixture.archive_uuid,
        nlohmann::json{{"archive_uuid", fixture.archive_uuid}, {"revision", 2u}, {"label", "ARCH-0001"},
            {"created", timestamp(now() - 1h)}, {"new_archive", false}, {"root", "K:/SAMPLE"}});
    auto relocation = va::refusing_relocation(fixture.archive_uuid,
        nlohmann::json{{"archive_uuid", fixture.archive_uuid}, {"revision", 1u},
            {"run_id", "c0ffee00-0000-4000-8000-0000000000e3"}, {"started", timestamp(now() - 1h)},
            {"root", "K:/SAMPLE"}});
    CHECK(registration.kind == va::RefusingOperation::Kind::Registration && registration.revision == 2 &&
          registration.root == "K:/SAMPLE" && relocation.kind == va::RefusingOperation::Kind::Relocation &&
          relocation.revision == 1 && relocation.run_id == "c0ffee00-0000-4000-8000-0000000000e3");
    for (const auto& refusing : {verification, registration, relocation}) {
        window.others = {fixture.processes.self};
        CHECK(runs(refusing, 10s, fixture.host.host, false));                 // it runs
        CHECK(runs(refusing, 10min, fixture.host.host, false));               // held up: its process runs
        CHECK(runs(refusing, 10s, "another-computer", false));                // it may run there
        CHECK(!runs(refusing, 10min, "another-computer", false));             // silent for minutes there
        CHECK(!runs(refusing, 10s, fixture.host.host, true));                 // it says that it stopped
        window.others.clear();
        CHECK(!runs(refusing, 10s, fixture.host.host, false));                // its process has ended
    }
    // As a version before decision 0022 wrote them: neither "updated" nor a
    // process, which one read cannot judge.
    window.others = {fixture.processes.self};
    for (const auto& says_nothing : {registration, relocation}) {
        const auto judged = va::app::refusing_operation_runs(fixture.store, says_nothing, attempts, now());
        CHECK(judged && !*judged);
    }
}

// The windows judge the operation that refuses them in the app
// (register_message_in_window and stop_message_in_window, given the
// repository, this computer's process table, its name and the time), as
// refusing_operation_runs judges it, with two rules of the window's own
// (decision 0033). Records written by hand (sample data, as the engines write
// them), the window a process of its own: a verification that adds chunk
// hashes, which said a moment ago that it works in a process that the window
// sees running, still runs in both views; while the repository cannot be read
// (its table of runs renamed away for a moment), the window cannot judge it
// and names its ways on; silent for minutes on another computer, as the
// window's clock tells, it has stopped. A registration's plan that names the
// window's own process and says nothing of a stop, as the window leaves its
// own registration when it ended and the stop mark could not be written, is
// of an attempt that has ended, as the window's one operation is the one
// refused (decision 0024): both views name their ways on, though another
// process, which sees the window's running, takes it for running. So do both
// views for a plan that says nothing of its state, as a version before
// decision 0022 wrote it, which one read cannot judge. A refusal that no
// other operation makes reads as it is.
void the_window_judges_the_operation_that_refuses_it()
{
    using namespace std::chrono_literals;
    const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    const auto timestamp = [](std::chrono::sys_seconds time) { return std::format("{:%Y-%m-%dT%H:%M:%SZ}", time); };
    Halves fixture;
    const auto interrupted = fixture.interrupted();
    if (!interrupted) {
        return;
    }
    const auto refused = fixture.add();
    CHECK(!refused && refused.error().refusing);
    if (refused || !refused.error().refusing) {
        return;
    }
    const auto& uuid = fixture.archive_uuid;
    const auto& host = fixture.host.host;
    test::FakeProcessTable window;
    window.self = {5151, "boot-1:2000", "veriarchive-gui.exe"};
    window.others = {fixture.processes.self};

    auto verification = *refused.error().refusing;
    verification.run_id = "c0ffee00-0000-4000-8000-0000000000e4";
    verification.record["updated"] = timestamp(now - 10s);
    verification.record.erase("process");
    va::record_attempt(verification.record, {fixture.processes.self, host});
    const va::VerifyStop stopped_by_verification{va::VerifyStop::Kind::Refused, "refused (sample)", {}, {}, verification};
    const va::RegisterFailure refused_by_verification{
        va::RegisterFailure::Kind::Refused, "refused (sample)", std::nullopt, verification};
    const auto verify_archive_says = [&](const va::VerifyStop& stop) {
        return va::app::stop_message_in_window(stop, fixture.store, window, host, now);
    };
    const auto register_archive_says = [&](const va::RegisterFailure& failure) {
        return va::app::register_message_in_window(failure, fixture.store, window, host, now);
    };
    CHECK(verify_archive_says(stopped_by_verification) == runs_in_window(uuid, verification.run_id));
    CHECK(register_archive_says(refused_by_verification) ==
          waits_for_in_register_archive(uuid, verification.run_id, true));
    const auto ways_on_in_verify_archive = waits_for_in_window(uuid, verification.run_id);
    fixture.repository.execute("ALTER TABLE verify_runs RENAME TO verify_runs_away");
    CHECK(!fixture.store.verify_run(verification.run_id).has_value());
    CHECK(verify_archive_says(stopped_by_verification) == ways_on_in_verify_archive);
    CHECK(register_archive_says(refused_by_verification) ==
          waits_for_in_register_archive(uuid, verification.run_id, false));
    fixture.repository.execute("ALTER TABLE verify_runs_away RENAME TO verify_runs");
    CHECK(verify_archive_says(stopped_by_verification) == runs_in_window(uuid, verification.run_id));
    // Silent for minutes on another computer, as the window's clock tells.
    auto elsewhere = verification;
    elsewhere.record["updated"] = timestamp(now - 10min);
    va::record_attempt(elsewhere.record, {fixture.processes.self, "another-computer"});
    CHECK(verify_archive_says({va::VerifyStop::Kind::Refused, "refused (sample)", {}, {}, elsewhere}) ==
          ways_on_in_verify_archive);

    auto registration = va::refusing_registration(uuid,
        nlohmann::json{{"archive_uuid", uuid}, {"revision", 2u}, {"label", "ARCH-0001"},
            {"created", timestamp(now - 1h)}, {"new_archive", false}, {"root", "K:/SAMPLE"},
            {"updated", timestamp(now - 10s)}});
    va::record_attempt(registration.record, {window.self, host});
    const va::VerifyStop stopped_by_registration{va::VerifyStop::Kind::Refused, "refused (sample)", {}, {}, registration};
    const va::RegisterFailure refused_by_registration{
        va::RegisterFailure::Kind::Refused, "refused (sample)", std::nullopt, registration};
    const auto creating = "an unfinished registration of archive " + uuid +
                          " (root \"K:/SAMPLE\") is creating revision 2; nothing was changed (finish it first " +
                          on_its_row + "; or finish it ";
    const auto ways_on_for_registration_in_verify_archive =
        creating + "in Register Archive by entering its root, ticking Resume an interrupted registration of this "
                   "folder or volume and pressing Register)";
    const auto ways_on_for_registration_in_register_archive =
        creating + "by entering its root above, ticking Resume an interrupted registration of this folder or "
                   "volume and pressing Register)";
    CHECK(verify_archive_says(stopped_by_registration) == ways_on_for_registration_in_verify_archive);
    CHECK(register_archive_says(refused_by_registration) == ways_on_for_registration_in_register_archive);
    test::FakeProcessTable outside;
    outside.self = {7000, "boot-1:7000", "veriarchive-cli.exe"};
    outside.others = {window.self};
    const auto from_outside = va::app::refusing_operation_runs(fixture.store, registration, {&outside, host, {}}, now);
    CHECK(from_outside && *from_outside);

    const auto says_nothing = va::refusing_registration(uuid,
        nlohmann::json{{"archive_uuid", uuid}, {"revision", 2u}, {"label", "ARCH-0001"},
            {"created", timestamp(now - 1h)}, {"new_archive", false}, {"root", "K:/SAMPLE"}});
    CHECK(verify_archive_says({va::VerifyStop::Kind::Refused, "refused (sample)", {}, {}, says_nothing}) ==
          ways_on_for_registration_in_verify_archive);
    CHECK(register_archive_says({va::RegisterFailure::Kind::Refused, "refused (sample)", std::nullopt, says_nothing}) ==
          ways_on_for_registration_in_register_archive);

    const va::VerifyStop of_its_own{va::VerifyStop::Kind::Refused, "a refusal of its own (sample)", {}, {}, std::nullopt};
    const va::RegisterFailure failed_on_its_own{
        va::RegisterFailure::Kind::Refused, "a refusal of its own (sample)", std::nullopt, std::nullopt};
    CHECK(verify_archive_says(of_its_own) == "a refusal of its own (sample)");
    CHECK(register_archive_says(failed_on_its_own) == "a refusal of its own (sample)");
}

// Which run Verify Archive's Resume continues (decision 0033): the run
// interrupted last in the window while its archive is chosen, also one that
// adds no chunk hashes; the chosen archive's stopped verification that adds
// them while another archive's run, or none, was interrupted in the window;
// and the window's run when the chosen archive has none (sample ids).
void which_run_resume_continues()
{
    using va::app::RunToResume;
    const RunToResume here{"c0ffee00-0000-4000-8000-0000000000f1", std::filesystem::path("K:/SAMPLE"),
        "c0ffee00-0000-4000-8000-00000000000a"};
    const RunToResume same_archive{"c0ffee00-0000-4000-8000-0000000000f2", std::nullopt,
        "c0ffee00-0000-4000-8000-00000000000a"};
    const RunToResume other_archive{"c0ffee00-0000-4000-8000-0000000000f3", std::nullopt,
        "c0ffee00-0000-4000-8000-00000000000b"};
    CHECK(!va::app::run_to_resume(std::nullopt, std::nullopt));
    const auto own = va::app::run_to_resume(here, std::nullopt);
    CHECK(own && own->run_id == here.run_id && own->root == here.root);
    const auto chosen = va::app::run_to_resume(std::nullopt, other_archive);
    CHECK(chosen && chosen->run_id == other_archive.run_id && !chosen->root);
    const auto own_archive = va::app::run_to_resume(here, same_archive);
    CHECK(own_archive && own_archive->run_id == here.run_id && own_archive->root == here.root);
    const auto another = va::app::run_to_resume(here, other_archive);
    CHECK(another && another->run_id == other_archive.run_id && another->archive_uuid == other_archive.archive_uuid);
}

// What the command line gives of adding chunk hashes (decision 0033, points 8
// to 10), from the outcomes of runs here: in English and Traditional Chinese
// where people read it, and --json in English. Verify's line and its
// chunk_hashes for a run that added them, one that added none and one
// interrupted, nothing for a run that adds none, and the same line in the
// GUI's report; archive list|show's unfinished_verification of the run
// interrupted, also with sample values (not counted, without a process, not
// resumable); and archive abandon-verification's verification.
void what_the_command_line_gives()
{
    using va::app::chunk_hashes_line;
    using va::app::chunk_hashes_member;
    const va::app::Text english;
    const va::app::Text chinese(va::app::Language::TraditionalChinese);
    // An object of one member, as these functions give it.
    const auto member = [](const char* name, const nlohmann::json& value) { return nlohmann::json{{name, value}}; };
    {
        RegisteredWithout fixture;
        const auto added = fixture.add();
        CHECK(added && added->added_revision == 2 && added->added_chunks == 5);
        if (added) {
            const auto& root = added->added_merkle_root;
            CHECK(chunk_hashes_line(english, *added) ==
                  "chunk hashes added: revision 2 records them (chunk size 64 KiB, 5 chunks, Merkle root " + root + ")");
            CHECK(chunk_hashes_line(chinese, *added) ==
                  "已加入區塊雜湊：由版次 2 記錄（區塊大小 64 KiB，5 個區塊，總碼 " + root + "）");
            CHECK(chunk_hashes_line(english, *added, true) ==
                  "Chunk hashes added: revision 2 records them (chunk size 64 KiB, 5 chunks, Merkle root " + root + ")");
            CHECK(chunk_hashes_line(chinese, *added, true) ==
                  "已加入區塊雜湊：由版次 2 記錄（區塊大小 64 KiB，5 個區塊，總碼 " + root + "）");
            const nlohmann::json expected{{"chunk_size", test_chunk_size}, {"added_revision", 2}, {"chunks", 5},
                {"merkle_root", root}, {"not_added", nullptr}};
            CHECK(chunk_hashes_member(*added) == member("chunk_hashes", expected));
        }
    }
    {
        RegisteredWithout fixture;
        fixture.archive.node("b.txt").content = "B (sample data)";
        const auto failed = fixture.add();
        CHECK(failed && failed->result == "FAILED");
        if (failed) {
            const std::string why = "the verification did not pass; revision 1 stays without chunk hashes";
            CHECK(chunk_hashes_line(english, *failed) == "chunk hashes not added: " + why);
            CHECK(chunk_hashes_line(english, *failed, true) == "Chunk hashes not added: " + why);
            CHECK(chunk_hashes_line(chinese, *failed) == "未加入區塊雜湊：驗證未通過；版次 1 仍不含區塊雜湊");
            CHECK(chunk_hashes_line(chinese, *failed, true) == "未加入區塊雜湊：驗證未通過；版次 1 仍不含區塊雜湊");
            const nlohmann::json expected{{"chunk_size", test_chunk_size}, {"added_revision", nullptr},
                {"chunks", nullptr}, {"merkle_root", nullptr}, {"not_added", why}};
            CHECK(chunk_hashes_member(*failed) == member("chunk_hashes", expected));
        }
    }
    {
        RegisteredWithout fixture;
        const auto plain = fixture.verify(fixture.archive.root());
        CHECK(plain && plain->adding_chunk_size == 0);
        if (plain) {
            CHECK(chunk_hashes_line(english, *plain).empty() && chunk_hashes_line(english, *plain, true).empty());
            CHECK(chunk_hashes_member(*plain) == nlohmann::json::object());
        }
    }

    Halves fixture;
    const auto interrupted = fixture.interrupted();
    if (!interrupted) {
        return;
    }
    CHECK(chunk_hashes_line(english, *interrupted).empty() && chunk_hashes_line(english, *interrupted, true).empty());
    const nlohmann::json reading{{"chunk_size", test_chunk_size}, {"added_revision", nullptr}, {"chunks", nullptr},
        {"merkle_root", nullptr}, {"not_added", nullptr}};
    CHECK(chunk_hashes_member(*interrupted) == member("chunk_hashes", reading));

    // Its record last said a while ago that it worked (a sample time).
    const std::string updated = "2026-10-16T01:02:03Z";
    fixture.repository.execute(("UPDATE settings SET value = json_set(value, '$.updated', '" + updated +
                                "') WHERE key = 'run:" + interrupted->run_id + "'")
                                   .c_str());
    const auto root = va::path_to_utf8(fixture.archive.root());
    const auto matched = fixture.a.size() + fixture.b.size() + fixture.c.size();
    const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    const auto unfinished = fixture.store.unfinished_verifications(now, {}, true);
    CHECK(unfinished && unfinished->size() == 1);
    if (unfinished && unfinished->size() == 1) {
        const auto& verification = unfinished->front();
        CHECK(!verification.started.empty() && verification.started != updated);
        const auto operation =
            va::app::verification_operation(verification, {&fixture.processes, fixture.host.host, {}});
        nlohmann::json expected{{"root", root}, {"revision", 1}, {"run_id", interrupted->run_id},
            {"chunk_size", test_chunk_size}, {"started", verification.started}, {"updated", updated},
            {"state", "stopped"}, {"resumable", true}, {"files_done", 3}, {"bytes_done", matched}, {"files_total", 6},
            {"bytes_total", matched + fixture.e.size() + fixture.f.size() + fixture.g.size()},
            {"process", {{"id", 4242}, {"program", "veriarchive-cli.exe"}, {"host", "test-host"}}}};
        CHECK(va::app::unfinished_verification_member(verification, operation, now) ==
              member("unfinished_verification", expected));

        auto uncounted = verification;
        uncounted.done_counted = false;
        uncounted.resumable = false;
        uncounted.updated.clear();
        auto without_process = operation;
        without_process.process.reset();
        expected["files_done"] = nullptr;
        expected["bytes_done"] = nullptr;
        expected["resumable"] = false;
        expected["updated"] = nullptr;
        expected.erase("process");
        CHECK(va::app::unfinished_verification_member(uncounted, without_process, now) ==
              member("unfinished_verification", expected));
    }

    va::AbandonServices services{fixture.store, fixture.processes, fixture.host.host};
    const auto abandonable = va::abandonable_verification(services, {fixture.archive_uuid, std::nullopt});
    CHECK(abandonable.has_value());
    if (abandonable) {
        const nlohmann::json shown{{"archive_uuid", fixture.archive_uuid}, {"label", "ARCH-0001"}, {"revision", 1},
            {"root", root}, {"run_id", interrupted->run_id}, {"started", abandonable->started},
            {"saved_progress", {{"files", 3}, {"bytes", matched}}}};
        CHECK(va::app::abandonable_verification_json(*abandonable) == shown);
        CHECK(va::app::abandonable_verification_member(*abandonable) == member("verification", shown));
    }
}

// A registration plan whose revision is recorded, and the record of a
// relocation whose next revision is recorded, can never finish (decisions
// 0023, 0026): they refuse nothing (sample records written by hand, as an
// operation of an older version may leave them).
void what_can_no_longer_finish_refuses_nothing()
{
    RegisteredWithout fixture;
    const auto& uuid = fixture.archive_uuid;
    const auto plan = "INSERT INTO settings(key, value) VALUES('registration:" + uuid + "', '{\"archive_uuid\":\"" +
                      uuid + "\",\"revision\":1,\"label\":\"ARCH-0001\",\"created\":\"2026-10-16T00:00:00Z\"," +
                      "\"new_archive\":true,\"root\":\"/vol\"}')";
    const auto relocation = "INSERT INTO settings(key, value) VALUES('relocation:" + uuid + "', '{\"archive_uuid\":\"" +
                            uuid + "\",\"revision\":0,\"run_id\":\"c0ffee00-0000-4000-8000-0000000000c2\"," +
                            "\"root\":\"K:/SAMPLE\"}')";
    fixture.repository.execute(plan.c_str());
    fixture.repository.execute(relocation.c_str());
    const auto added = fixture.add();
    CHECK(added && added->added_revision == 2);
}

// Reports what `inner` reports, and runs `hook` once, the first time the
// attached volumes are listed: a verification of an archive named by its uuid
// looks for it there after it found the run it resumes, and before it takes
// that run over.
class HookedMedia final : public va::IMediaEnumerator {
public:
    HookedMedia(va::IMediaEnumerator& inner, std::function<void()> hook) : inner_(inner), hook_(std::move(hook)) {}

    std::expected<std::vector<va::VolumeInfo>, va::IoError> volumes() override
    {
        if (hook_) {
            const auto hook = std::move(hook_);
            hook_ = nullptr;
            hook();
        }
        return inner_.volumes();
    }
    std::expected<va::VolumeInfo, va::IoError> volume_for_path(const std::filesystem::path& path) override
    {
        return inner_.volume_for_path(path);
    }
    std::expected<va::DeviceHealth, va::IoError> device_health(const va::PhysicalDeviceInfo& device) override
    {
        return inner_.device_health(device);
    }

private:
    va::IMediaEnumerator& inner_;
    std::function<void()> hook_;
};

// Two resumes of one run that both found it before either took it over: the
// one that takes it over second finds it gone, in the transaction that would
// take it over, and refuses, naming the run that took it over while that one
// is unfinished; once that one has recorded revision 2, the run cannot be
// resumed (R-104).
void two_resumes_that_found_the_run_at_once()
{
    for (const bool first_finishes : {false, true}) {
        Halves fixture;
        const auto interrupted = fixture.interrupted();
        if (!interrupted) {
            return;
        }
        OtherProcess other(fixture);
        std::optional<std::expected<va::VerifyOutcome, va::VerifyStop>> first;
        HookedMedia media(fixture.media, [&] {
            if (!first_finishes) {
                fixture.archive.node("g.bin").open_error =
                    va::IoError{va::IoErrorKind::DeviceRemoved, 0, "unplugged"};
            }
            first = other.add(fixture, fixture.archive, interrupted->run_id);
            fixture.archive.node("g.bin").open_error.reset();
        });
        va::VerifyServices services{fixture.archive, *fixture.repository.files, media, fixture.store,
            fixture.repository.context.repo, fixture.repository.context.repository_uuid, fixture.host,
            *fixture.repository.writer, fixture.processes};
        va::VerifyRequest request;
        request.archive_uuid = fixture.archive_uuid;
        request.resume_run = interrupted->run_id;
        const auto second = va::verify_full(services, request, fixture.cancel, fixture.progress);
        CHECK(first && *first && (**first).result == (first_finishes ? "PASSED" : "INCOMPLETE"));
        if (!first || !*first) {
            continue;
        }
        if (first_finishes) {
            // The archive is at revision 2 by then (R-104).
            CHECK(!second && second.error().kind == va::VerifyStop::Kind::Decision &&
                  second.error().message.starts_with("run " + interrupted->run_id + " cannot be resumed"));
        } else {
            CHECK(!second && refused_by(second.error(), fixture.archive_uuid, (**first).run_id));
        }
    }
}

// A verification that adds chunk hashes to an earlier revision than the
// archive's can never finish (R-104): it is not unfinished, refuses nothing,
// and the next verification that adds them removes its record and saved
// progress (here left by an import of revision 2, sample data written by
// hand: the archive's record set to revision 2).
void a_verification_of_an_earlier_revision_is_not_unfinished()
{
    Halves fixture;
    const auto interrupted = fixture.interrupted();
    if (!interrupted) {
        return;
    }
    const auto& uuid = fixture.archive_uuid;
    const auto at_revision = [&](int revision) {
        fixture.repository.execute(("UPDATE archives SET current_revision = " + std::to_string(revision) +
                                    " WHERE archive_uuid = '" + uuid + "'")
                                       .c_str());
    };
    at_revision(2);
    const auto adding = va::unfinished_adding_verification(fixture.store, uuid);
    CHECK(adding && !*adding);
    at_revision(1);
    fixture.repository.execute(("UPDATE settings SET value = json_set(value, '$.revision', 0) WHERE key = 'run:" +
                                interrupted->run_id + "'")
                                   .c_str());
    const auto added = fixture.add();
    CHECK(added && added->added_revision == 2);
    CHECK(fixture.store.setting("run:" + interrupted->run_id) == "");
    const auto left = fixture.store.progress(interrupted->run_id);
    CHECK(left && left->empty());
}

} // namespace

void add_chunk_hashes_tests()
{
    adds_the_chunk_hashes_of_an_archive_registered_without_them();
    a_difference_records_the_verification_alone();
    entries_unreadable_since_registration_are_carried();
    refusals();
    the_record_leaves_the_revision_to_another_operation();
    the_record_waits_for_the_write_lock();
    the_record_removes_what_can_no_longer_finish();
    entry_lines_that_cannot_be_written_again_add_nothing();
    an_interrupted_run_resumes_adding();
    a_device_that_goes_away_interrupts_the_run_that_adds_them();
    a_file_without_all_its_kept_chunk_hashes_is_read_again();
    a_run_interrupted_twice_reads_nothing_twice();
    kept_chunk_hashes_in_the_repository();
    what_takes_chunk_hashes();
    the_revision_that_adds_them();
    golden_revision_with_chunk_hashes_added();
    a_registration_waits_for_a_verification_that_adds_chunk_hashes();
    a_verification_that_adds_chunk_hashes_waits_for_a_registration();
    a_relocation_and_a_verification_that_adds_chunk_hashes_wait_for_each_other();
    two_at_once();
    abandoning_a_verification_that_adds_chunk_hashes();
    a_verification_that_adds_chunk_hashes_is_not_abandoned_while_it_runs();
    a_verification_of_an_earlier_revision_is_not_unfinished();
    a_run_abandoned_while_it_reads_saves_no_checkpoint();
    a_run_that_cannot_record_itself_says_that_it_stopped();
    what_resume_continues();
    which_run_resume_continues();
    what_the_refusal_in_the_window_says();
    the_window_judges_the_operation_that_refuses_it();
    what_the_command_line_gives();
    what_can_no_longer_finish_refuses_nothing();
    two_resumes_that_found_the_run_at_once();
}
