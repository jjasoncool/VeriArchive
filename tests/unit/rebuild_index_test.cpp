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
    CHECK(damaged && damaged->skipped.size() == 1 && damaged->revisions == 1 && damaged->revisions_kept.size() == 1 &&
          damaged->revisions_left_out.empty() && damaged->revisions_not_indexed.empty());
    if (damaged && damaged->revisions_kept.size() == 1) {
        const auto& kept = damaged->revisions_kept.front();
        CHECK(kept.archive_uuid == first->archive_uuid && kept.label == "ARCH-0001" && kept.revision == 1 &&
              kept.manifest == "manifests/" + first->archive_uuid + "/r0001.vam" &&
              kept.cause == "manifest body does not match its header (ManifestHashMismatch)");
    }
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
    CHECK(report && report->skipped.size() == 5 && report->revisions == 0 && report->revisions_kept.size() == 1);
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
// revision 2. It is listed with what is wrong with that manifest (where only
// the body changed, the rebuild that skipped it marked the archive, and the
// list reads the manifest whole), FAILS by its uuid at revision 2 without
// reading the media, and stops a verification of its root, naming it (where
// only the body changed, its root finds it and fails on the body). Restored,
// the manifest verifies at revision 2, with no other rebuild, and is listed
// without a problem while the mark stays; the next rebuild removes the mark.
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
        const auto message = wrong + test::put_back(archive_uuid, 2);

        // A plan of revision 2 that an older version left behind, though the
        // revision is recorded (decision 0023): no saved progress, as the
        // rows under revision 2 are that revision's index.
        const nlohmann::json left_plan{{"archive_uuid", archive_uuid}, {"revision", 2}, {"label", "ARCH-0001"},
            {"created", "2026-10-16T01:00:00Z"}, {"new_archive", false}, {"root", "Z:\\left behind"}};
        CHECK(fixture.store.set_setting("registration:" + archive_uuid, left_plan.dump()));

        const auto report = va::rebuild_index(fixture.store, *fixture.repository.files, layout);
        CHECK(report && report->revisions == 1 && report->revisions_kept.size() == 1 &&
              report->revisions_left_out.empty() && report->revisions_not_indexed.empty() &&
              report->saved_progress_kept == 0 && report->skipped.size() == (what == Case::Missing ? 0u : 1u));
        if (report && report->revisions_kept.size() == 1) {
            const auto& kept_revision = report->revisions_kept.front();
            CHECK(kept_revision.archive_uuid == archive_uuid && kept_revision.revision == 2 &&
                  kept_revision.manifest == "manifests/" + archive_uuid + "/r0002.vam" &&
                  kept_revision.cause.has_value() == (what != Case::Missing));
        }
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

        // A manifest that the rebuild skipped, not one that is missing, marks
        // the archive, and the list reads it whole, as the rebuild did.
        const auto marks = fixture.store.settings_with_prefix(va::skipped_manifest_prefix);
        CHECK(marks && marks->size() == (what == Case::Missing ? 0u : 1u));
        const auto status = listed(fixture);
        CHECK(status && !status->header && status->unusable_manifest &&
              va::plain_message(*status->unusable_manifest) == message);

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
        const auto after = listed(fixture);
        CHECK(after && after->header && !after->unusable_manifest);
        const auto clean = va::rebuild_index(fixture.store, *fixture.repository.files, layout);
        CHECK(clean && clean->revisions == 2 && clean->revisions_kept.empty() && clean->skipped.empty());
        const auto no_marks = fixture.store.settings_with_prefix(va::skipped_manifest_prefix);
        CHECK(no_marks && no_marks->empty());
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
    CHECK(report && report->revisions == 1 && report->revisions_kept.empty() && report->skipped.size() == 1 &&
          report->revisions_not_indexed.size() == 1);
    if (report && report->revisions_not_indexed.size() == 1) {
        const auto& not_indexed = report->revisions_not_indexed.front();
        CHECK(not_indexed.archive_uuid == archive_uuid && not_indexed.revision == 2 &&
              not_indexed.manifest == "manifests/" + archive_uuid + "/r0002.vam" &&
              not_indexed.cause == "manifest body does not match its header (ManifestHashMismatch)");
    }
    auto archive = fixture.store.find_archive(archive_uuid);
    CHECK(archive && *archive && (*archive)->current_revision == 2);
    const auto no_record = "the repository's index has no record of the manifest of revision 2 (manifests/" +
                           archive_uuid + "/r0002.vam)";
    const auto cannot_be_read = no_record +
                                ", which cannot be read: manifest body does not match its header "
                                "(ManifestHashMismatch)" +
                                test::put_back(archive_uuid, 2);
    // The rebuild skipped the manifest and marked the archive: the list reads
    // it whole, as the rebuild did and as its verification does.
    auto status = listed(fixture);
    CHECK(status && status->unusable_manifest &&
          status->unusable_manifest->kind == va::UnusableManifest::Kind::NotIndexed &&
          va::plain_message(*status->unusable_manifest) == cannot_be_read);
    va::VerifyRequest by_uuid;
    by_uuid.archive_uuid = archive_uuid;
    const auto failed = fixture.verify(by_uuid);
    CHECK(failed && failed->result == "FAILED" && !failed->media_read && failed->revision == 2 &&
          failed->warnings == std::vector<std::string>{"manifest: " + cannot_be_read});

    std::ofstream(second, std::ios::binary | std::ios::trunc) << kept;
    // Restored, it reads as the manifest of revision 2: to rebuild the index.
    status = listed(fixture);
    CHECK(status && status->unusable_manifest &&
          va::plain_message(*status->unusable_manifest) == no_record + "; rebuild the index with `repo rebuild-index`");
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
    CHECK(with_zero && with_zero->revisions == 0 && with_zero->revisions_kept.empty() &&
          with_zero->revisions_not_indexed.empty() && with_zero->skipped.size() == 1);
    archive = fixture.store.find_archive(archive_uuid);
    CHECK(archive && *archive && (*archive)->current_revision == 2);
    std::filesystem::remove(zero);
    std::filesystem::rename(away / "r0001.vam", first);
    std::filesystem::rename(away / "r0002.vam", second);
    const auto without = va::rebuild_index(fixture.store, *fixture.repository.files, layout);
    CHECK(without && without->revisions == 2 && without->skipped.empty() && without->revisions_kept.empty());
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
    CHECK(report && report->revisions == 1 && report->revisions_kept.size() == 1);
    auto archive = fixture.store.find_archive(archive_uuid);
    CHECK(archive && *archive && (*archive)->current_revision == 2 && (*archive)->root_subpath == "new");

    std::ofstream(second, std::ios::binary | std::ios::trunc) << kept;
    const auto rebuilt = va::rebuild_index(fixture.store, *fixture.repository.files, layout);
    CHECK(rebuilt && rebuilt->revisions == 2 && rebuilt->revisions_kept.empty());
    archive = fixture.store.find_archive(archive_uuid);
    CHECK(archive && *archive && (*archive)->current_revision == 2 && (*archive)->root_subpath == "new");
}

