#include "test_support.h"

#include "engine_fixture.h"

#include "common/time.h"
#include "core/engine/archive_status.h"
#include "core/engine/recorded_manifest.h"
#include "core/manifest/manifest_writer.h"

#include <algorithm>
#include <fstream>
#include <iterator>

namespace {

std::chrono::sys_seconds days_from_now(int days)
{
    return std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()) +
           std::chrono::days(days);
}

// The fixture's archives as the Archive List reads them: each header from its manifest.
std::expected<std::vector<va::ArchiveStatus>, va::IoError> statuses_of(
    test::EngineFixture& fixture, const va::Policy& policy, std::chrono::sys_seconds now)
{
    return va::archive_statuses(
        fixture.store, *fixture.repository.files, fixture.repository.context.repo, policy, now);
}

void timestamps_round_trip()
{
    const auto parsed = va::parse_timestamp("2026-10-08T05:06:07Z");
    CHECK(parsed.has_value());
    if (parsed) {
        CHECK(std::format("{:%FT%TZ}", *parsed) == "2026-10-08T05:06:07Z");
    }
    CHECK(!va::parse_timestamp("2026-13-08T05:06:07Z"));
    CHECK(!va::parse_timestamp("yesterday"));
    CHECK(va::parse_timestamp(va::current_timestamp()).has_value());
}

// R-21 / R-105 / decision 0007: last FULL and last verify with OVERDUE flags.
void overdue_follows_policy()
{
    test::EngineFixture fixture;
    fixture.archive.add_file("a.txt", "a");
    const auto registered = fixture.register_root(fixture.archive.root());
    CHECK(registered.has_value());
    const va::Policy policy; // 365 / 180 days

    auto statuses = statuses_of(fixture, policy, days_from_now(0));
    CHECK(statuses && statuses->size() == 1);
    if (statuses && statuses->size() == 1) {
        CHECK(!statuses->front().last_full && !statuses->front().any_overdue);
    }

    CHECK(fixture.verify(fixture.archive.root()).has_value());
    statuses = statuses_of(fixture, policy, days_from_now(200));
    if (statuses && statuses->size() == 1) {
        const auto& status = statuses->front();
        CHECK(status.last_full && status.last_full->result == "PASSED");
        CHECK(status.any_overdue && !status.full_overdue);
    }
    statuses = statuses_of(fixture, policy, days_from_now(400));
    CHECK(statuses && statuses->front().full_overdue);

    // Decision 0003: retired archives are never overdue.
    auto archive = fixture.store.find_archive(registered->archive_uuid);
    if (archive && *archive) {
        (*archive)->status = "RETIRED";
        CHECK(fixture.store.update_archive(**archive));
    }
    statuses = statuses_of(fixture, policy, days_from_now(400));
    CHECK(statuses && !statuses->front().full_overdue && !statuses->front().any_overdue);
}

// R-88: shared labels are shown with a short uuid.
void shared_labels_are_disambiguated()
{
    test::EngineFixture fixture;
    fixture.archive.add_file("one/a.txt", "a");
    fixture.archive.add_file("two/b.txt", "b");
    CHECK(fixture.register_root(fixture.archive.root() / "one").has_value());
    va::RegisterRequest request;
    request.root = fixture.archive.root() / "two";
    request.label = "ARCH-0001";
    CHECK(fixture.register_root(request).has_value());

    const auto statuses = statuses_of(fixture, va::Policy{}, days_from_now(0));
    CHECK(statuses && statuses->size() == 2);
    if (statuses && statuses->size() == 2) {
        const auto& status = statuses->front();
        CHECK(status.label_shared);
        CHECK(va::display_label(status) == "ARCH-0001 (" + status.archive.archive_uuid.substr(0, 8) + ")");
    }
}

