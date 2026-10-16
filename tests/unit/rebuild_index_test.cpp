#include "test_support.h"

#include "engine_fixture.h"

#include "core/engine/rebuild_index.h"
#include "core/engine/relocate_engine.h"
#include "core/hash/hashers.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <iterator>
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

    // A damaged manifest is reported, not indexed.
    const auto path = fixture.repository.context.repo.root / (*revisions)[0].manifest_relpath;
    std::string bytes;
    {
        std::ifstream input(path, std::ios::binary);
        bytes.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    }
    bytes[bytes.size() - 3] ^= 0x01;
    std::ofstream(path, std::ios::binary | std::ios::trunc) << bytes;
    const auto damaged = va::rebuild_index(fixture.store, *fixture.repository.files, fixture.repository.context.repo);
    CHECK(damaged && damaged->skipped.size() == 1 && damaged->revisions == 1);
    if (damaged && damaged->skipped.size() == 1) {
        CHECK(damaged->skipped.front() == "cannot read manifest " + first->archive_uuid +
            "/r0001.vam: manifest body does not match its header (ManifestHashMismatch)");
    }
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
    CHECK(report && report->skipped.size() == 5 && report->revisions == 0);
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

} // namespace

void rebuild_index_tests()
{
    rebuild_keeps_database_facts();
    skipped_manifests_say_why();
    rebuild_keeps_saved_progress();
}
