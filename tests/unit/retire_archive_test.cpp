// Retiring an archive (decision 0003), also one whose latest manifest is lost
// and whose device is gone as well (decision 0028, point 9). Sample data
// throughout: sample folders on a fake volume and a temporary repository.
#include "test_support.h"

#include "engine_fixture.h"

#include "core/engine/abandon_registration.h"
#include "core/engine/archive_status.h"
#include "core/engine/rebuild_index.h"
#include "core/engine/relocate_engine.h"
#include "core/engine/retire_archive.h"
#include "core/engine/transfer.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iterator>

namespace {

std::string read_bytes(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

va::RegisterRequest at(const std::filesystem::path& root)
{
    va::RegisterRequest request;
    request.root = root;
    return request;
}

std::expected<va::Retirement, va::RetireFailure> retire(
    test::EngineFixture& fixture, const va::Uuid& archive_uuid, bool confirmed)
{
    return va::retire_archive(fixture.store, *fixture.repository.files, fixture.repository.context.repo,
        va::RetireRequest{archive_uuid, confirmed});
}

std::string status_of(test::EngineFixture& fixture, const va::Uuid& archive_uuid)
{
    const auto archive = fixture.store.find_archive(archive_uuid);
    return archive && *archive ? (*archive)->status : std::string{};
}

// The archive as `archive show` reads it.
std::optional<va::ArchiveStatus> listed(test::EngineFixture& fixture, const va::Uuid& archive_uuid)
{
    const auto statuses = va::archive_statuses(fixture.store, *fixture.repository.files,
        fixture.repository.context.repo, va::Policy{},
        std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()),
        va::StatusSelection{archive_uuid, true, true});
    CHECK(statuses && statuses->size() == 1);
    if (!statuses || statuses->size() != 1) {
        return std::nullopt;
    }
    return statuses->front();
}

using test::put_back_only;

constexpr const char* confirm_message = "run the command again with --confirm to retire the archive";

// An archive at /vol/a, registered twice: its archive uuid.
va::Uuid registered_twice(test::EngineFixture& fixture)
{
    fixture.archive.add_file("a/SAMPLE-one.txt", "one (sample data)");
    const auto first = fixture.register_root(fixture.archive.root() / "a");
    fixture.archive.add_file("a/SAMPLE-two.txt", "two (sample data)");
    auto again = at(fixture.archive.root() / "a");
    again.new_revision = true;
    const auto second = fixture.register_root(again);
    CHECK(first && second && second->revision == 2);
    return first ? first->archive_uuid : va::Uuid{};
}

// Decision 0003: an archive whose latest manifest can be used is retired at
// once, without a confirmation, as before; so is one of which only the index
// is wrong (its copy of the header differs from the manifest's line 1: a
// rebuild gives it back, so it is not lost), and one whose first registration
// never finished (revision 0). Retiring an archive retired before changes
// nothing.
void an_archive_whose_manifest_can_be_used_is_retired_at_once()
{
    test::EngineFixture fixture;
    fixture.archive.add_file("a/SAMPLE-one.txt", "one (sample data)");
    fixture.archive.add_file("b/SAMPLE-two.txt", "two (sample data)");
    fixture.archive.add_file("c/SAMPLE-three.txt", "three (sample data)");
    const auto a = fixture.register_root(fixture.archive.root() / "a");
    const auto b = fixture.register_root(fixture.archive.root() / "b");
    CHECK(a && b);
    if (!a || !b) {
        return;
    }
    const auto retired = retire(fixture, a->archive_uuid, false);
    CHECK(retired && retired->archive_uuid == a->archive_uuid && retired->label == "ARCH-0001" &&
          retired->revision == 1 && !retired->lost_manifest && !retired->already_retired);
    CHECK(status_of(fixture, a->archive_uuid) == "RETIRED");
    const auto again = retire(fixture, a->archive_uuid, false);
    CHECK(again && again->already_retired && !again->lost_manifest);

    // A first registration that never finished: the archive has no revision.
    fixture.archive.node("c/SAMPLE-three.txt").open_error =
        va::IoError{va::IoErrorKind::DeviceRemoved, 0, "unplugged (sample)"};
    const auto interrupted = fixture.register_root(fixture.archive.root() / "c");
    CHECK(!interrupted && interrupted.error().kind == va::RegisterFailure::Kind::Incomplete);
    const auto archives = fixture.store.archives();
    CHECK(archives && archives->size() == 3);
    if (archives && archives->size() == 3) {
        const auto& c = archives->back();
        const auto first = retire(fixture, c.archive_uuid, false);
        CHECK(first && first->revision == 0 && !first->lost_manifest);
        CHECK(status_of(fixture, c.archive_uuid) == "RETIRED");
    }

    fixture.repository.execute("UPDATE archive_revisions SET header_json = replace(header_json, "
                               "'\"volume_label\":\"ARCHIVE\"', '\"volume_label\":\"OTHER\"') WHERE archive_uuid = "
                               "(SELECT archive_uuid FROM archives WHERE label = 'ARCH-0002')");
    const auto index_differs = retire(fixture, b->archive_uuid, false);
    CHECK(index_differs && !index_differs->lost_manifest);
    if (!index_differs) {
        std::cerr << "  an index that differs: " << index_differs.error().message << '\n';
    }
    CHECK(status_of(fixture, b->archive_uuid) == "RETIRED");

}

// Decision 0028, point 9: archive A at /vol/a (sample data), registered twice
// and verified at revision 2, whose manifest of revision 2 is lost where
// matching reads it: gone, its first line changed, or gone with its record,
// so that the index has no record of it. Meanwhile a registration of a new
// folder stops on it. Retiring A without a confirmation
// says what it would retire, and writes nothing; confirmed, A is retired, and
// a new folder registers. A is listed retired with its manifest lost, its
// plain message ending with the import, as it takes no new revision: the
// registration over its lost manifest is refused. A verification of it by its
// uuid fails on its manifest, read no media, and is recorded in an intact
// chain, saying the same. Retiring it again needs no confirmation and changes
// nothing.
void an_archive_whose_manifest_is_lost_is_retired_once_confirmed()
{
    enum class Case { Missing, FirstLine, NotIndexed };
    for (const auto what : {Case::Missing, Case::FirstLine, Case::NotIndexed}) {
        test::EngineFixture fixture;
        const auto archive_uuid = registered_twice(fixture);
        va::VerifyRequest by_uuid;
        by_uuid.archive_uuid = archive_uuid;
        const auto verified = fixture.verify(by_uuid);
        CHECK(verified && verified->result == "PASSED" && verified->revision == 2);
        const auto& layout = fixture.repository.context.repo;
        const auto second = va::manifest_path(layout.manifests(), archive_uuid, 2);
        auto bytes = read_bytes(second);
        auto kind = va::UnusableManifest::Kind::Missing;
        std::string what_is_wrong = "the manifest of revision 2 (manifests/" + archive_uuid + "/r0002.vam) is missing";
        if (what == Case::Missing || what == Case::NotIndexed) {
            std::filesystem::remove(second);
        } else if (what == Case::FirstLine) {
            bytes[bytes.find("\"volume_label\":\"ARCHIVE\"") + 16] ^= 0x01; // "ARCHIVE" -> "@RCHIVE"
            std::ofstream(second, std::ios::binary | std::ios::trunc) << bytes;
            kind = va::UnusableManifest::Kind::Changed;
            what_is_wrong = "the manifest of revision 2 (manifests/" + archive_uuid +
                            "/r0002.vam) has changed: its first line differs from what the repository recorded";
        }
        if (what == Case::NotIndexed) {
            fixture.repository.execute("DELETE FROM archive_revisions WHERE revision = 2; "
                                       "DELETE FROM files WHERE revision = 2");
            kind = va::UnusableManifest::Kind::NotIndexed;
            what_is_wrong = "the repository's index has no record of the manifest of revision 2 (manifests/" +
                            archive_uuid + "/r0002.vam), which is missing";
        }
        fixture.archive.add_file("b/SAMPLE-three.txt", "three (sample data)");
        fixture.archive.add_file("c/SAMPLE-four.txt", "four (sample data)");
        const auto stopped = fixture.register_root(fixture.archive.root() / "b");
        CHECK(!stopped);

        // Not confirmed: what it would retire, and nothing written.
        const auto asked = retire(fixture, archive_uuid, false);
        CHECK(!asked && asked.error().kind == va::RetireFailure::Kind::Unconfirmed &&
              asked.error().message == confirm_message);
        CHECK(!asked && asked.error().retirement && asked.error().retirement->archive_uuid == archive_uuid &&
              asked.error().retirement->label == "ARCH-0001" && asked.error().retirement->revision == 2 &&
              asked.error().retirement->lost_manifest && asked.error().retirement->lost_manifest->kind == kind &&
              va::what_is_wrong_with(*asked.error().retirement->lost_manifest) == what_is_wrong &&
              // Before it is retired, its message names the other way out.
              va::plain_message(*asked.error().retirement->lost_manifest) ==
                  what_is_wrong + test::put_back(archive_uuid, 2));
        CHECK(status_of(fixture, archive_uuid) == "REGISTERED");

        // Confirmed: retired, with its manifest lost.
        const auto retired = retire(fixture, archive_uuid, true);
        CHECK(retired && retired->revision == 2 && retired->lost_manifest && !retired->already_retired &&
              va::plain_message(*retired->lost_manifest) == what_is_wrong + put_back_only(2));
        if (!retired) {
            std::cerr << "  retired: " << retired.error().message << '\n';
            continue;
        }
        CHECK(status_of(fixture, archive_uuid) == "RETIRED");
        const auto c = fixture.register_root(fixture.archive.root() / "c");
        CHECK(c && c->revision == 1 && c->archive_uuid != archive_uuid);
        if (!c) {
            std::cerr << "  a new folder after the retirement: " << c.error().message << '\n';
        }

        const auto shown = listed(fixture, archive_uuid);
        CHECK(shown && shown->manifest_lost && shown->unusable_manifest && shown->unusable_manifest->kind == kind &&
              va::plain_message(*shown->unusable_manifest) == what_is_wrong + put_back_only(2) &&
              shown->last_full && shown->last_full->result == "PASSED" && !shown->full_overdue);

        const auto over_it = fixture.register_root([&] {
            va::RegisterRequest request = at(fixture.archive.root() / "a");
            request.lost_manifest_of = archive_uuid;
            request.confirmed = true;
            return request;
        }());
        CHECK(!over_it && over_it.error().kind == va::RegisterFailure::Kind::Refused &&
              over_it.error().message == "a retired archive takes no new revisions (decision 0003)");

        fixture.archive.bytes_read = 0;
        const auto failed = fixture.verify(by_uuid);
        CHECK(failed && failed->result == "FAILED" && !failed->media_read && failed->revision == 2 &&
              fixture.archive.bytes_read == 0 && !failed->warnings.empty() &&
              failed->warnings.front() == "manifest: " + what_is_wrong + put_back_only(2));
        const auto chain = fixture.store.check_history(fixture.repository.context.repository_uuid);
        CHECK(chain && chain->runs == 2 && !chain->first_broken_run);

        // Retired before: nothing to confirm, nothing changed.
        const auto again = retire(fixture, archive_uuid, false);
        CHECK(again && again->already_retired && again->lost_manifest &&
              va::plain_message(*again->lost_manifest) == what_is_wrong + put_back_only(2));
    }
}

// Decision 0028, point 9: a manifest changed only in its body is lost, but
// matching reads it by its line 1, which is intact, and keeps its archive a
// candidate. Archive A at /vol/a, registered twice and verified (sample
// data), one byte of the body of its manifest of revision 2 changed (sample
// damage). Retiring A changes nothing of matching: it is retired at once, as
// any archive, without a confirmation and without its manifest named as lost.
// Retired, A is still recognised: a verification of its root matches it and
// fails on the body, reading the media, its folder takes no new archive, and
// a new folder registers. Once a rebuild has skipped the manifest, the lists,
// which read it whole then, show A retired with its manifest lost; retiring
// it again changes nothing.
void an_archive_whose_manifest_changed_only_in_its_body_is_retired_at_once()
{
    test::EngineFixture fixture;
    const auto archive_uuid = registered_twice(fixture);
    va::VerifyRequest by_uuid;
    by_uuid.archive_uuid = archive_uuid;
    const auto verified = fixture.verify(by_uuid);
    CHECK(verified && verified->result == "PASSED" && verified->revision == 2);
    const auto& layout = fixture.repository.context.repo;
    const auto second = va::manifest_path(layout.manifests(), archive_uuid, 2);
    auto bytes = read_bytes(second);
    bytes[bytes.size() - 5] ^= 0x01;
    std::ofstream(second, std::ios::binary | std::ios::trunc) << bytes;

    const auto retired = retire(fixture, archive_uuid, false);
    CHECK(retired && !retired->lost_manifest && !retired->already_retired && retired->revision == 2);
    if (!retired) {
        std::cerr << "  retiring a manifest changed only in its body: " << retired.error().message << '\n';
    }
    CHECK(status_of(fixture, archive_uuid) == "RETIRED");
    const auto by_root = fixture.verify(fixture.archive.root() / "a");
    CHECK(by_root && by_root->archive_uuid == archive_uuid && by_root->result == "FAILED" && by_root->media_read);
    if (!by_root) {
        std::cerr << "  its root after the retirement: " << by_root.error().message << '\n';
    }
    const auto again = fixture.register_root(fixture.archive.root() / "a");
    CHECK(!again && again.error().kind == va::RegisterFailure::Kind::Refused &&
          again.error().message.starts_with("already registered as archive " + archive_uuid));
    fixture.archive.add_file("c/SAMPLE-four.txt", "four (sample data)");
    const auto c = fixture.register_root(fixture.archive.root() / "c");
    CHECK(c && c->revision == 1);

    const auto rebuilt = va::rebuild_index(fixture.store, *fixture.repository.files, layout, test::not_cancelled(), test::no_progress());
    CHECK(rebuilt && rebuilt->revisions_kept.size() == 1);
    const auto shown = listed(fixture, archive_uuid);
    CHECK(shown && shown->manifest_lost && shown->unusable_manifest &&
          va::plain_message(*shown->unusable_manifest) ==
              "the manifest of revision 2 (manifests/" + archive_uuid +
                  "/r0002.vam) has changed: its body does not match its header" + put_back_only(2));
    const auto once_more = retire(fixture, archive_uuid, false);
    CHECK(once_more && once_more->already_retired && !once_more->lost_manifest);
}

// Decision 0028, point 9: a retired archive whose latest manifest is lost is
// no possible owner of a root. Archives A at /vol/a and B at /vol/b (sample
// data), A's manifest gone. While A is not retired, every root that is not
// plainly another archive's stops on it: B's root without a device serial to
// compare (MATCH_WEAK), a relocation of B, and A's own folder, registered as
// a new archive. A retired, B's root verifies as B's, B relocates, and A's
// folder registers as a new archive: should A's device turn up again,
// nothing recognises it. A retired archive whose index alone differs from its
// manifest still stops them, as its manifest is not lost; rebuilt, it stops
// nothing. A's manifest put back from an export, A is a retired archive like
// any other: its root is recognised as A's again.
void a_retired_archive_whose_manifest_is_lost_owns_no_root()
{
    test::EngineFixture fixture;
    fixture.archive.add_file("a/SAMPLE-one.txt", "one (sample data)");
    fixture.archive.add_file("b/SAMPLE-two.txt", "two (sample data)");
    fixture.archive.add_file("b-copy/SAMPLE-two.txt", "two (sample data)");
    const auto a = fixture.register_root(fixture.archive.root() / "a");
    const auto b = fixture.register_root(fixture.archive.root() / "b");
    CHECK(a && b);
    if (!a || !b) {
        return;
    }
    const auto& layout = fixture.repository.context.repo;
    const auto exports = layout.root.parent_path() / "SAMPLE-export";
    const auto exported = va::export_archive(fixture.store, *fixture.repository.files, *fixture.repository.writer,
        layout, a->archive_uuid, exports, test::not_cancelled(), test::no_progress());
    CHECK(exported.has_value());
    const auto path = va::manifest_path(layout.manifests(), a->archive_uuid, 1);
    std::filesystem::remove(path);

    const auto volume = fixture.media.volume;
    const auto without_serial = [&] {
        fixture.media.volume.devices.front().serial.clear();
    };
    const auto relocate_b = [&] {
        va::RelocateServices services{fixture.archive, *fixture.repository.files, *fixture.repository.writer,
            fixture.media, fixture.store, layout, fixture.repository.context.repository_uuid, fixture.host,
            fixture.processes};
        return va::relocate_archive(services,
            va::RelocateRequest{b->archive_uuid, fixture.archive.root() / "b-copy", std::nullopt, false},
            fixture.cancel, fixture.progress);
    };
    without_serial();
    const auto stopped = fixture.verify(fixture.archive.root() / "b");
    CHECK(!stopped && stopped.error().message.starts_with("this root may belong to archive ARCH-0001"));
    fixture.media.volume = volume;
    const auto not_relocated = relocate_b();
    CHECK(!not_relocated && not_relocated.error().kind == va::RelocateFailure::Kind::Repository);
    auto separate = at(fixture.archive.root() / "a");
    const auto not_registered = fixture.register_root(separate);
    CHECK(!not_registered && not_registered.error().kind == va::RegisterFailure::Kind::Repository);

    const auto retired = retire(fixture, a->archive_uuid, true);
    CHECK(retired && retired->lost_manifest);
    without_serial();
    const auto verified = fixture.verify(fixture.archive.root() / "b");
    CHECK(verified && verified->archive_uuid == b->archive_uuid && verified->result == "PASSED_WITH_WARNINGS");
    if (!verified) {
        std::cerr << "  B's root beside a retired archive whose manifest is lost: " << verified.error().message << '\n';
    }
    fixture.media.volume = volume;
    const auto relocated = relocate_b();
    CHECK(relocated && relocated->revision == 2);
    if (!relocated) {
        std::cerr << "  B relocated: " << relocated.error().message << '\n';
    }
    const auto again = fixture.register_root(separate);
    CHECK(again && again->archive_uuid != a->archive_uuid && again->revision == 1);
    if (!again) {
        std::cerr << "  A's folder as a new archive: " << again.error().message << '\n';
    }

    // B's index differs from its intact manifest: retired, B is not lost,
    // and it stops a new folder until the index is rebuilt.
    fixture.archive.add_file("d/SAMPLE-five.txt", "five (sample data)");
    const auto b_retired = retire(fixture, b->archive_uuid, false);
    CHECK(b_retired && !b_retired->lost_manifest);
    fixture.repository.execute("UPDATE archive_revisions SET header_json = replace(header_json, "
                               "'\"volume_label\":\"ARCHIVE\"', '\"volume_label\":\"OTHER\"') WHERE revision = 2");
    const auto index_stops = fixture.register_root(fixture.archive.root() / "d");
    CHECK(!index_stops && index_stops.error().kind == va::RegisterFailure::Kind::Repository &&
          index_stops.error().message.contains("rebuild the index with `repo rebuild-index`"));
    const auto shown = listed(fixture, b->archive_uuid);
    CHECK(shown && shown->unusable_manifest && !shown->manifest_lost);
    CHECK(va::rebuild_index(fixture.store, *fixture.repository.files, layout, test::not_cancelled(), test::no_progress()).has_value());
    const auto d = fixture.register_root(fixture.archive.root() / "d");
    CHECK(d && d->revision == 1);

    // A's manifest put back: A is recognised again, at the folder that was
    // registered as a new archive meanwhile, which both archives match now.
    const auto imported = va::import_manifest(fixture.store, *fixture.repository.files, *fixture.repository.writer,
        layout, exports / "r0001.vam", false, test::not_cancelled(), test::no_progress());
    CHECK(imported && imported->restored);
    const auto a_listed = listed(fixture, a->archive_uuid);
    CHECK(a_listed && a_listed->header && !a_listed->unusable_manifest && !a_listed->manifest_lost &&
          a_listed->archive.status == "RETIRED");
    const auto both = fixture.verify(fixture.archive.root() / "a");
    CHECK(!both && both.error().identity.result == va::MatchResult::Ambiguous &&
          std::ranges::count(both.error().identity.archives, a->archive_uuid) == 1);
}

// Decision 0028, point 9: the lost manifest of a retired archive behind an
// index that seems to be all that is wrong with it, as line 1 alone tells.
// Archive A at /vol/a, registered twice, and B at /vol/b (sample data); A's
// manifest of revision 2 damaged (sample damage): one byte of its body
// changed, and the index's record of revision 2 gone, as an older version's
// rebuild dropped it, or the index's copy of its header changed; or an entry
// that does not parse in a manifest that the record names (its line 1 and
// body) and whose header copy in the index differs. `repo rebuild-index`
// skips such a manifest and keeps the index as it was, so no rebuild repairs
// it. While A is not retired, a new folder and a relocation of B stop on it
// with what line 1 alone tells (rebuild the index), and A is listed so.
// Retired once confirmed, as `archive retire` judges the manifest as
// matching does once A is retired, A is no candidate of matching: the new
// folder registers and B relocates, and A is listed
// with its manifest lost and what the whole manifest says, which ends with
// the import; so after a rebuild as well, after which another new folder
// registers.
void a_lost_manifest_behind_a_wrong_index_leaves_matching_once_retired()
{
    enum class Case { NotIndexed, IndexDiffers, EntryBehindIndexDiffers };
    for (const auto what : {Case::NotIndexed, Case::IndexDiffers, Case::EntryBehindIndexDiffers}) {
        test::EngineFixture fixture;
        const auto archive_uuid = registered_twice(fixture);
        fixture.archive.add_file("b/SAMPLE-three.txt", "three (sample data)");
        fixture.archive.add_file("b-copy/SAMPLE-three.txt", "three (sample data)");
        fixture.archive.add_file("c/SAMPLE-four.txt", "four (sample data)");
        fixture.archive.add_file("d/SAMPLE-five.txt", "five (sample data)");
        const auto b = fixture.register_root(fixture.archive.root() / "b");
        CHECK(b.has_value());
        if (!b) {
            return;
        }
        const auto& layout = fixture.repository.context.repo;
        const auto second = va::manifest_path(layout.manifests(), archive_uuid, 2);
        const auto manifest = "the manifest of revision 2 (manifests/" + archive_uuid + "/r0002.vam)";
        const auto of_a = " WHERE archive_uuid = " + test::sql_text(archive_uuid) + " AND revision = 2";
        const auto header_copy_changed = "UPDATE archive_revisions SET header_json = replace(header_json, "
                                         "'\"volume_label\":\"ARCHIVE\"', '\"volume_label\":\"OTHER\"')" +
                                         of_a;
        auto bytes = read_bytes(second);
        std::string line_1_says;  // what line 1 alone tells, before the archive is retired
        std::string whole_says;   // what the whole manifest tells
        if (what == Case::EntryBehindIndexDiffers) {
            bytes = test::with_an_entry_that_does_not_parse(bytes);
            std::ofstream(second, std::ios::binary | std::ios::trunc) << bytes;
            test::record_as_written(fixture.repository, archive_uuid, 2, bytes);
            fixture.repository.execute(header_copy_changed.c_str());
            line_1_says =
                "the repository's index differs from " + manifest + "; rebuild the index with `repo rebuild-index`";
            whole_says = manifest + " cannot be read: invalid manifest entry: unknown entry type x";
        } else {
            bytes[bytes.size() - 5] ^= 0x01;
            std::ofstream(second, std::ios::binary | std::ios::trunc) << bytes;
            if (what == Case::NotIndexed) {
                fixture.repository.execute(
                    ("DELETE FROM archive_revisions" + of_a + "; DELETE FROM files" + of_a).c_str());
                line_1_says = "the repository's index has no record of " + manifest +
                              "; rebuild the index with `repo rebuild-index`";
                whole_says = "the repository's index has no record of " + manifest +
                             ", which cannot be read: manifest body does not match its header (ManifestHashMismatch)";
            } else {
                fixture.repository.execute(header_copy_changed.c_str());
                line_1_says =
                    "the repository's index differs from " + manifest + "; rebuild the index with `repo rebuild-index`";
                whole_says = manifest + " has changed: its body does not match its header";
            }
        }
        const auto relocate_b = [&] {
            va::RelocateServices services{fixture.archive, *fixture.repository.files, *fixture.repository.writer,
                fixture.media, fixture.store, layout, fixture.repository.context.repository_uuid, fixture.host,
                fixture.processes};
            return va::relocate_archive(services,
                va::RelocateRequest{b->archive_uuid, fixture.archive.root() / "b-copy", std::nullopt, false},
                fixture.cancel, fixture.progress);
        };

        // Not retired: line 1 alone tells, and every registration of a new
        // folder and every relocation stops on it.
        const auto stopped = fixture.register_root(fixture.archive.root() / "c");
        CHECK(!stopped && stopped.error().kind == va::RegisterFailure::Kind::Repository &&
              stopped.error().message == line_1_says);
        if (!stopped && stopped.error().message != line_1_says) {
            std::cerr << "  a new folder beside A: " << stopped.error().message << '\n';
        }
        const auto not_relocated = relocate_b();
        CHECK(!not_relocated && not_relocated.error().kind == va::RelocateFailure::Kind::Repository);
        const auto before = listed(fixture, archive_uuid);
        CHECK(before && before->unusable_manifest && !before->manifest_lost &&
              va::plain_message(*before->unusable_manifest) == line_1_says);

        // Retired once confirmed, as the whole manifest is lost.
        const auto asked = retire(fixture, archive_uuid, false);
        CHECK(!asked && asked.error().kind == va::RetireFailure::Kind::Unconfirmed && asked.error().retirement &&
              asked.error().retirement->lost_manifest &&
              va::what_is_wrong_with(*asked.error().retirement->lost_manifest) == whole_says);
        if (asked) {
            std::cerr << "  retired without a confirmation: lost " << asked->lost_manifest.has_value() << '\n';
        }
        const auto retired = retire(fixture, archive_uuid, true);
        CHECK(retired && retired->lost_manifest && va::what_is_wrong_with(*retired->lost_manifest) == whole_says);

        // No candidate of matching any more.
        const auto c = fixture.register_root(fixture.archive.root() / "c");
        CHECK(c && c->revision == 1 && c->archive_uuid != archive_uuid);
        if (!c) {
            std::cerr << "  a new folder after the retirement: " << c.error().message << '\n';
        }
        const auto relocated = relocate_b();
        CHECK(relocated && relocated->revision == 2);
        if (!relocated) {
            std::cerr << "  B relocated after the retirement: " << relocated.error().message << '\n';
        }
        const auto shown = listed(fixture, archive_uuid);
        CHECK(shown && shown->manifest_lost && shown->unusable_manifest &&
              va::plain_message(*shown->unusable_manifest) == whole_says + put_back_only(2));
        if (shown && shown->unusable_manifest && !shown->manifest_lost) {
            std::cerr << "  listed: " << va::plain_message(*shown->unusable_manifest) << '\n';
        }

        // A rebuild skips the manifest and leaves the index as it was.
        const auto rebuilt = va::rebuild_index(fixture.store, *fixture.repository.files, layout, test::not_cancelled(), test::no_progress());
        CHECK(rebuilt && rebuilt->revisions_kept.size() + rebuilt->revisions_not_indexed.size() == 1);
        const auto after = listed(fixture, archive_uuid);
        CHECK(after && after->manifest_lost && after->archive.current_revision == 2);
        const auto d = fixture.register_root(fixture.archive.root() / "d");
        CHECK(d && d->revision == 1);
        if (!d) {
            std::cerr << "  a new folder after a rebuild: " << d.error().message << '\n';
        }
    }
}

// Decision 0028, point 9: retiring an archive whose latest manifest is lost
// writes nothing while a registration of the archive is unfinished, which
// would give it a new revision: a registration over the lost manifest that
// stopped (its device removed), named with both ways out, retired once it is
// abandoned; and one made for an older revision (decision 0030), named with
// the abandon alone. Nor where the archive has another revision by the time
// it would be retired (sample damage while the manifest is checked).
void retiring_writes_nothing_beside_a_registration_or_a_later_revision()
{
    {
        test::EngineFixture fixture;
        const auto archive_uuid = registered_twice(fixture);
        std::filesystem::remove(va::manifest_path(fixture.repository.context.repo.manifests(), archive_uuid, 2));
        fixture.archive.node("a/SAMPLE-two.txt").open_error =
            va::IoError{va::IoErrorKind::DeviceRemoved, 0, "unplugged (sample)"};
        auto over_it = at(fixture.archive.root() / "a");
        over_it.lost_manifest_of = archive_uuid;
        over_it.confirmed = true;
        const auto interrupted = fixture.register_root(over_it);
        CHECK(!interrupted && interrupted.error().kind == va::RegisterFailure::Kind::Incomplete);
        const auto refused = retire(fixture, archive_uuid, true);
        CHECK(!refused && refused.error().kind == va::RetireFailure::Kind::Refused &&
              refused.error().message ==
                  "an unfinished registration of archive " + archive_uuid + " (root \"" +
                      va::path_to_utf8(fixture.archive.root() / "a") +
                      "\") is creating revision 3; nothing was changed (finish it with `register --resume` first, "
                      "or abandon it with `archive abandon-registration " +
                      archive_uuid + "`)");
        CHECK(status_of(fixture, archive_uuid) == "REGISTERED");
        va::AbandonServices services{fixture.store, fixture.processes, fixture.host.host};
        CHECK(va::abandon_registration(services, va::AbandonRequest{archive_uuid, std::nullopt, std::nullopt}));
        const auto retired = retire(fixture, archive_uuid, true);
        CHECK(retired && retired->lost_manifest);
        CHECK(status_of(fixture, archive_uuid) == "RETIRED");
    }
    {
        // A registration of revision 2, interrupted; revision 3 imported; the
        // manifest of revision 3 lost.
        test::EngineFixture fixture;
        fixture.archive.add_file("a/SAMPLE-one.txt", "one (sample data)");
        const auto first = fixture.register_root(fixture.archive.root() / "a");
        CHECK(first.has_value());
        if (!first) {
            return;
        }
        fixture.archive.add_file("a/SAMPLE-two.txt", "two (sample data)");
        fixture.archive.node("a/SAMPLE-two.txt").open_error =
            va::IoError{va::IoErrorKind::DeviceRemoved, 0, "unplugged (sample)"};
        auto again = at(fixture.archive.root() / "a");
        again.new_revision = true;
        const auto interrupted = fixture.register_root(again);
        CHECK(!interrupted && interrupted.error().kind == va::RegisterFailure::Kind::Incomplete);
        test::import_later_revision(fixture, fixture.store, first->archive_uuid, 3);
        std::filesystem::remove(
            va::manifest_path(fixture.repository.context.repo.manifests(), first->archive_uuid, 3));
        const auto refused = retire(fixture, first->archive_uuid, true);
        CHECK(!refused && refused.error().kind == va::RetireFailure::Kind::Refused &&
              refused.error().message ==
                  "the unfinished registration of archive " + first->archive_uuid + " (root \"" +
                      va::path_to_utf8(fixture.archive.root() / "a") +
                      "\") would create revision 2, but the archive now has a later revision (3); the registration "
                      "can no longer finish: abandon it with `archive abandon-registration " +
                      first->archive_uuid + "`");
        CHECK(status_of(fixture, first->archive_uuid) == "REGISTERED");
    }
    {
        // Another revision recorded while the manifest was checked (sample
        // damage of the archive's record, as an import of revision 3 would
        // leave it).
        test::EngineFixture fixture;
        const auto archive_uuid = registered_twice(fixture);
        std::filesystem::remove(va::manifest_path(fixture.repository.context.repo.manifests(), archive_uuid, 2));
        const std::string sql = "UPDATE archives SET current_revision = 3 WHERE archive_uuid = '" + archive_uuid + "'";
        test::HookedFiles hooked(*fixture.repository.files, "r0002.vam",
            [&] { fixture.repository.execute(sql.c_str()); });
        const auto refused = va::retire_archive(fixture.store, hooked, fixture.repository.context.repo,
            va::RetireRequest{archive_uuid, true});
        CHECK(!refused && refused.error().kind == va::RetireFailure::Kind::Refused &&
              refused.error().message == "archive " + archive_uuid +
                                             " now has revision 3, not revision 2; nothing was changed (run the "
                                             "command again)");
        CHECK(status_of(fixture, archive_uuid) == "REGISTERED");
    }
}

} // namespace

void retire_archive_tests()
{
    an_archive_whose_manifest_can_be_used_is_retired_at_once();
    an_archive_whose_manifest_is_lost_is_retired_once_confirmed();
    an_archive_whose_manifest_changed_only_in_its_body_is_retired_at_once();
    a_retired_archive_whose_manifest_is_lost_owns_no_root();
    a_lost_manifest_behind_a_wrong_index_leaves_matching_once_retired();
    retiring_writes_nothing_beside_a_registration_or_a_later_revision();
}