// Decision 0028: one archive whose latest manifest cannot be used never stops
// the list. Three archives on one volume (sample data): A, B and C. B was
// verified; then its manifest went missing, and one byte of C's header line
// changed. Every archive is listed: A with its header, B and C each with what
// is wrong with its manifest (its kind, failure class and plain message) and
// no header, their verifications and OVERDUE read from the history as for any
// archive. Only B, as `archive show` reads it, reads the same; the labels
// alone, which read no manifest, show no problem.
void a_damaged_archive_never_stops_the_list()
{
    test::EngineFixture fixture;
    fixture.archive.add_file("a/one.txt", "a (sample data)");
    fixture.archive.add_file("b/two.txt", "b (sample data)");
    fixture.archive.add_file("c/three.txt", "c (sample data)");
    const auto a = fixture.register_root(fixture.archive.root() / "a");
    const auto b = fixture.register_root(fixture.archive.root() / "b");
    const auto c = fixture.register_root(fixture.archive.root() / "c");
    CHECK(a && b && c);
    if (!a || !b || !c) {
        return;
    }
    const auto verified = fixture.verify(fixture.archive.root() / "b");
    CHECK(verified && verified->result == "PASSED");
    const auto& manifests = fixture.repository.context.repo.manifests();
    std::filesystem::remove(va::manifest_path(manifests, b->archive_uuid, 1));
    const auto c_path = va::manifest_path(manifests, c->archive_uuid, 1);
    std::string bytes;
    {
        std::ifstream input(c_path, std::ios::binary);
        bytes.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    }
    bytes[bytes.find("\"volume_label\":\"ARCHIVE\"") + 16] ^= 0x01; // "ARCHIVE" -> "@RCHIVE"
    std::ofstream(c_path, std::ios::binary | std::ios::trunc) << bytes;

    const auto statuses = statuses_of(fixture, va::Policy{}, days_from_now(0));
    CHECK(statuses && statuses->size() == 3);
    if (!statuses || statuses->size() != 3) {
        return;
    }
    const auto& listed_a = (*statuses)[0];
    const auto& listed_b = (*statuses)[1];
    const auto& listed_c = (*statuses)[2];
    CHECK(listed_a.archive.archive_uuid == a->archive_uuid && listed_a.header && !listed_a.unusable_manifest);
    CHECK(listed_b.archive.archive_uuid == b->archive_uuid && !listed_b.header && listed_b.unusable_manifest &&
          listed_b.unusable_manifest->kind == va::UnusableManifest::Kind::Missing &&
          va::failure_class_of(*listed_b.unusable_manifest) == "RepositoryError" &&
          va::plain_message(*listed_b.unusable_manifest) ==
              "the manifest of revision 1 (manifests/" + b->archive_uuid + "/r0001.vam) is missing" +
                  test::put_back(b->archive_uuid, 1));
    CHECK(listed_b.last_full && listed_b.last_full->result == "PASSED" && !listed_b.full_overdue);
    CHECK(listed_c.archive.archive_uuid == c->archive_uuid && !listed_c.header && listed_c.unusable_manifest &&
          listed_c.unusable_manifest->kind == va::UnusableManifest::Kind::Changed &&
          va::failure_class_of(*listed_c.unusable_manifest) == "ManifestHashMismatch" &&
          va::plain_message(*listed_c.unusable_manifest) ==
              "the manifest of revision 1 (manifests/" + c->archive_uuid +
                  "/r0001.vam) has changed: its first line differs from what the repository recorded" +
                  test::put_back(c->archive_uuid, 1));
    CHECK(!listed_c.last_full && !listed_c.full_overdue);

    const auto now = days_from_now(0);
    const auto shown = va::archive_statuses(fixture.store, *fixture.repository.files, fixture.repository.context.repo,
        va::Policy{}, now, {b->archive_uuid, true});
    CHECK(shown && shown->size() == 1 && shown->front().unusable_manifest &&
          shown->front().unusable_manifest->kind == va::UnusableManifest::Kind::Missing);
    const auto labels = va::archive_statuses(fixture.store, *fixture.repository.files, fixture.repository.context.repo,
        va::Policy{}, now, {std::nullopt, false});
    CHECK(labels && labels->size() == 3 && std::ranges::none_of(*labels, [](const va::ArchiveStatus& status) {
        return status.header.has_value() || status.unusable_manifest.has_value();
    }));
}

// Decision 0028: each kind of manifest that cannot be used has its own code
// in the --json of `archive list` and `archive show` (`manifest_problem.kind`),
// also one of which the index has no record.
void each_manifest_problem_has_its_code()
{
    using Kind = va::UnusableManifest::Kind;
    CHECK(va::kind_code(Kind::Missing) == "missing");
    CHECK(va::kind_code(Kind::Unreadable) == "unreadable");
    CHECK(va::kind_code(Kind::Changed) == "changed");
    CHECK(va::kind_code(Kind::IndexDiffers) == "index_differs");
    CHECK(va::kind_code(Kind::NotIndexed) == "not_indexed");
}

} // namespace

void archive_status_tests()
{
    timestamps_round_trip();
    overdue_follows_policy();
    shared_labels_are_disambiguated();
    a_damaged_archive_never_stops_the_list();
    each_manifest_problem_has_its_code();
}
