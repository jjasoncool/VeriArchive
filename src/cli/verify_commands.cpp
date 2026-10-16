// `verify --full <root_path|archive_uuid>`: re-read the whole archive and
// compare it with its manifest (M3). The result line always carries its mode
// (R-21) and the report states which guarantee applies (R-02).
#include "cli/commands.h"

#include "common/utf8.h"
#include "common/uuid.h"
#include "core/engine/verify_engine.h"
#include "platform/file_system.h"
#include "platform/interrupt.h"
#include "platform/media_enumerator.h"

#include <iostream>

namespace va::cli {
namespace {

// R-02. Translated in text output; --json keeps the English sentence.
constexpr const char* guarantee =
    "V1 detects accidental corruption and unintended change; tamper evidence "
    "(signed manifests) begins in V1.1.";

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
                     outcome.archive_uuid, outcome.revision)
              << "\n  " << text.format("root: {}", path_to_utf8(outcome.root))
              << "\n  " << text.format("identity: {}", text(to_string(outcome.identity.result)))
              << "\n  "
              << text.format("coverage: {} of {} registered bytes read and matched", outcome.coverage_bytes,
                     outcome.registered_bytes)
              << "\n  " << text("items:");
    for (const auto& [status, count] : outcome.counts) {
        std::cout << "  " << text(status) << " " << count;
    }
    std::cout << '\n';
    for (const auto& failure : outcome.failure_classes) {
        std::cout << "  " << text.format("failure: {}", text(failure)) << '\n';
    }
    for (const auto& warning : outcome.warnings) {
        std::cout << "  " << text.format("warning: {}", text(warning)) << '\n';
    }
    for (const auto& item : outcome.items) {
        std::cout << "  " << text(item.status) << "  " << item.path.p
                  << (item.detail.empty() ? "" : "  " + text.parenthesized(text(item.detail)))
                  << '\n';
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
    return {
        {"run_id", outcome.run_id},
        {"resumed_from", outcome.resumed_from},
        {"archive_uuid", outcome.archive_uuid},
        {"label", outcome.label},
        {"revision", outcome.revision},
        {"mode", "FULL"},
        {"result", outcome.result},
        {"root", path_to_utf8(outcome.root)},
        {"identity", {{"result", to_string(outcome.identity.result)}, {"details", outcome.identity.details}}},
        {"failure_classes", outcome.failure_classes},
        {"warnings", outcome.warnings},
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
            "QUICK and DEEP verification arrive in V1.1; use --full"});
    }
    if (const auto problem = check_options(arguments, {"--full", "--strict-metadata"}, {"--io", "--resume"})) {
        return report_failure(arguments, *problem);
    }
    if (arguments.words.size() != 2 || !arguments.has("--full")) {
        return report_failure(arguments, {exit_code::usage,
            "usage: verify --full <root_path|archive_uuid> [--strict-metadata] [--io hdd|ssd] "
            "[--resume RUN_ID]"});
    }

    VerifyRequest request;
    if (is_canonical_uuid(arguments.words[1])) {
        request.archive_uuid = arguments.words[1];
    } else {
        request.root = root_path(arguments.words[1]);
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
    auto store = ArchiveStore::open(session->context.repo);
    if (!store) {
        return report_failure(arguments, {exit_code::repository, store.error().message});
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