// The file names in `directory`.
std::set<std::string> names_in(const std::filesystem::path& directory)
{
    std::set<std::string> names;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(directory, error)) {
        names.insert(va::path_to_utf8(entry.path().filename()));
    }
    return names;
}

// Decision 0028: `import` of an export or a media copy of a recorded
// revision puts its manifest back where the repository's copy is missing,
// cannot be read or is not the one recorded, as restoring it from a backup of
// the repository does, so that the record that repo rebuild-index kept is
// what the manifest is checked against. An archive registered twice (sample
// data), exported at revision 2, whose manifest of revision 2 is then gone
// and the index rebuilt: the export imported restores it (the record and its
// index unchanged), the archive lists no problem and verifies at revision 2,
// and the same export imported again is already present. Gone again, a copy
// of it as the media holds one (sample) restores it without a rebuild; with
// one byte of its body changed, the export restores it as well. A file that
// is not the one recorded (its header line written otherwise, sample data)
// is refused (ManifestHashMismatch) and changes nothing. Where the record
// changes while the file is copied (sample damage: it is removed), nothing is
// restored (refused, exit 12); imported again, the file records the revision
// as any import does.
void an_import_restores_the_manifest_recorded()
{
    test::EngineFixture fixture;
    const auto archive_uuid = registered_twice(fixture);
    const auto& layout = fixture.repository.context.repo;
    auto& files = *fixture.repository.files;
    auto& writer = *fixture.repository.writer;
    const auto second = va::manifest_path(layout.manifests(), archive_uuid, 2);
    const auto kept = read_file(second);
    const auto exported =
        va::export_archive(fixture.store, files, writer, layout, archive_uuid, layout.root.parent_path() / "export");
    CHECK(exported.has_value());
    if (!exported) {
        return;
    }
    const auto media_copy = layout.root.parent_path() / "SAMPLE media" / ".veriarchive" / "r0002.vam";
    std::filesystem::create_directories(media_copy.parent_path());
    std::ofstream(media_copy, std::ios::binary) << kept;
    const auto record_before = fixture.store.revision(archive_uuid, 2);
    const auto rows_before = fixture.store.file_row_count(archive_uuid, 2);
    CHECK(record_before && *record_before && rows_before && rows_before->files == 2);
    const auto import = [&](const std::filesystem::path& source) {
        return va::import_manifest(fixture.store, files, writer, layout, source, false);
    };
    const auto restored_as_recorded = [&](const char* what) {
        CHECK(read_file(second) == kept);
        CHECK((names_in(second.parent_path()) == std::set<std::string>{"r0001.vam", "r0002.vam"}));
        const auto record = fixture.store.revision(archive_uuid, 2);
        CHECK(record && *record && record_before && *record_before &&
              (*record)->header_blake3 == (*record_before)->header_blake3 &&
              (*record)->header_json == (*record_before)->header_json &&
              (*record)->imported_from == (*record_before)->imported_from);
        const auto rows = fixture.store.file_row_count(archive_uuid, 2);
        CHECK(rows && rows_before && rows->files == rows_before->files);
        const auto status = listed(fixture);
        CHECK(status && status->header && !status->unusable_manifest);
        va::VerifyRequest by_uuid;
        by_uuid.archive_uuid = archive_uuid;
        const auto verified = fixture.verify(by_uuid);
        const bool passed = verified && verified->result == "PASSED" && verified->revision == 2;
        CHECK(passed);
        if (!passed) {
            std::cerr << "  verified after a restore from " << what << ": "
                      << (verified ? verified->result : verified.error().message) << '\n';
        }
    };

    std::filesystem::remove(second);
    const auto report = va::rebuild_index(fixture.store, files, layout);
    CHECK(report && report->revisions_kept.size() == 1);
    const auto from_export = import(exported->manifest);
    CHECK(from_export && from_export->restored && !from_export->already_present && !from_export->new_archive &&
          from_export->revision == 2);
    if (!from_export) {
        std::cerr << "  import of the export: " << from_export.error().message << '\n';
    }
    restored_as_recorded("the export");
    const auto again = import(exported->manifest);
    CHECK(again && again->already_present && !again->restored);

    std::filesystem::remove(second);
    const auto from_media = import(media_copy);
    CHECK(from_media && from_media->restored);
    restored_as_recorded("the media copy");

    auto changed = kept;
    changed[changed.size() - 5] ^= 0x01;
    std::ofstream(second, std::ios::binary | std::ios::trunc) << changed;
    const auto over_changed = import(exported->manifest);
    CHECK(over_changed && over_changed->restored);
    restored_as_recorded("the export, over a changed manifest");

    // Not the one recorded: refused, whether the repository's copy is gone
    // or changed, and nothing is written.
    auto other = kept;
    const auto host = other.find("\"host\":\"");
    CHECK(host != std::string::npos && host < other.find('\n'));
    other.insert(host + 8, "another-");
    const auto other_path = layout.root.parent_path() / "another export" / "r0002.vam";
    std::filesystem::create_directories(other_path.parent_path());
    std::ofstream(other_path, std::ios::binary) << other;
    for (const bool gone : {true, false}) {
        if (gone) {
            std::filesystem::remove(second);
        } else {
            std::ofstream(second, std::ios::binary | std::ios::trunc) << changed;
        }
        const auto refused = import(other_path);
        CHECK(!refused && refused.error().kind == va::ImportFailure::Kind::Failed &&
              refused.error().failure_class == "ManifestHashMismatch" &&
              refused.error().message == "revision 2 is already known with different bytes");
        CHECK(gone ? !std::filesystem::exists(second) : read_file(second) == changed);
        CHECK((names_in(second.parent_path()) ==
               (gone ? std::set<std::string>{"r0001.vam"} : std::set<std::string>{"r0001.vam", "r0002.vam"})));
    }

    // The record changes while the file is copied.
    std::filesystem::remove(second);
    test::StagingHookWriter removes_the_record(writer, [&] {
        fixture.repository.execute("DELETE FROM archive_revisions WHERE revision = 2");
    });
    const auto lost =
        va::import_manifest(fixture.store, files, removes_the_record, layout, exported->manifest, false);
    CHECK(!lost && lost.error().kind == va::ImportFailure::Kind::Refused &&
          lost.error().message == "the repository's record of revision 2 of archive " + archive_uuid +
                                      " changed while this import ran; nothing was imported (import it again)");
    CHECK(names_in(second.parent_path()) == std::set<std::string>{"r0001.vam"});
    const auto recorded = import(exported->manifest);
    CHECK(recorded && !recorded->restored && !recorded->already_present && recorded->revision == 2);
    CHECK(read_file(second) == kept);

    // Where only the index differs from the manifest (its copy of the header,
    // sample damage), the file is still the one recorded, its line 1 having
    // the BLAKE3 recorded for it: it is put back, the list then says to
    // rebuild the index, and the rebuild repairs it.
    std::filesystem::remove(second);
    fixture.repository.execute(
        "UPDATE archive_revisions SET header_json = 'not the header (sample damage)' WHERE revision = 2");
    const auto despite_the_index = import(exported->manifest);
    CHECK(despite_the_index && despite_the_index->restored);
    CHECK(read_file(second) == kept);
    auto status = listed(fixture);
    CHECK(status && status->unusable_manifest &&
          status->unusable_manifest->kind == va::UnusableManifest::Kind::IndexDiffers);
    const auto repaired = va::rebuild_index(fixture.store, files, layout);
    CHECK(repaired && repaired->skipped.empty() && repaired->revisions == 2);
    status = listed(fixture);
    CHECK(status && status->header && !status->unusable_manifest);
}

