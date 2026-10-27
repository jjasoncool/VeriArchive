// `verify --full <root_path|archive_uuid>`: re-read the whole archive and
// compare it with its manifest (M3). The result line always carries its mode
// (R-21) and the report states which guarantee applies (R-02).
#include "cli/commands.h"

#include "app/verify_report.h"
#include "common/utf8.h"
#include "common/uuid.h"
#include "core/engine/verify_engine.h"
#include "platform/file_system.h"
#include "platform/interrupt.h"
#include "platform/media_enumerator.h"

#include <algorithm>
#include <iostream>
#include <vector>

namespace va::cli {
namespace {

// R-02. Translated in text output; --json keeps the English sentence. It names
// no version of the program (decision 0017).
constexpr const char* guarantee =
    "VeriArchive detects accidental corruption and unintended change; tamper evidence "
    "(signed manifests) is not part of this version.";

int result_code(const std::string& result)
{
    if (result == "PASSED") {
        return exit_code::passed;
    }
    if (result == "PASSED_WITH_WARNINGS") {
        return exit_code::passed_with_warnings;
    }
    if (result == "INCOMPLETE") {
        return exit_code::incomplete;
    }
    return exit_code::failed;
}

void print_outcome(const app::Text& text, const VerifyOutcome& outcome)
{
    // R-21: the mode stays on the result line in every language.
    std::cout << text.format("FULL VERIFY {}  {} ({}) revision {}", text(outcome.result), outcome.label,
                     outcome.archive_uuid, outcome.revision);
    if (!outcome.media_read) {
        // Decision 0028: failed on its manifest before the media was looked for.
        std::cout << "\n  " << text("media: not read, as the manifest of this revision cannot be used") << '\n';
    } else {
        std::cout << "\n  " << text.format("root: {}", path_to_utf8(outcome.root))
                  << "\n  " << text.format("identity: {}", text(to_string(outcome.identity.result)))
                  << "\n  "
                  << text.format("coverage: {} of {} registered bytes read and matched", outcome.coverage_bytes,
                         outcome.registered_bytes)
                  << "\n  " << text("items:");
        for (const auto& [status, count] : outcome.counts) {
            std::cout << "  " << text(status) << " " << count;
        }
        std::cout << '\n';
    }
    for (const auto& failure : outcome.failure_classes) {
        std::cout << "  " << text.format("failure: {}", text(failure)) << '\n';
    }
    for (const auto& warning : outcome.warnings) {
        std::cout << "  " << text.format("warning: {}", text(warning)) << '\n';
    }
    for (const auto& note : outcome.notes) {
        std::cout << "  " << text.format("note: {}", text(note)) << '\n';
    }
    for (const auto& item : outcome.items) {
        std::cout << "  " << text(item.status) << "  " << item.path.p
                  << (item.detail.empty() ? "" : "  " + text.parenthesized(text(item.detail)))
                  << '\n';
    }
    if (const auto advice = app::unbaselined_advice(text, outcome, app::ReportedIn::CommandLine)) {
        std::cout << "  " << *advice << '\n';
    }
    if (outcome.result == "INCOMPLETE") {
        std::cout << "  "
                  << text.format("resume with: veriarchive-cli verify --full {} --resume {}",
                         outcome.archive_uuid, outcome.run_id)
                  << std::endl;
    }
    if (!outcome.resumed_from.empty()) {
        std::cout << "  " << text.format("resumed from run: {}", outcome.resumed_from) << std::endl;
    }
    std::cout << "  " << text.format("run: {}", outcome.run_id) << "\n  " << text(guarantee) << '\n';
}

nlohmann::json outcome_json(const VerifyOutcome& outcome)
{
    auto items = nlohmann::json::array();
    for (const auto& item : outcome.items) {
        items.push_back({{"p", item.path.p}, {"status", item.status}, {"detail", item.detail}});
    }
    // Decision 0028: a run that read no media has no root and no identity.
    const auto root = outcome.media_read ? nlohmann::json(path_to_utf8(outcome.root)) : nlohmann::json(nullptr);
    const auto identity = outcome.media_read ? nlohmann::json{{"result", to_string(outcome.identity.result)},
                                                   {"details", outcome.identity.details}}
                                             : nlohmann::json(nullptr);
    return {
        {"run_id", outcome.run_id},
        {"resumed_from", outcome.resumed_from},
        {"archive_uuid", outcome.archive_uuid},
        {"label", outcome.label},
        {"revision", outcome.revision},
        {"mode", "FULL"},
        {"result", outcome.result},
        {"media_read", outcome.media_read},
        {"root", root},
        {"identity", identity},
        {"failure_classes", outcome.failure_classes},
        {"warnings", outcome.warnings},
        {"notes", outcome.notes},
        {"counts", outcome.counts},
        {"items", std::move(items)},
        {"coverage_bytes", outcome.coverage_bytes},
        {"registered_bytes", outcome.registered_bytes},
        {"guarantee", guarantee},
    };
}

} // namespace

int verify_command(const Arguments& arguments)
{
    if (arguments.has("--quick") || arguments.has("--deep")) {
        return report_failure(arguments, {exit_code::usage,
            "QUICK and DEEP verification are not available in this version; use --full"});
    }
    if (const auto problem = check_options(arguments, {"--full", "--strict-metadata"}, {"--io", "--resume"})) {
        return report_failure(arguments, *problem);
    }
    if (arguments.words.size() != 2 || !arguments.has("--full")) {
        return report_failure(arguments, {exit_code::usage,
            "usage: verify --full <root_path|archive_uuid> [--strict-metadata] [--io hdd|ssd] "
            "[--resume RUN_ID]"});
    }

    // Decision 0029, point 7: an archive's uuid; a folder or volume that is
    // there; the start of an archive's uuid (8 characters or more), as the
    // other archive commands take it; otherwise the folder or volume was not
    // found (exit 10), never "not a registered archive".
    VerifyRequest request;
    const auto& word = arguments.words[1];
    std::optional<std::string> absent;  // the word names no root that is there: why, for when it names no archive
    if (is_canonical_uuid(word)) {
        request.archive_uuid = word;
    } else {
        const auto root = app::typed_root(word);
        // Decision 0029: a root on a network share, or one written in the
        // \\?\ form, is refused before anything of it is read.
        if (const auto refusal = app::root_refusal(root)) {
            return report_failure(arguments, {exit_code::usage, *refusal});
        }
        absent = app::root_absent(root, word);
        const bool like_a_uuid = word.size() >= 8 && std::ranges::all_of(word, [](char character) {
            return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f') ||
                   (character >= 'A' && character <= 'F') || character == '-';
        });
        if (absent && !like_a_uuid) {
            return report_failure(arguments, {exit_code::usage, *absent});
        }
        if (!absent) {
            if (const auto problem = app::root_not_taken(root, word)) {
                return report_failure(arguments, {exit_code::usage, *problem});
            }
            request.root = root;
        }
    }
    request.strict_metadata = arguments.has("--strict-metadata");
    request.resume_run = arguments.value("--resume");
    if (const auto io = arguments.value("--io")) {
        if (*io != "hdd" && *io != "ssd") {
            return report_failure(arguments, {exit_code::usage, "--io takes hdd or ssd"});
        }
        request.rotational = *io == "hdd";
    }

    auto session = open_session(arguments, false);
    if (!session) {
        return report_failure(arguments, session.error());
    }
    if (!request.strict_metadata) {
        request.strict_metadata = session->context.config.policy.strict_metadata;
    }
    // R-111: the config's per-device override; --io wins over it.
    request.rotational_override = session->context.config.rotational_override;
    auto store = ArchiveStore::open(session->context.repo);
    if (!store) {
        return report_failure(arguments, {exit_code::repository, store.error().message});
    }
    // The start of an archive's uuid, where no root is there by that name.
    if (absent) {
        const auto archives = store->archives();
        if (!archives) {
            return report_failure(arguments, {exit_code::repository, archives.error().message});
        }
        std::vector<Uuid> named;
        for (const auto& archive : *archives) {
            if (names_archive(archive.archive_uuid, word)) {
                named.push_back(archive.archive_uuid);
            }
        }
        if (named.empty()) {
            return report_failure(arguments, {exit_code::usage, *absent});
        }
        if (named.size() > 1) {
            return report_failure(arguments, {exit_code::usage, "several archives start with " + word});
        }
        request.archive_uuid = named.front();
    }
    const auto archive_files = platform::make_file_system_access();
    const auto repository_files = platform::make_file_system_access();
    const auto media = platform::make_media_enumerator();
    VerifyServices services{*archive_files, *repository_files, *media, *store, session->context.repo,
        session->context.repository_uuid, host_info()};

    CancelToken cancel;
    platform::cancel_on_interrupt(cancel);
    CliProgress progress(arguments);
    const auto outcome = verify_full(services, request, cancel, progress);
    progress.finish();
    if (!outcome) {
        const auto code = outcome.error().kind == VerifyStop::Kind::Decision ? exit_code::identity_decision
                                                                             : exit_code::repository;
        return report_failure(arguments, {code, outcome.error().message});
    }
    if (arguments.json) {
        print_json(outcome_json(*outcome));
    } else {
        print_outcome(arguments.text, *outcome);
    }
    return result_code(outcome->result);
}

} // namespace va::cli
