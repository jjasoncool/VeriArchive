#include "test_support.h"

#include "app/local_time.h"
#include "app/session.h"
#include "common/utf8.h"
#include "platform/platform_services.h"

#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <nlohmann/json.hpp>
#include <optional>
#include <sqlite3.h>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace std::chrono_literals;

// The sessions of these tests run as an account that is not elevated, whoever
// runs them: GitLab's Linux runners run as root, and a session of an elevated
// process warns of it before all else (R-12), which these texts do not hold.
bool not_elevated()
{
    return false;
}

// A folder of its own in the temporary directory for a sample repository and
// its config file (sample data), removed afterwards.
class SampleFolder {
public:
    explicit SampleFolder(std::string_view test_name)
    {
        const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
                ("va-session-" + std::string(test_name) + '-' + std::to_string(unique));
        std::filesystem::create_directories(path_);
    }

    ~SampleFolder()
    {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    SampleFolder(const SampleFolder&) = delete;
    SampleFolder& operator=(const SampleFolder&) = delete;

    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

// Runs `sql` on the database of the repository at `root`, through a
// connection of its own.
void run_sql(const std::filesystem::path& root, const char* sql)
{
    sqlite3* database = nullptr;
    const auto name = (root / "veriarchive.db").u8string();
    CHECK(sqlite3_open(reinterpret_cast<const char*>(name.c_str()), &database) == SQLITE_OK);
    CHECK(sqlite3_exec(database, sql, nullptr, nullptr, nullptr) == SQLITE_OK);
    sqlite3_close(database);
}

// A sample repository and its config file in `folder`, created as `repo init
// --repository` creates one, its database then lowered to schema version 1,
// as V1 left its databases. Returns the options that open it at `now`.
std::optional<va::app::SessionOptions> repository_of_version_1(
    const SampleFolder& folder, std::chrono::sys_seconds now)
{
    const auto repository = folder.path() / "SAMPLE-repository";
    const auto config = folder.path() / "config.json";
    std::ofstream(config) << "{}\n";
    const auto created = va::app::open_session({true, repository, config, not_elevated, false, now});
    CHECK(created.has_value());
    if (!created) {
        return std::nullopt;
    }
    run_sql(repository, "DROP TABLE verify_progress_chunks; UPDATE schema_version SET version = 1");
    return va::app::SessionOptions{false, repository, config, not_elevated, false, now};
}

// A moment at which the local date here is not the UTC date, where there is
// one in a day: the clock is not at UTC. Otherwise noon, whose local date is
// its UTC date. On a day long past, so that it is never the day the tests
// run on, whose date a session opened at the clock's moment would give.
std::chrono::sys_seconds moment_of_another_date()
{
    const auto day = std::chrono::sys_days(2025y / 1 / 15);
    for (auto hour = 0h; hour < 24h; ++hour) {
        const std::chrono::sys_seconds moment = day + hour;
        if (va::app::local_date(moment, va::platform::utc_offset(moment)) != std::chrono::year_month_day{day}) {
            return moment;
        }
    }
    return day + 12h;
}

// The copy of a database kept before its upgrade (decision 0032) is named for
// the date that the clock here shows at the moment the repository is opened,
// as people read it, not for the UTC date: opened at a moment when the two
// differ, the copy and the note that says where it is name the local date.
void the_copy_before_an_upgrade_is_named_for_the_local_date()
{
    SampleFolder folder("local-date");
    const auto moment = moment_of_another_date();
    const auto options = repository_of_version_1(folder, moment);
    if (!options) {
        return;
    }
    const auto date = va::app::local_date(moment, va::platform::utc_offset(moment));
    const auto copy = options->repository->lexically_normal() /
                      std::format("veriarchive-schema-1-before-upgrade-{:%F}.db", date);
    const auto opened = va::app::open_session(*options);
    CHECK(opened && opened->notes == std::vector<std::string>{va::app::schema_upgrade_note({1, 3, copy})});
    if (opened && opened->notes.size() == 1 && opened->notes.front() != va::app::schema_upgrade_note({1, 3, copy})) {
        std::cerr << "  noted: " << opened->notes.front() << '\n';
    }
    CHECK(std::filesystem::exists(copy));
}

// An open that fails after it upgraded the database, as on a config file
// that it cannot write when it records the pinned repository's uuid (decision
// 0037; here a sample config file naming another uuid, which cannot be
// replaced: read only, in a read-only folder), still says where the copy from
// before the upgrade is, with why it failed (decision 0032): the database
// stays upgraded, and no later open says it again, as the next one, which
// fails the same way, shows. Where the config file can be written all the
// same, as for an account that may write anything, the check is left out.
void a_failed_open_still_says_where_the_copy_is()
{
    SampleFolder folder("failed-open");
    const auto moment = moment_of_another_date();
    auto options = repository_of_version_1(folder, moment);
    if (!options) {
        return;
    }
    // The repository through its pin, which the init wrote into the config
    // file, and a record of another uuid there (sample).
    const auto repository = *options->repository;
    options->repository.reset();
    const auto config = *options->config;
    const auto pinned = nlohmann::json{{"repository", va::path_to_utf8(repository)},
        {"last_repository_uuid", "c0ffee00-0000-4000-8000-000000000099"}};
    std::ofstream(config, std::ios::binary | std::ios::trunc) << pinned.dump();
    const auto read_only = std::filesystem::perms::owner_read | std::filesystem::perms::group_read |
                           std::filesystem::perms::others_read;
    std::filesystem::permissions(config, read_only);
    std::filesystem::permissions(folder.path(), read_only | std::filesystem::perms::owner_exec |
                                                    std::filesystem::perms::group_exec |
                                                    std::filesystem::perms::others_exec);
    const auto writable_again = [&] {
        std::filesystem::permissions(folder.path(), std::filesystem::perms::owner_all);
        std::filesystem::permissions(config, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
    };
    const auto copy = repository.lexically_normal() /
                      std::format("veriarchive-schema-1-before-upgrade-{:%F}.db",
                          va::app::local_date(moment, va::platform::utc_offset(moment)));
    const auto failed = va::app::open_session(*options);
    if (failed) {
        writable_again();
        std::cerr << "  the config file could be written: the open that fails after its upgrade is not checked\n";
        return;
    }
    CHECK(failed.error().message.starts_with("cannot record repository_uuid in config: ") &&
          failed.error().notes == std::vector<std::string>{va::app::schema_upgrade_note({1, 3, copy})});
    CHECK(std::filesystem::exists(copy));
    CHECK(va::app::session_failure_in_window(va::app::Text{}, failed.error()) ==
          failed.error().message + "\n\nNote: " + va::app::schema_upgrade_note({1, 3, copy}) +
              "\n\nCopy of the database from before an upgrade: " + va::path_to_utf8(copy) +
              " (an earlier version of VeriArchive can open only this copy; you can delete it once you no longer "
              "need that version)");
    const auto again = va::app::open_session(*options);
    CHECK(!again && again.error().message.starts_with("cannot record repository_uuid in config: ") &&
          again.error().notes.empty());
    writable_again();
}

// What the line of a copy of the database kept before an upgrade says the copy
// is for, after its path (decision 0032), in English and in Traditional
// Chinese.
const std::string copy_purpose = " (an earlier version of VeriArchive can open only this copy; you can delete it "
                                 "once you no longer need that version)";
const std::string copy_purpose_chinese = "（較早版本的 VeriArchive 只能開啟這份副本；不再需要那個版本時，就可以刪除它）";

// Settings and `repo info` say where the copy from before an upgrade is for as
// long as it is in RepositoryRoot (decision 0032), not only once, at the open
// that made it, whose note says so too, and what the copy is for: an earlier
// version can open only it, and it can go once that version is no longer
// needed. In English and in Traditional Chinese, after that open and after
// the next one, which says nothing of an upgrade; and nothing once the copy
// is removed.
void settings_and_repo_info_say_where_the_copy_is()
{
    SampleFolder folder("copy-lines");
    const auto moment = moment_of_another_date();
    const auto options = repository_of_version_1(folder, moment);
    if (!options) {
        return;
    }
    const auto opened = va::app::open_session(*options);
    CHECK(opened && opened->notes.size() == 1);
    if (!opened) {
        return;
    }
    const auto& layout = opened->context.repo;
    const auto copy = layout.root / std::format("veriarchive-schema-1-before-upgrade-{:%F}.db",
                                        va::app::local_date(moment, va::platform::utc_offset(moment)));
    const va::app::Text english;
    const va::app::Text chinese(va::app::Language::TraditionalChinese);
    const std::vector<std::string> in_english{
        "Copy of the database from before an upgrade: " + va::path_to_utf8(copy) + copy_purpose};
    CHECK(va::app::copy_before_upgrade_lines(english, layout) == in_english);
    CHECK(va::app::copy_before_upgrade_lines(chinese, layout) ==
          std::vector<std::string>{"升級前的資料庫副本：" + va::path_to_utf8(copy) + copy_purpose_chinese});
    const auto again = va::app::open_session(*options);
    CHECK(again && again->notes.empty());
    CHECK(again && va::app::copy_before_upgrade_lines(english, again->context.repo) == in_english);
    std::filesystem::remove(copy);
    CHECK(va::app::copy_before_upgrade_lines(english, layout).empty());
}

// Settings and `repo info` name the copies of the database kept before an
// upgrade also where the repository cannot be opened (decision 0032), as there
// a copy is the way back: the failure carries the RepositoryRoot that the open
// resolved, and a window gives why and a line per copy there, read from the
// files each time it is worded. As on a database upgraded once, its copy kept,
// and found damaged since (a table dropped: sample damage), which the open
// refuses without upgrading or copying anything (decision 0036), in English
// and in Traditional Chinese, and once the copy is removed; and on a database
// that a later version upgraded (its schema version raised: sample data),
// which this version refuses, beside the copy that the later version kept (a
// sample file of its name), which this version can open. An open that
// resolves no RepositoryRoot carries none, and a window gives why alone.
void a_failed_open_names_the_copies_where_the_repository_is()
{
    SampleFolder folder("failed-open-copies");
    const auto moment = moment_of_another_date();
    const auto options = repository_of_version_1(folder, moment);
    if (!options) {
        return;
    }
    const auto root = options->repository->lexically_normal();
    const auto copy = root / std::format("veriarchive-schema-1-before-upgrade-{:%F}.db",
                                 va::app::local_date(moment, va::platform::utc_offset(moment)));
    const auto copy_line = "Copy of the database from before an upgrade: " + va::path_to_utf8(copy) + copy_purpose;
    const va::app::Text english;
    const va::app::Text chinese(va::app::Language::TraditionalChinese);

    const auto upgraded = va::app::open_session(*options);
    CHECK(upgraded && upgraded->notes == std::vector<std::string>{va::app::schema_upgrade_note({1, 3, copy})});
    run_sql(*options->repository, "DROP TABLE trusted_keys");
    const auto damaged = "the repository's database " + va::path_to_utf8(root / "veriarchive.db") +
                         " is damaged; make it again from the manifests with `repo rebuild-index`";
    const auto again = va::app::open_session(*options);
    CHECK(!again && again.error().message == damaged && again.error().notes.empty() && again.error().repository &&
          again.error().repository->root == root);
    if (again) {
        return;
    }
    const auto in_english = va::app::session_failure_in_window(english, again.error());
    CHECK(in_english == damaged + "\n\n" + copy_line);
    if (in_english != damaged + "\n\n" + copy_line) {
        std::cerr << "  worded: " << in_english << '\n';
    }
    CHECK(va::app::session_failure_in_window(chinese, again.error()) ==
          "儲存庫的資料庫 " + va::path_to_utf8(root / "veriarchive.db") +
              " 已損毀；請以 `repo rebuild-index` 從清單檔重新建立\n\n升級前的資料庫副本：" + va::path_to_utf8(copy) +
              copy_purpose_chinese);
    std::filesystem::remove(copy);
    CHECK(va::app::session_failure_in_window(english, again.error()) == damaged);

    SampleFolder later_folder("later-version");
    const auto later = later_folder.path() / "SAMPLE-repository";
    const auto config = later_folder.path() / "config.json";
    std::ofstream(config) << "{}\n";
    CHECK(va::app::open_session({true, later, config, not_elevated, false, moment}).has_value());
    run_sql(later, "UPDATE schema_version SET version = 4");
    const auto later_copy = later.lexically_normal() / "veriarchive-schema-3-before-upgrade-2026-10-17.db";
    std::ofstream(later_copy, std::ios::binary) << "SAMPLE: the copy a later version kept (sample data)\n";
    const auto refused = va::app::open_session({false, later, config, not_elevated, false, moment});
    CHECK(!refused && refused.error().message == "unsupported repository schema" &&
          refused.error().repository && refused.error().repository->root == later.lexically_normal());
    CHECK(!refused && va::app::session_failure_in_window(english, refused.error()) ==
                          "unsupported repository schema\n\nCopy of the database from before an upgrade: " +
                              va::path_to_utf8(later_copy) + copy_purpose);

    auto elsewhere = *options;
    elsewhere.repository = folder.path() / "SAMPLE-no-repository";
    const auto unresolved = va::app::open_session(elsewhere);
    CHECK(!unresolved && !unresolved.error().repository);
    CHECK(!unresolved && va::app::session_failure_in_window(english, unresolved.error()) ==
                             "the repository " + va::path_to_utf8(elsewhere.repository->lexically_normal()) +
                                 " is unavailable or is not a VeriArchive repository");
}

// Whether a session's open will upgrade the database of the repository it
// opens is known ahead, for the repository that open resolves (decision
// 0032): the GUI shows its window with a notice before such an open. The
// version of a repository of version 1, and nothing once it is upgraded, nor
// for a repository that cannot be resolved, whose open says why.
void an_upgrade_is_known_ahead_of_the_open()
{
    SampleFolder folder("upgrade-ahead");
    const auto options = repository_of_version_1(folder, moment_of_another_date());
    if (!options) {
        return;
    }
    CHECK(va::app::schema_upgrade_ahead(*options) == 1);
    const auto opened = va::app::open_session(*options);
    CHECK(opened && opened->notes.size() == 1);
    CHECK(!va::app::schema_upgrade_ahead(*options));

    auto elsewhere = *options;
    elsewhere.repository = folder.path() / "SAMPLE-no-repository";
    CHECK(!va::app::schema_upgrade_ahead(elsewhere) && !va::app::open_session(elsewhere));
}

} // namespace

void session_tests()
{
    the_copy_before_an_upgrade_is_named_for_the_local_date();
    a_failed_open_still_says_where_the_copy_is();
    settings_and_repo_info_say_where_the_copy_is();
    a_failed_open_names_the_copies_where_the_repository_is();
    an_upgrade_is_known_ahead_of_the_open();
}