// Decision 0028: the record of an earlier revision whose manifest
// repo rebuild-index cannot read goes with its index, while the archive's
// own revision keeps its record. An archive at /vol/old relocated to a
// second volume (sample data): revision 1 at "old" on the first volume,
// revision 2 at the root of the second. With the manifest of revision 1
// gone, a verification of /vol/old stops, as matching reads the earlier
// carriers there and that manifest cannot be used, and `archive show` (the
// earlier revisions read) says so. The rebuild names revision 1, left out
// of the index; the archive stays at revision 2, /vol/old is no archive's
// any more (UNKNOWN) and the second volume verifies. Put back, the manifest
// is indexed again by the next rebuild, and /vol/old is the earlier carrier
// (PREVIOUS_CARRIER). With the manifest of revision 2 gone instead, the
// rebuild keeps its record, and once it is put back /vol/old is the earlier
// carrier as well.
void an_earlier_revision_whose_manifest_is_gone_is_left_out()
{
    test::EngineFixture fixture;
    auto second_volume = test::fake_volume(fixture.archive.root() / "new");
    second_volume.filesystem_uuid = "5555666677778888";
    second_volume.filesystem_uuid_short = "77778888";
    second_volume.devices.front().serial = "SAMPLE-NEW-DISK";
    fixture.media.extra_volumes.push_back(second_volume);
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
    const auto first = va::manifest_path(layout.manifests(), archive_uuid, 1);
    const auto second = va::manifest_path(layout.manifests(), archive_uuid, 2);
    const auto first_bytes = read_file(first);
    const auto second_bytes = read_file(second);
    const auto first_place = "manifests/" + archive_uuid + "/r0001.vam";
    // An earlier revision's: put back, with no last resort, as the archive
    // has a later revision already (decision 0028).
    const auto first_missing =
        "the manifest of revision 1 (" + first_place + ") is missing" + test::put_back(archive_uuid, 1, false);
    // The archive as `archive show` reads it, with its earlier revisions.
    const auto shown = [&]() -> std::optional<va::ArchiveStatus> {
        const auto statuses = va::archive_statuses(fixture.store, *fixture.repository.files, layout, va::Policy{},
            now(), va::StatusSelection{archive_uuid, true, true});
        CHECK(statuses && statuses->size() == 1);
        if (!statuses || statuses->size() != 1) {
            return std::nullopt;
        }
        return statuses->front();
    };
    const auto old_root = fixture.archive.root() / "old";

    std::filesystem::remove(first);
    auto status = shown();
    CHECK(status && !status->unusable_manifest && status->earlier_unusable.size() == 1 &&
          va::plain_message(status->earlier_unusable.front()) == first_missing);
    auto by_old_root = fixture.verify(old_root);
    CHECK(!by_old_root && by_old_root.error().kind == va::VerifyStop::Kind::Repository &&
          by_old_root.error().message == first_missing);

    const auto report = va::rebuild_index(fixture.store, *fixture.repository.files, layout);
    CHECK(report && report->revisions == 1 && report->revisions_kept.empty() &&
          report->revisions_not_indexed.empty() && report->revisions_left_out.size() == 1 && report->skipped.empty());
    if (report && report->revisions_left_out.size() == 1) {
        const auto& left_out = report->revisions_left_out.front();
        CHECK(left_out.archive_uuid == archive_uuid && left_out.label == "ARCH-0001" && left_out.revision == 1 &&
              left_out.manifest == first_place && !left_out.cause);
    }
    const auto first_record = fixture.store.revision(archive_uuid, 1);
    CHECK(first_record && !*first_record && !fixture.store.has_file_rows(archive_uuid, 1).value_or(true));
    const auto archive = fixture.store.find_archive(archive_uuid);
    CHECK(archive && *archive && (*archive)->current_revision == 2);
    status = shown();
    CHECK(status && !status->unusable_manifest && status->earlier_unusable.empty());
    by_old_root = fixture.verify(old_root);
    CHECK(!by_old_root && by_old_root.error().kind == va::VerifyStop::Kind::Decision &&
          by_old_root.error().identity.result == va::MatchResult::Unknown);
    if (by_old_root) {
        std::cerr << "  /vol/old after the rebuild: " << by_old_root->result << '\n';
    } else if (by_old_root.error().kind != va::VerifyStop::Kind::Decision) {
        std::cerr << "  /vol/old after the rebuild: " << by_old_root.error().message << '\n';
    }
    const auto by_new_root = fixture.verify(fixture.archive.root() / "new");
    CHECK(by_new_root && by_new_root->result == "PASSED" && by_new_root->revision == 2);

    // Put back, revision 1 is indexed again by the next rebuild.
    std::ofstream(first, std::ios::binary | std::ios::trunc) << first_bytes;
    const auto again = va::rebuild_index(fixture.store, *fixture.repository.files, layout);
    CHECK(again && again->revisions == 2 && again->revisions_left_out.empty() && again->revisions_kept.empty());
    by_old_root = fixture.verify(old_root);
    CHECK(!by_old_root && by_old_root.error().identity.result == va::MatchResult::PreviousCarrier);

    // The archive's own revision keeps its record; put back, its earlier
    // carrier is known as before.
    std::filesystem::remove(second);
    const auto kept = va::rebuild_index(fixture.store, *fixture.repository.files, layout);
    CHECK(kept && kept->revisions == 1 && kept->revisions_kept.size() == 1 && kept->revisions_left_out.empty());
    std::ofstream(second, std::ios::binary | std::ios::trunc) << second_bytes;
    by_old_root = fixture.verify(old_root);
    CHECK(!by_old_root && by_old_root.error().identity.result == va::MatchResult::PreviousCarrier);
}

