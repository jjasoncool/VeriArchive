#include "test_support.h"

#include "engine_fixture.h"

#include "common/time.h"
#include "core/engine/archive_status.h"

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

} // namespace

void archive_status_tests()
{
    timestamps_round_trip();
    overdue_follows_policy();
    shared_labels_are_disambiguated();
}
