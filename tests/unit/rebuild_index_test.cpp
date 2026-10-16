#include "test_support.h"

#include "engine_fixture.h"

#include "core/engine/archive_status.h"
#include "core/engine/rebuild_index.h"
#include "core/engine/relocate_engine.h"
#include "core/hash/hashers.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iterator>
#include <optional>
#include <set>

namespace {

std::string read_file(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

bool contains(const std::vector<std::string>& lines, const std::string& line)
{
    return std::find(lines.begin(), lines.end(), line) != lines.end();
}

// R-80: the index can be rebuilt from manifests alone; history is untouched.
void rebuild_keeps_database_facts()
{
    test::EngineFixture fixture;
    fixture.archive.add_file("one/a.txt", "a");
    fixture.archive.add_file("two/b.txt", "b");
    const auto first = fixture.register_root(fixture.archive.root() / "one");
    const auto second = fixture.register_root(fixture.archive.root() / "two");
    CHECK(first && second);
    if (!first || !second) {
        return;
    }
    CHECK(fixture.verify(fixture.archive.root() / "one").has_value());

    auto archive = fixture.store.find_archive(second->archive_uuid);
    if (archive && *archive) {
        (*archive)->status = "RETIRED";
        CHECK(fixture.store.update_archive(**archive));
    }
    fixture.repository.execute("DELETE FROM files; DELETE FROM archive_revisions;");

    const auto report = va::rebuild_index(fixture.store, *fixture.repository.files, fixture.repository.context.repo);
    CHECK(report && report->archives == 2 && report->revisions == 2 && report->skipped.empty());
    const auto revisions = fixture.store.revisions(first->archive_uuid);
    CHECK(revisions && revisions->size() == 1);
    const auto retired = fixture.store.find_archive(second->archive_uuid);
    CHECK(retired && *retired && (*retired)->status == "RETIRED"); // database-only facts are kept

    // Verification works against the rebuilt index, and history survived.
    const auto verified = fixture.verify(fixture.archive.root() / "one");
    CHECK(verified && verified->result == "PASSED");
    const auto runs = fixture.store.verify_runs(first->archive_uuid);
    CHECK(runs && runs->size() == 2);

    // A damaged manifest is reported, not indexed: its revision keeps the
    // record it had (decision 0028).
    const auto path = fixture.repository.context.repo.root / (*revisions)[0].manifest_relpath;
    std::string bytes;
    {
        std::ifstream input(path, std::ios::binary);
        bytes.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    }
    bytes[bytes.size() - 3] ^= 0x01;
    std::ofstream(path, std::ios::binary | std::ios::trunc) << bytes;
    const auto damaged = va::rebuild_index(fixture.store, *fixture.repository.files, fixture.repository.context.repo);
    CHECK(damaged && damaged->skipped.size() == 1 && damaged->revisions == 1 && damaged->revisions_kept == 1);
    if (damaged && damaged->skipped.size() == 1) {
        CHECK(damaged->skipped.front() == "cannot read manifest " + first->archive_uuid +
            "/r0001.vam: manifest body does not match its header (ManifestHashMismatch)");
    }
    const auto still = fixture.store.revisions(first->archive_uuid);
    CHECK(still && still->size() == 1 && still->front().header_blake3 == (*revisions)[0].header_blake3);
}

// Each skipped line starts with its own words and ends with the cause after
// ": ", so the interface language catalog can match it (decision 0017).
void skipped_manifests_say_why()
{
    test::EngineFixture fixture;
    fixture.archive.add_file("a.txt", "a");
    const auto registered = fixture.register_root(fixture.archive.root());
    CHECK(registered.has_value());
    if (!registered) {
        return;
    }
    const auto& layout = fixture.repository.context.repo;
    const auto revisions = fixture.store.revisions(registered->archive_uuid);
    CHECK(revisions && revisions->size() == 1);
    if (!revisions || revisions->size() != 1) {
        return;
    }
    const auto manifest = read_file(layout.root / revisions->front().manifest_relpath);

    // The same manifest with one entry type changed and the body hash in the
    // header fixed up, so the body passes and only the entry is wrong.
    const auto newline = manifest.find('\n');
    auto header = manifest.substr(0, newline + 1);
    auto body = manifest.substr(newline + 1);
    const auto body_hash = header.find(va::blake3_hex(body));
    const auto entry_type = body.find("\"t\":\"f\"");
    CHECK(body_hash != std::string::npos && entry_type != std::string::npos);
    if (body_hash == std::string::npos || entry_type == std::string::npos) {
        return;
    }
    body[entry_type + 5] = 'x';
    header.replace(body_hash, 64, va::blake3_hex(body));

    // manifests/ as rebuild-index sees it, with one fault per manifest.
    const std::string unlisted = "aaaaaaaa-0000-4000-8000-000000000001";
    const std::string broken = "bbbbbbbb-0000-4000-8000-000000000002";
    const auto& own = registered->archive_uuid; // the bad entry needs a matching header
    test::FakeFileSystem manifests(layout.manifests());
    manifests.add_directory(unlisted).list_error = va::IoError{va::IoErrorKind::PermissionDenied, 5, "denied"};
    manifests.add_file(broken + "/r0001.vam", "");
    manifests.add_file(broken + "/r0002.vam", "{\"format\":\"vam\",\"version\":2}\n");
    manifests.add_file(broken + "/r0003.vam", manifest); // another archive's manifest
    manifests.add_file(own + "/r0001.vam", header + body);

    const auto report = va::rebuild_index(fixture.store, manifests, layout);
    CHECK(report && report->skipped.size() == 5 && report->revisions == 0 && report->revisions_kept == 1);
    if (!report) {
        return;
    }
    const auto& skipped = report->skipped;
    CHECK(contains(skipped, "cannot list the manifests of archive " + unlisted + ": denied"));
    CHECK(contains(skipped, "cannot read manifest " + broken + "/r0001.vam: empty manifest"));
    // No "[json.exception.other_error.501] " tag from the JSON library.
    CHECK(contains(skipped,
        "cannot read manifest " + broken + "/r0002.vam: invalid manifest header: not a version 1 .vam manifest"));
    CHECK(contains(skipped,
        "cannot read manifest " + broken + "/r0003.vam: header does not match its file name"));
    // Not indexed with only the entries before the bad one.
    CHECK(contains(skipped,
        "cannot read manifest " + own + "/r0001.vam: invalid manifest entry: unknown entry type x"));
}

// Decision 0026: rebuild-index rebuilds only what the manifests define, and
// keeps the saved progress of the unfinished operations whose record exists:
// an interrupted registration's rows under the revision it creates, and a
// relocation's checkpoints, which are not in the index at all. Both then
// resume reading only the rest. Rows that no manifest and no plan names go,
// and those of a revision a manifest defines are its index, also where a plan
// of that revision was left behind (sample data on a fake volume).
void rebuild_keeps_saved_progress()
{
    test::EngineFixture fixture;
    auto second = test::fake_volume(fixture.archive.root() / "new");
    second.filesystem_uuid = "5555666677778888";
    second.filesystem_uuid_short = "77778888";
    second.devices.front().serial = "NEW-DISK";
    fixture.media.extra_volumes.push_back(second);

    // An archive at /vol/old, relocated to a new disk that went away after
    // one of its two files had matched.
    fixture.archive.add_file("old/a.txt", std::string(100, 'a'));
    fixture.archive.add_file("old/b.txt", std::string(200, 'b'));
    const auto moved = fixture.register_root(fixture.archive.root() / "old");
    CHECK(moved.has_value());
    if (!moved) {
        return;
    }
    const auto& moved_uuid = moved->archive_uuid;
    fixture.archive.add_file("new/a.txt", std::string(100, 'a'));
    fixture.archive.add_file("new/b.txt", std::string(200, 'b')).open_error =
        va::IoError{va::IoErrorKind::DeviceRemoved, 0, "unplugged"};
    va::RelocateServices services{fixture.archive, *fixture.repository.files, *fixture.repository.writer,
        fixture.media, fixture.store, fixture.repository.context.repo, fixture.repository.context.repository_uuid,
        fixture.host, fixture.processes};
    const auto stopped = va::relocate_archive(services, {moved_uuid, fixture.archive.root() / "new", std::nullopt},
        fixture.cancel, fixture.progress);
    CHECK(!stopped && stopped.error().kind == va::RelocateFailure::Kind::Incomplete);
    fixture.archive.node("new/b.txt").open_error.reset();
    const auto record =
        nlohmann::json::parse(fixture.store.setting(va::relocation_key(moved_uuid)).value_or(""), nullptr, false);
    const auto run_id = record.is_object() ? record.value("run_id", std::string{}) : std::string{};
    CHECK(fixture.store.progress(run_id).value_or(std::set<std::string>{}).size() == 1);

    // A first registration at /vol/broken, interrupted after it saved two of
    // its three files.
    fixture.archive.add_file("broken/f0.bin", std::string(100, 'x'));
    fixture.archive.add_file("broken/f1.bin", std::string(100, 'y'));
    fixture.archive.add_file("broken/z.bin", std::string(100, 'z')).open_error =
        va::IoError{va::IoErrorKind::DeviceRemoved, 0, "unplugged"};
    const auto interrupted = fixture.register_root(fixture.archive.root() / "broken");
    CHECK(!interrupted && interrupted.error().kind == va::RegisterFailure::Kind::Incomplete);
    fixture.archive.node("broken/z.bin").open_error.reset();
    va::Uuid broken_uuid;
    if (const auto archives = fixture.store.archives()) {
        for (const auto& archive : *archives) {
            if (archive.status == "REGISTRATION_INCOMPLETE") {
                broken_uuid = archive.archive_uuid;
            }
        }
    }
    const auto saved = fixture.store.file_row_count(broken_uuid, 1);
    CHECK(saved && saved->files == 2 && saved->bytes == 200);

    // Rows that no manifest and no plan names, a row too many under revision
    // 1 of the relocated archive, and a plan of that revision that an older
    // version left behind although it is recorded (decision 0023).
    const std::string stray = "aaaaaaaa-0000-4000-8000-00000000dead";
    fixture.repository.execute(("INSERT INTO files(archive_uuid, revision, path, raw_hex, stream, type, size) "
                                "VALUES('" + stray + "', 7, 'stray.bin', '', '', 'f', 5)")
                                   .c_str());
    fixture.repository.execute(("INSERT INTO files(archive_uuid, revision, path, raw_hex, stream, type, size) "
                                "VALUES('" + moved_uuid + "', 1, 'ghost.bin', '', '', 'f', 5)")
                                   .c_str());
    const nlohmann::json left_plan{{"archive_uuid", moved_uuid}, {"revision", 1}, {"label", "ARCH-0001"},
        {"created", "2026-10-16T01:00:00Z"}, {"new_archive", true}, {"root", "Z:\\left behind"}};
    CHECK(fixture.store.set_setting("registration:" + moved_uuid, left_plan.dump()));

    const auto report = va::rebuild_index(fixture.store, *fixture.repository.files, fixture.repository.context.repo);
    CHECK(report && report->revisions == 1 && report->skipped.empty() && report->saved_progress_kept == 1);
    const auto kept = fixture.store.file_row_count(broken_uuid, 1);
    CHECK(kept && kept->files == 2 && kept->bytes == 200);
    CHECK(!fixture.store.has_file_rows(stray, 7).value_or(true));
    const auto index = fixture.store.file_rows(moved_uuid, 1);
    CHECK(index && index->size() == 2 && std::ranges::none_of(*index, [](const va::Entry& row) {
        return row.path.p == "ghost.bin";
    }));
    CHECK(fixture.store.progress(run_id).value_or(std::set<std::string>{}).size() == 1);
    CHECK(fixture.store.remove_setting("registration:" + moved_uuid));

    // Both resume reading only the rest.
    fixture.archive.bytes_read = 0;
    va::RegisterRequest resume;
    resume.root = fixture.archive.root() / "broken";
    resume.resume = true;
    const auto registered = fixture.register_root(resume);
    CHECK(registered && registered->revision == 1 && fixture.archive.bytes_read == 100);
    fixture.archive.bytes_read = 0;
    const auto relocated = va::relocate_archive(services,
        {moved_uuid, fixture.archive.root() / "new", std::nullopt, true}, fixture.cancel, fixture.progress);
    CHECK(relocated && relocated->revision == 2 && fixture.archive.bytes_read == 200);
}

std::chrono::sys_seconds now()
{
    return std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
}

// The one archive of `fixture`, as the archive list reads it.
std::optional<va::ArchiveStatus> listed(test::EngineFixture& fixture)
{
    const auto statuses = va::archive_statuses(
        fixture.store, *fixture.repository.files, fixture.repository.context.repo, va::Policy{}, now());
    CHECK(statuses && statuses->size() == 1);
    if (!statuses || statuses->size() != 1) {
        return std::nullopt;
    }
    return statuses->front();
}

// An archive registered twice (sample data): revision 1 with a.txt, revision
// 2 with b.txt as well. Its uuid.
va::Uuid registered_twice(test::EngineFixture& fixture)
{
    fixture.archive.add_file("a.txt", "a (sample data)");
    const auto first = fixture.register_root(fixture.archive.root());
    fixture.archive.add_file("b.txt", "b (sample data)");
    va::RegisterRequest again;
    again.root = fixture.archive.root();
    again.new_revision = true;
    const auto second = fixture.register_root(again);
    CHECK(first && second && second->revision == 2);
    return first ? first->archive_uuid : va::Uuid{};
}

// Decision 0028: a revision whose manifest `repo rebuild-index` cannot read
// keeps its record and its index as they were, and the archive stays at that
// revision, never taken for one at an earlier revision. An archive registered
// twice, whose manifest of revision 1 is intact while that of revision 2 is
// gone, has a first line that no longer parses, or has one byte of its body
// changed (sample data). The rebuild rebuilds revision 1, keeps the record of
// revision 2 with its rows of files, and says so; the archive stays at
// revision 2. It is listed with what is wrong with that manifest (nothing
// where only the body changed: the list reads line 1 alone), FAILS by its
// uuid at revision 2 without reading the media, and stops a verification of
// its root, naming it (where only the body changed, its root finds it and
// fails on the body). Restored, the manifest verifies at revision 2, with no
// other rebuild.
void a_manifest_that_cannot_be_read_keeps_its_revision()
{
    enum class Case { Missing, FirstLine, Body };
    for (const auto what : {Case::Missing, Case::FirstLine, Case::Body}) {
        test::EngineFixture fixture;
        const auto archive_uuid = registered_twice(fixture);
        const auto& layout = fixture.repository.context.repo;
        const auto second = va::manifest_path(layout.manifests(), archive_uuid, 2);
        const auto kept = read_file(second);
        const auto record_before = fixture.store.revision(archive_uuid, 2);
        const auto rows_before = fixture.store.file_row_count(archive_uuid, 2);
        CHECK(record_before && *record_before && rows_before && rows_before->files == 2);
        const auto manifest = "the manifest of revision 2 (manifests/" + archive_uuid + "/r0002.vam)";
        std::string wrong;
        auto bytes = kept;
        if (what == Case::Missing) {
            std::filesystem::remove(second);
            wrong = manifest + " is missing";
        } else if (what == Case::FirstLine) {
            bytes[0] = '[';
            wrong = manifest + " has changed: its first line differs from what the repository recorded";
        } else {
            bytes[bytes.size() - 5] ^= 0x01;
            wrong = manifest + " has changed: its body does not match its header";
        }
        if (what != Case::Missing) {
            std::ofstream(second, std::ios::binary | std::ios::trunc) << bytes;
        }
        const auto message = wrong + "; restore it from a backup of the repository";

        // A plan of revision 2 that an older version left behind, though the
        // revision is recorded (decision 0023): no saved progress, as the
        // rows under revision 2 are that revision's index.
        const nlohmann::json left_plan{{"archive_uuid", archive_uuid}, {"revision", 2}, {"label", "ARCH-0001"},
            {"created", "2026-10-16T01:00:00Z"}, {"new_archive", false}, {"root", "Z:\\left behind"}};
        CHECK(fixture.store.set_setting("registration:" + archive_uuid, left_plan.dump()));

        const auto report = va::rebuild_index(fixture.store, *fixture.repository.files, layout);
        CHECK(report && report->revisions == 1 && report->revisions_kept == 1 && report->saved_progress_kept == 0 &&
              report->skipped.size() == (what == Case::Missing ? 0u : 1u));
        CHECK(fixture.store.remove_setting("registration:" + archive_uuid));
        const auto archive = fixture.store.find_archive(archive_uuid);
        CHECK(archive && *archive && (*archive)->current_revision == 2);
        const auto record = fixture.store.revision(archive_uuid, 2);
        CHECK(record && *record && record_before && *record_before &&
              (*record)->header_blake3 == (*record_before)->header_blake3 &&
              (*record)->body_blake3 == (*record_before)->body_blake3 &&
              (*record)->header_json == (*record_before)->header_json);
        const auto rows = fixture.store.file_row_count(archive_uuid, 2);
        CHECK(rows && rows_before && rows->files == rows_before->files && rows->bytes == rows_before->bytes);

        const auto status = listed(fixture);
        if (what == Case::Body) {
            CHECK(status && status->header && status->header->revision == 2 && !status->unusable_manifest);
        } else {
            CHECK(status && !status->header && status->unusable_manifest &&
                  va::plain_message(*status->unusable_manifest) == message);
        }

        const auto by_root = fixture.verify(fixture.archive.root());
        if (what == Case::Body) {
            CHECK(by_root && by_root->result == "FAILED" && by_root->media_read && by_root->revision == 2 &&
                  by_root->warnings == std::vector<std::string>{"manifest: " + message});
        } else {
            CHECK(!by_root && by_root.error().kind == va::VerifyStop::Kind::Repository &&
                  by_root.error().message.starts_with("this root may belong to archive ARCH-0001 (" + archive_uuid +
                                                      "), whose manifest cannot be used: " + wrong + ";"));
        }
        fixture.media.error = va::IoError{va::IoErrorKind::Other, 0, "the media is not looked for (sample)"};
        va::VerifyRequest by_uuid;
        by_uuid.archive_uuid = archive_uuid;
        const auto named = fixture.verify(by_uuid);
        CHECK(named && named->result == "FAILED" && !named->media_read && named->revision == 2 &&
              named->warnings == std::vector<std::string>{"manifest: " + message});
        fixture.media.error.reset();

        std::ofstream(second, std::ios::binary | std::ios::trunc) << kept;
        const auto restored = fixture.verify(by_uuid);
        CHECK(restored && restored->result == "PASSED" && restored->revision == 2);
        const auto clean = va::rebuild_index(fixture.store, *fixture.repository.files, layout);
        CHECK(clean && clean->revisions == 2 && clean->revisions_kept == 0 && clean->skipped.empty());
    }
}

// Decision 0028: each archive is at the latest revision that a manifest in
// its folder names by its file name, whether rebuild-index could read it or
// not. Here the index has lost the record of revision 2 (sample damage to the
// database) when the rebuild finds its manifest with one byte of its body
// changed: there is no record to keep, and the archive still stays at
// revision 2, of which the index has no record. The archive list says so, and
// that the manifest cannot be read: to restore it, then to rebuild the index.
// Restored, it says to rebuild the index, and the rebuild records revision 2.
// A file that only bears the name of a manifest (a stray r0003.vam, sample
// text) takes the archive to revision 3 the same way; once it is removed,
// the next rebuild takes the archive back to revision 2.
void an_archive_is_at_the_latest_manifest_its_folder_names()
{
    test::EngineFixture fixture;
    const auto archive_uuid = registered_twice(fixture);
    const auto& layout = fixture.repository.context.repo;
    const auto second = va::manifest_path(layout.manifests(), archive_uuid, 2);
    const auto kept = read_file(second);
    auto bytes = kept;
    bytes[bytes.size() - 5] ^= 0x01;
    std::ofstream(second, std::ios::binary | std::ios::trunc) << bytes;
    fixture.repository.execute("DELETE FROM archive_revisions WHERE revision = 2; DELETE FROM files WHERE revision = 2");

    const auto report = va::rebuild_index(fixture.store, *fixture.repository.files, layout);
    CHECK(report && report->revisions == 1 && report->revisions_kept == 0 && report->skipped.size() == 1);
    auto archive = fixture.store.find_archive(archive_uuid);
    CHECK(archive && *archive && (*archive)->current_revision == 2);
    const auto no_record = "the repository's index has no record of the manifest of revision 2 (manifests/" +
                           archive_uuid + "/r0002.vam)";
    auto status = listed(fixture);
    // The list reads line 1 alone: the manifest reads as that of revision 2.
    CHECK(status && status->unusable_manifest &&
          status->unusable_manifest->kind == va::UnusableManifest::Kind::NotIndexed &&
          va::plain_message(*status->unusable_manifest) == no_record + "; rebuild the index with `repo rebuild-index`");
    // Its verification reads it up to its body's hash.
    va::VerifyRequest by_uuid;
    by_uuid.archive_uuid = archive_uuid;
    const auto failed = fixture.verify(by_uuid);
    CHECK(failed && failed->result == "FAILED" && !failed->media_read && failed->revision == 2 &&
          failed->warnings ==
              std::vector<std::string>{"manifest: " + no_record +
                                       ", which cannot be read: manifest body does not match its header "
                                       "(ManifestHashMismatch); restore it from a backup of the repository, then "
                                       "rebuild the index with `repo rebuild-index`"});

    std::ofstream(second, std::ios::binary | std::ios::trunc) << kept;
    const auto rebuilt = va::rebuild_index(fixture.store, *fixture.repository.files, layout);
    CHECK(rebuilt && rebuilt->revisions == 2 && rebuilt->skipped.empty());
    const auto verified = fixture.verify(by_uuid);
    CHECK(verified && verified->result == "PASSED" && verified->revision == 2);

    // A stray file under a manifest's name.
    const auto stray = va::manifest_path(layout.manifests(), archive_uuid, 3);
    std::ofstream(stray, std::ios::binary) << "not a manifest (sample text)\n";
    const auto with_stray = va::rebuild_index(fixture.store, *fixture.repository.files, layout);
    CHECK(with_stray && with_stray->revisions == 2 && with_stray->skipped.size() == 1);
    archive = fixture.store.find_archive(archive_uuid);
    CHECK(archive && *archive && (*archive)->current_revision == 3);
    status = listed(fixture);
    CHECK(status && status->unusable_manifest &&
          status->unusable_manifest->kind == va::UnusableManifest::Kind::NotIndexed &&
          status->unusable_manifest->revision == 3 &&
          status->unusable_manifest->file == va::UnusableManifest::File::Unreadable);
    std::filesystem::remove(stray);
    const auto without_stray = va::rebuild_index(fixture.store, *fixture.repository.files, layout);
    CHECK(without_stray && without_stray->revisions == 2 && without_stray->skipped.empty());
    archive = fixture.store.find_archive(archive_uuid);
    CHECK(archive && *archive && (*archive)->current_revision == 2);

    // A file under the name of revision 0 names none: with both manifests
    // moved away and the index's records of them removed by hand (sample
    // damage), nothing is left of the archive's revisions, and its record
    // keeps revision 2, never taken to revision 0.
    const auto zero = va::manifest_path(layout.manifests(), archive_uuid, 0);
    const auto first = va::manifest_path(layout.manifests(), archive_uuid, 1);
    const auto away = layout.root.parent_path() / "away";
    std::filesystem::create_directories(away);
    std::filesystem::rename(first, away / "r0001.vam");
    std::filesystem::rename(second, away / "r0002.vam");
    fixture.repository.execute("DELETE FROM archive_revisions; DELETE FROM files");
    std::ofstream(zero, std::ios::binary) << "not a manifest (sample text)\n";
    const auto with_zero = va::rebuild_index(fixture.store, *fixture.repository.files, layout);
    CHECK(with_zero && with_zero->revisions == 0 && with_zero->revisions_kept == 0 && with_zero->skipped.size() == 1);
    archive = fixture.store.find_archive(archive_uuid);
    CHECK(archive && *archive && (*archive)->current_revision == 2);
    std::filesystem::remove(zero);
    std::filesystem::rename(away / "r0001.vam", first);
    std::filesystem::rename(away / "r0002.vam", second);
    const auto without = va::rebuild_index(fixture.store, *fixture.repository.files, layout);
    CHECK(without && without->revisions == 2 && without->skipped.empty() && without->revisions_kept == 0);
    archive = fixture.store.find_archive(archive_uuid);
    CHECK(archive && *archive && (*archive)->current_revision == 2);
    status = listed(fixture);
    CHECK(status && status->header && status->header->revision == 2 && !status->unusable_manifest);
}

// Decision 0028: the archive's record keeps the root that the manifest of its
// revision records where rebuild-index cannot read that manifest, never the
// root of an earlier revision. An archive at /vol/old relocated to /vol/new
// on the same volume (sample data), whose manifest of revision 2 is gone:
// the rebuild reads revision 1, at "old", and the archive stays at revision 2
// with its root "new"; restored and rebuilt, it is at "new" as before.
void the_archive_keeps_the_root_of_its_revision()
{
    test::EngineFixture fixture;
    fixture.archive.add_file("old/a.txt", "a (sample data)");
    fixture.archive.add_file("new/a.txt", "a (sample data)");
    const auto registered = fixture.register_root(fixture.archive.root() / "old");
    CHECK(registered.has_value());
    if (!registered) {
        return;
    }
    const auto& archive_uuid = registered->archive_uuid;
    va::RelocateServices services{fixture.archive, *fixture.repository.files, *fixture.repository.writer,
        fixture.media, fixture.store, fixture.repository.context.repo, fixture.repository.context.repository_uuid,
        fixture.host, fixture.processes};
    const auto relocated = va::relocate_archive(
        services, {archive_uuid, fixture.archive.root() / "new", std::nullopt}, fixture.cancel, fixture.progress);
    CHECK(relocated && relocated->revision == 2);
    const auto& layout = fixture.repository.context.repo;
    const auto second = va::manifest_path(layout.manifests(), archive_uuid, 2);
    const auto kept = read_file(second);
    std::filesystem::remove(second);

    const auto report = va::rebuild_index(fixture.store, *fixture.repository.files, layout);
    CHECK(report && report->revisions == 1 && report->revisions_kept == 1);
    auto archive = fixture.store.find_archive(archive_uuid);
    CHECK(archive && *archive && (*archive)->current_revision == 2 && (*archive)->root_subpath == "new");

    std::ofstream(second, std::ios::binary | std::ios::trunc) << kept;
    const auto rebuilt = va::rebuild_index(fixture.store, *fixture.repository.files, layout);
    CHECK(rebuilt && rebuilt->revisions == 2 && rebuilt->revisions_kept == 0);
    archive = fixture.store.find_archive(archive_uuid);
    CHECK(archive && *archive && (*archive)->current_revision == 2 && (*archive)->root_subpath == "new");
}

} // namespace

void rebuild_index_tests()
{
    rebuild_keeps_database_facts();
    skipped_manifests_say_why();
    rebuild_keeps_saved_progress();
    a_manifest_that_cannot_be_read_keeps_its_revision();
    an_archive_is_at_the_latest_manifest_its_folder_names();
    the_archive_keeps_the_root_of_its_revision();
}