// Decision 0028, point 2: the message of an earlier revision's manifest that
// cannot be used names the way out where nothing holds it, `repo
// rebuild-index`, which leaves that revision out of the index (point 6). An
// archive at /vol/old relocated to the root of a second volume (sample data),
// whose manifest of revision 1 is gone and held nowhere: a registration of a
// new folder of the first volume reads the earlier carriers and stops with
// that message, and a registration over a lost manifest is refused, as the
// manifest of the archive's revision 2 can be used. Once the rebuild has left
// revision 1 out, the same registration goes ahead.
void an_earlier_revision_whose_manifest_is_lost_is_left_out_by_the_rebuild_it_names()
{
    test::EngineFixture fixture;
    auto second_volume = test::fake_volume(fixture.archive.root() / "new");
    second_volume.filesystem_uuid = "5555666677778888";
    second_volume.filesystem_uuid_short = "77778888";
    second_volume.devices.front().serial = "SAMPLE-NEW-DISK";
    fixture.media.extra_volumes.push_back(second_volume);
    fixture.archive.add_file("old/a.txt", "a (sample data)");
    fixture.archive.add_file("new/a.txt", "a (sample data)");
    fixture.archive.add_file("other/SAMPLE-other.txt", "other (sample data)");
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
    std::filesystem::remove(va::manifest_path(layout.manifests(), archive_uuid, 1));
    const auto first_lost = "the manifest of revision 1 (manifests/" + archive_uuid + "/r0001.vam) is missing" +
                            test::put_back(archive_uuid, 1, false);
    CHECK(first_lost.ends_with("; if none of these holds it, leave revision 1 out of the index with `repo "
                               "rebuild-index`"));

    const auto stopped = fixture.register_root(fixture.archive.root() / "other");
    CHECK(!stopped && stopped.error().kind == va::RegisterFailure::Kind::Repository &&
          stopped.error().message == first_lost);
    va::RegisterRequest over_it;
    over_it.root = fixture.archive.root() / "new";
    over_it.lost_manifest_of = archive_uuid;
    const auto refused = fixture.register_root(over_it);
    CHECK(!refused && refused.error().kind == va::RegisterFailure::Kind::Refused &&
          refused.error().message == "the manifest of revision 2 of archive " + archive_uuid +
                                         " can be used: register its next revision with `register <root_path> "
                                         "--new-revision`");

    const auto rebuilt = va::rebuild_index(fixture.store, *fixture.repository.files, layout);
    CHECK(rebuilt && rebuilt->revisions_left_out.size() == 1 && rebuilt->revisions_kept.empty());
    const auto other = fixture.register_root(fixture.archive.root() / "other");
    CHECK(other && other->revision == 1 && other->archive_uuid != archive_uuid);
    if (!other) {
        std::cerr << "  the new folder after the rebuild: " << other.error().message << '\n';
    }
}

// Decision 0028: repo rebuild-index never sets an archive below the latest
// revision that a verification of it which read its media names; the
// verification history is no index, so it keeps that revision where an older
// version's rebuild took the archive back for want of its manifest. Sample
// data: an archive registered twice and verified at revision 2, which an
// older rebuild set back to revision 1 (its manifest of revision 2 gone, the
// record and rows of revision 2 dropped, the archive's record at revision 1,
// as that version left them). A run that read no media naming revision 3
// (as one that failed on its manifest records) counts for nothing. This
// version's rebuild puts the archive at revision 2, as one of which the index
// has no record, says so, lists it with the step to restore the manifest and
// then rebuild, and fails it by its uuid at revision 2 without reading the
// media, never verifying it against revision 1; imported from its export, the
// manifest of revision 2 is recorded and verifies. Where nothing else is left
// of the archive's revisions, the history still raises its record to the
// revision it names, and never lowers it.
void the_history_keeps_an_archive_from_an_earlier_revision()
{
    test::EngineFixture fixture;
    const auto archive_uuid = registered_twice(fixture);
    const auto& layout = fixture.repository.context.repo;
    const auto exported = va::export_archive(fixture.store, *fixture.repository.files, *fixture.repository.writer,
        layout, archive_uuid, layout.root.parent_path() / "export");
    CHECK(exported.has_value());
    va::VerifyRequest by_uuid;
    by_uuid.archive_uuid = archive_uuid;
    const auto verified = fixture.verify(by_uuid);
    CHECK(verified && verified->result == "PASSED" && verified->revision == 2 && verified->media_read);
    va::VerifyRunRecord unread;
    unread.run_id = "c0ffee00-0000-4000-8000-000000000003";
    unread.archive_uuid = archive_uuid;
    unread.revision = 3;
    unread.mode = "FULL";
    unread.started = "2026-10-16T02:00:00Z";
    unread.finished = "2026-10-16T02:00:00Z";
    unread.result = "FAILED";
    unread.coverage_bytes = 0;
    unread.counts_json = R"({"items":{},"media_read":false})";
    unread.failure_classes = "RepositoryError";
    unread.host = "sample-host";
    unread.app_version = "0.1.0";
    CHECK(fixture.store.append_verify_run(unread, fixture.repository.context.repository_uuid).has_value());
    if (!exported) {
        return;
    }

    // What an older version's rebuild left.
    const auto second = va::manifest_path(layout.manifests(), archive_uuid, 2);
    std::filesystem::remove(second);
    fixture.repository.execute("DELETE FROM archive_revisions WHERE revision = 2; DELETE FROM files WHERE revision = 2; "
                               "UPDATE archives SET current_revision = 1");

    const auto report = va::rebuild_index(fixture.store, *fixture.repository.files, layout);
    CHECK(report && report->revisions == 1 && report->revisions_kept.empty() && report->skipped.empty() &&
          report->revisions_not_indexed.size() == 1);
    if (report && report->revisions_not_indexed.size() == 1) {
        const auto& not_indexed = report->revisions_not_indexed.front();
        CHECK(not_indexed.archive_uuid == archive_uuid && not_indexed.revision == 2 && !not_indexed.cause &&
              not_indexed.manifest == "manifests/" + archive_uuid + "/r0002.vam");
    }
    auto archive = fixture.store.find_archive(archive_uuid);
    CHECK(archive && *archive && (*archive)->current_revision == 2);
    const auto no_record = "the repository's index has no record of the manifest of revision 2 (manifests/" +
                           archive_uuid + "/r0002.vam), which is missing" + test::put_back(archive_uuid, 2);
    const auto status = listed(fixture);
    CHECK(status && status->unusable_manifest && va::plain_message(*status->unusable_manifest) == no_record);
    fixture.media.error = va::IoError{va::IoErrorKind::Other, 0, "the media is not looked for (sample)"};
    const auto failed = fixture.verify(by_uuid);
    CHECK(failed && failed->result == "FAILED" && !failed->media_read && failed->revision == 2 &&
          failed->warnings == std::vector<std::string>{"manifest: " + no_record});
    fixture.media.error.reset();
    const auto imported = va::import_manifest(fixture.store, *fixture.repository.files, *fixture.repository.writer,
        layout, exported->manifest, false);
    CHECK(imported && imported->revision == 2 && !imported->restored);
    const auto restored = fixture.verify(by_uuid);
    CHECK(restored && restored->result == "PASSED" && restored->revision == 2);

    // Nothing else left of its revisions (the manifests moved away and the
    // records removed by hand, sample damage): the history raises the
    // archive's record from revision 1 to revision 2, and leaves revision 3
    // as it is.
    const auto away = layout.root.parent_path() / "away";
    std::filesystem::create_directories(away);
    const auto first = va::manifest_path(layout.manifests(), archive_uuid, 1);
    std::filesystem::rename(first, away / "r0001.vam");
    std::filesystem::rename(second, away / "r0002.vam");
    fixture.repository.execute(
        "DELETE FROM archive_revisions; DELETE FROM files; UPDATE archives SET current_revision = 1");
    const auto raised = va::rebuild_index(fixture.store, *fixture.repository.files, layout);
    CHECK(raised && raised->revisions_not_indexed.size() == 1);
    archive = fixture.store.find_archive(archive_uuid);
    CHECK(archive && *archive && (*archive)->current_revision == 2);
    fixture.repository.execute("UPDATE archives SET current_revision = 3");
    const auto kept = va::rebuild_index(fixture.store, *fixture.repository.files, layout);
    CHECK(kept && kept->revisions_not_indexed.empty());
    archive = fixture.store.find_archive(archive_uuid);
    CHECK(archive && *archive && (*archive)->current_revision == 3);
    std::filesystem::rename(away / "r0001.vam", first);
    std::filesystem::rename(away / "r0002.vam", second);
    const auto back = va::rebuild_index(fixture.store, *fixture.repository.files, layout);
    CHECK(back && back->revisions == 2 && back->revisions_not_indexed.empty());
    archive = fixture.store.find_archive(archive_uuid);
    CHECK(archive && *archive && (*archive)->current_revision == 2);
}

// `manifest` with the type of its first file entry made "x", and the body's
// BLAKE3 in its header made the one of the body so changed (sample bytes):
// its body matches its header, and an entry does not parse.
std::string with_an_entry_that_does_not_parse(const std::string& manifest)
{
    const auto newline = manifest.find('\n');
    auto header = manifest.substr(0, newline + 1);
    auto body = manifest.substr(newline + 1);
    const auto body_hash = header.find(va::blake3_hex(body));
    const auto entry_type = body.find("\"t\":\"f\"");
    CHECK(body_hash != std::string::npos && entry_type != std::string::npos);
    if (body_hash == std::string::npos || entry_type == std::string::npos) {
        return manifest;
    }
    body[entry_type + 5] = 'x';
    header.replace(body_hash, 64, va::blake3_hex(body));
    return header + body;
}

// Decision 0028: a manifest that the index has no record of is read, where a
// verification reads it whole, as `repo rebuild-index` reads a manifest:
// every entry too. Sample damage: an archive registered twice whose index
// lost the record of revision 2, while the manifest of revision 2 has an
// entry that does not parse though its body matches its header. The rebuild
// skips it, and the archive stays at revision 2, of which the index has no
// record. A verification by its uuid says that the file cannot be read, why,
// and to restore it before rebuilding the index, never only to rebuild the
// index, which skips it again; so does every whole load. Restored and
// rebuilt, the archive verifies at revision 2.
void a_manifest_without_a_record_is_read_to_its_last_entry()
{
    test::EngineFixture fixture;
    const auto archive_uuid = registered_twice(fixture);
    const auto& layout = fixture.repository.context.repo;
    const auto second = va::manifest_path(layout.manifests(), archive_uuid, 2);
    const auto kept = read_file(second);
    std::ofstream(second, std::ios::binary | std::ios::trunc) << with_an_entry_that_does_not_parse(kept);
    fixture.repository.execute("DELETE FROM archive_revisions WHERE revision = 2; DELETE FROM files WHERE revision = 2");

    const auto report = va::rebuild_index(fixture.store, *fixture.repository.files, layout);
    CHECK(report && report->revisions == 1 && report->skipped.size() == 1 && report->revisions_not_indexed.size() == 1);
    if (report && report->skipped.size() == 1) {
        CHECK(report->skipped.front() == "cannot read manifest " + archive_uuid +
                                             "/r0002.vam: invalid manifest entry: unknown entry type x");
    }
    const auto archive = fixture.store.find_archive(archive_uuid);
    CHECK(archive && *archive && (*archive)->current_revision == 2);
    const auto message = "the repository's index has no record of the manifest of revision 2 (manifests/" +
                         archive_uuid +
                         "/r0002.vam), which cannot be read: invalid manifest entry: unknown entry type x" +
                         test::put_back(archive_uuid, 2);
    // The rebuild skipped it and marked the archive: the list reads it whole
    // too, and says the same.
    const auto status = listed(fixture);
    CHECK(status && status->unusable_manifest && va::plain_message(*status->unusable_manifest) == message);
    fixture.media.error = va::IoError{va::IoErrorKind::Other, 0, "the media is not looked for (sample)"};
    va::VerifyRequest by_uuid;
    by_uuid.archive_uuid = archive_uuid;
    const auto failed = fixture.verify(by_uuid);
    CHECK(failed && failed->result == "FAILED" && !failed->media_read && failed->revision == 2 &&
          failed->warnings == std::vector<std::string>{"manifest: " + message});
    if (failed && failed->warnings != std::vector<std::string>{"manifest: " + message} && !failed->warnings.empty()) {
        std::cerr << "  by uuid: " << failed->warnings.front() << '\n';
    }
    fixture.media.error.reset();
    const auto loaded = va::load_recorded_manifest(fixture.store, *fixture.repository.files, layout, archive_uuid, 2);
    CHECK(!loaded && loaded.error().message == message && loaded.error().unusable &&
          loaded.error().unusable->file == va::UnusableManifest::File::Unreadable);

    std::ofstream(second, std::ios::binary | std::ios::trunc) << kept;
    const auto rebuilt = va::rebuild_index(fixture.store, *fixture.repository.files, layout);
    CHECK(rebuilt && rebuilt->revisions == 2 && rebuilt->skipped.empty() && rebuilt->revisions_not_indexed.empty());
    const auto verified = fixture.verify(by_uuid);
    CHECK(verified && verified->result == "PASSED" && verified->revision == 2);
}

// SQL text: `text` between single quotes, a quote in it doubled.
std::string sql_text(const std::string& text)
{
    std::string quoted = "'";
    for (const char character : text) {
        quoted += character;
        if (character == '\'') {
            quoted += '\'';
        }
    }
    return quoted + "'";
}

// Decision 0028: the lists read the manifest of an archive's revision that
// the last rebuild skipped as the rebuild reads it, to its last entry. Sample
// damage: the manifest of revision 2 has an entry that does not parse, and
// the index's record of revision 2 names that very file (its line 1, its
// BLAKE3 and its body), as if it had been recorded so. Its line 1 is then the
// one recorded, and the lists read line 1 alone: nothing would show that the
// rebuild skips the manifest. Rebuilt, the record is kept and the archive
// marked, and the list says that the manifest cannot be read, why, and to
// restore it, as the rebuild found.
void a_recorded_manifest_that_the_rebuild_skipped_is_listed_with_its_entry()
{
    test::EngineFixture fixture;
    const auto archive_uuid = registered_twice(fixture);
    const auto& layout = fixture.repository.context.repo;
    const auto second = va::manifest_path(layout.manifests(), archive_uuid, 2);
    const auto damaged = with_an_entry_that_does_not_parse(read_file(second));
    std::ofstream(second, std::ios::binary | std::ios::trunc) << damaged;
    const auto line = damaged.substr(0, damaged.find('\n'));
    const auto header = nlohmann::json::parse(line, nullptr, false);
    CHECK(header.is_object() && header.contains("body"));
    if (!header.is_object() || !header.contains("body")) {
        return;
    }
    fixture.repository.execute(("UPDATE archive_revisions SET header_json = " + sql_text(line) +
                                   ", header_blake3 = " + sql_text(va::blake3_hex(line)) + ", body_blake3 = " +
                                   sql_text(header["body"]["blake3"].get<std::string>()) + " WHERE revision = 2")
                                   .c_str());
    auto status = listed(fixture);
    CHECK(status && status->header && !status->unusable_manifest);

    const auto report = va::rebuild_index(fixture.store, *fixture.repository.files, layout);
    CHECK(report && report->revisions_kept.size() == 1 && report->skipped.size() == 1);
    status = listed(fixture);
    CHECK(status && !status->header && status->unusable_manifest &&
          va::plain_message(*status->unusable_manifest) ==
              "the manifest of revision 2 (manifests/" + archive_uuid +
                  "/r0002.vam) cannot be read: invalid manifest entry: unknown entry type x" +
                  test::put_back(archive_uuid, 2));

    // Read whole, the index's copy of the header is still compared with
    // line 1 (sample damage): the index differs, before any entry is read.
    fixture.repository.execute(
        "UPDATE archive_revisions SET header_json = 'not the header (sample damage)' WHERE revision = 2");
    status = listed(fixture);
    CHECK(status && status->unusable_manifest &&
          status->unusable_manifest->kind == va::UnusableManifest::Kind::IndexDiffers);
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
    an_import_restores_the_manifest_recorded();
    an_earlier_revision_whose_manifest_is_gone_is_left_out();
    an_earlier_revision_whose_manifest_is_lost_is_left_out_by_the_rebuild_it_names();
    the_history_keeps_an_archive_from_an_earlier_revision();
    a_manifest_without_a_record_is_read_to_its_last_entry();
    a_recorded_manifest_that_the_rebuild_skipped_is_listed_with_its_entry();
}
