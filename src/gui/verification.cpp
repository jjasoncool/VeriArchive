// FULL verification (§B17, decision 0046), started from an archive's details
// in Archives: of the archive, found by its uuid on the attached volumes, in
// full, in full adding chunk hashes (decision 0033) or with strict metadata;
// or of the folder or volume the operator types or chooses, told by its
// identity, with strict metadata or not. Each runs as a job (decision 0045),
// shown as a card in the details that started it, whose Stop stops it, and
// none starts while the archive has one that waits or works; once it ended, a
// toast says its result, with View result, the result page of the run it
// recorded, or, where it recorded none, Details, its report. The result always carries
// its mode (R-21) and the V1 guarantee (R-02). A stopped verification is
// resumed from the unfinished operations or the archive's details.
//
// Decision 0047: FULL verify of an archive whose device the latest read of
// the attached devices sees behind another enclosure asks first whether it is
// the same device (enclosure_dialog.cpp), and goes on with the carrier
// confirmation given there; its details say meanwhile that only a pass
// records the new enclosure, and its toast what it recorded. A verification
// that stops MIGRATED there anyway (the read was older than the change) says
// to verify it from the details, which ask; one that stops MIGRATED anywhere
// else names the relocation (decision 0046); every other refusal of the
// engine, those of a confirmation included, is said in its own words.
#include "gui/gui_app.h"

#include "app/device_alerts.h"
#include "app/enclosure_words.h"
#include "app/plain_words.h"
#include "app/typed_path.h"
#include "app/verify_report.h"
#include "common/utf8.h"
#include "core/engine/add_chunk_hashes.h"
#include "core/engine/attached.h"
#include "core/engine/candidates.h"
#include "core/engine/carrier_confirmation.h"
#include "core/engine/verify_engine.h"
#include "gui/archive_rows.h"
#include "gui/smart_rows.h"
#include "platform/file_system.h"
#include "platform/media_enumerator.h"
#include "platform/process_table.h"

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace va::gui {
namespace {

std::string report(const app::Text& text, const VerifyOutcome& outcome)
{
    std::string body = text.format("FULL VERIFY {}", text(outcome.result)) + "\n\n" +
                       text.format("{} ({}) revision {}", outcome.label, outcome.archive_uuid, outcome.revision) +
                       "\n";
    // Decision 0044: a device alert comes right below the archive, beside the
    // result, which it does not change.
    for (const auto& line : app::health_alert_lines(text, outcome.devices)) {
        body += line + "\n";
    }
    if (outcome.read_compared) {
        if (const auto line = app::slower_alert_line(text, *outcome.read_compared)) {
            body += *line + "\n";
        }
    }
    if (!outcome.media_read) {
        // Decision 0028: failed on its manifest before the media was looked for.
        body += text("Media: not read, as the manifest of this revision cannot be used") + "\n";
    } else {
        // Decision 0047: past a changed enclosure, how it was confirmed.
        const auto identity = outcome.carrier_confirmed
                                  ? app::confirmed_identity_words(text, outcome.carrier_confirmed->confirmation)
                                  : text(to_string(outcome.identity.result));
        body += text.format("Root: {}", path_to_utf8(outcome.root)) + "\n" + text.format("Identity: {}", identity) +
                "\n";
        for (const auto& device : outcome.devices) {
            body += text.format("Device health: {}", app::device_health_words(text, device)) + "\n";
        }
        body += text.format("Coverage: {} of {} registered bytes read and matched",
                    format_bytes(outcome.coverage_bytes), format_bytes(outcome.registered_bytes)) +
                "\n";
        if (const auto speed = app::read_speed_words(text, outcome.read_speed, outcome.read_compared)) {
            body += text.format("Read speed: {}", *speed) + "\n";
        }
        body += text("Items:");
        for (const auto& [status, count] : outcome.counts) {
            body += "  " + text(status) + " " + std::to_string(count);
        }
        body += "\n";
    }
    for (const auto& failure : outcome.failure_classes) {
        body += text.format("Failure: {}", text(app::failure_class_words(failure))) + "\n";
    }
    for (const auto& warning : outcome.warnings) {
        body += text.format("Warning: {}", text(warning)) + "\n";
    }
    for (const auto& note : outcome.notes) {
        body += text.format("Note: {}", text(note)) + "\n";
    }
    if (!outcome.items.empty()) {
        body += "\n";
        for (const auto& item : outcome.items) {
            body += text(item.status) + "  " + item.path.p +
                    (item.detail.empty() ? "" : "  " + text.parenthesized(text(app::item_detail_words(item.detail)))) +
                    "\n";
        }
    }
    // Decision 0033: the revision that records the chunk hashes added, or why
    // there is none.
    if (const auto line = app::chunk_hashes_line(text, outcome, true); !line.empty()) {
        body += "\n" + line + "\n";
    }
    // Decision 0047: the revision that records the new enclosure, or why
    // there is none.
    if (const auto line = app::carrier_line(text, outcome, true); !line.empty()) {
        body += "\n" + line + "\n";
    }
    if (const auto advice = app::unbaselined_advice(text, outcome, app::ReportedIn::Window)) {
        body += "\n" + *advice + "\n";
    }
    if (outcome.result == "INCOMPLETE") {
        body += "\n" +
                text("The run was interrupted; continue it with Resume in the archive's details in Archives, without "
                     "reading again the files verified.") +
                "\n";
    }
    body += "\n";
    if (!outcome.resumed_from.empty()) {
        body += text.format("Resumed from run: {}", outcome.resumed_from) + "\n";
    }
    body += text.format("Run: {}", outcome.run_id) + "\n" + text(guarantee) + "\n";
    return body;
}

// The archive as a job's title and its toast name it: as the list shows it
// (display_label: its label, with the start of its uuid where another archive
// shares the label, R-88), or by the start of its uuid.
std::string archive_name(const GuiState& state, const Uuid& archive_uuid)
{
    for (const auto& status : state.archives_page.statuses) {
        if (status.archive.archive_uuid == archive_uuid && !status.archive.label.empty()) {
            return display_label(status);
        }
    }
    if (state.session) {
        if (auto store = ArchiveStore::open_read_only(state.session->context.repo)) {
            if (const auto archive = store->find_archive(archive_uuid); archive && *archive &&
                                                                        !(*archive)->label.empty()) {
                return (*archive)->label;
            }
        }
    }
    return archive_uuid.substr(0, 8);
}

// Decision 0047: where a verification of `archive` stopped MIGRATED, whether
// its device is seen behind another enclosure now (enclosure_case_of), as
// identity matching finds the archive's latest carrier on the volumes
// attached now; the volume's mount point, if it is. Asked on the job's
// thread; nothing where it cannot be told.
std::optional<std::string> enclosure_seen(ArchiveStore& store, IFileSystemAccess& repository_files,
    const RepositoryLayout& layout, IMediaEnumerator& media, const Uuid& archive)
{
    const auto pool = load_candidate_pool(store, repository_files, layout, archive);
    if (!pool) {
        return std::nullopt;
    }
    const auto candidate = std::ranges::find(pool->candidates, archive, &ArchiveCandidate::archive_uuid);
    if (candidate == pool->candidates.end()) {
        return std::nullopt;
    }
    const auto volumes = media.volumes();
    if (!volumes) {
        return std::nullopt;
    }
    const auto found = where_attached(*volumes, std::span(&*candidate, 1));
    if (found.archives.empty()) {
        return std::nullopt;
    }
    const auto seen = enclosure_case_of(found.archives.front(), *volumes, candidate->identity, false);
    return seen ? std::optional(seen->where) : std::nullopt;
}

// Decision 0047: the carrier confirmation that the record of the unfinished
// run `run_id` carries (run:<run_id>, as the engine reads it to resume the
// run); nothing where it carries none, or cannot be read.
std::optional<CarrierConfirmation> confirmation_carried(const GuiState& state, const std::string& run_id)
{
    if (!state.session) {
        return std::nullopt;
    }
    auto store = ArchiveStore::open_read_only(state.session->context.repo);
    if (!store) {
        return std::nullopt;
    }
    const auto record = store->setting("run:" + run_id);
    if (!record || record->empty()) {
        return std::nullopt;
    }
    const auto carried = carrier_confirmed_in_record(nlohmann::json::parse(*record, nullptr, false));
    return carried ? std::optional(carried->confirmation) : std::nullopt;
}

// The request of FULL verify of `archive` by its uuid, as `choice` asks for
// it.
VerifyRequest archive_request(const GuiState& state, const Uuid& archive, VerifyChoice choice)
{
    VerifyRequest request;
    request.archive_uuid = archive;
    // R-51: strict metadata as chosen, as the command line's
    // --strict-metadata, or as the policy sets it.
    request.strict_metadata =
        choice == VerifyChoice::StrictMetadata || state.session->context.config.policy.strict_metadata;
    // R-111: the window has no --io; the config's override by serial applies.
    request.rotational_override = state.session->context.config.rotational_override;
    // Decision 0033: offered only for an archive to which it adds them; the
    // verification checks again.
    request.add_chunk_hashes = choice == VerifyChoice::AddingChunkHashes;
    return request;
}

// Runs `request` as a job (decision 0045), shown as a card in the details of
// `shown_in` until another one is started there. A run left INCOMPLETE is the
// one that Resume in its archive's details continues as the window ran it
// (details_resume).
void run_verification(GuiState& state, VerifyRequest request, const Uuid& shown_in)
{
    // Filled by the worker; read by `done`, which runs only after the worker finished.
    struct Interrupted {
        std::string run_id;
        Uuid archive_uuid;
    };
    auto interrupted = std::make_shared<std::optional<Interrupted>>();
    // What the toast says once the job ended: its result and archive, or
    // nothing where it reached no result. Filled by the worker, worded by
    // `done`, which names the archive as the list does.
    struct Reached {
        std::string result;
        Uuid archive_uuid;
        std::string label;  // as the verification read it
        std::string run_id; // the run it recorded, whose result page the toast opens
        // Decision 0047: what it recorded of the new enclosure, which the
        // toast says (only carrier_* are set).
        VerifyOutcome carrier;
    };
    auto reached = std::make_shared<std::optional<Reached>>();
    // Decision 0047: it stopped MIGRATED behind another enclosure that the
    // latest read of the attached devices had not seen: `done` reads them
    // again, so the details ask first. Set by the worker.
    auto enclosure_unseen = std::make_shared<bool>(false);
    // The worker returns its report unworded; it is worded when shown, in the
    // language in use then.
    JobSpec job;
    job.keeps_progress = true;
    const auto name = request.root ? path_to_utf8(*request.root) : archive_name(state, *request.archive_uuid);
    if (request.resume_run) {
        job.title = [name](const app::Text& text) { return text.format("Resume the FULL verification of {}", name); };
    } else {
        job.title = [name](const app::Text& text) { return text.format("FULL verify {}", name); };
    }
    job.target = {request.root, request.root ? std::nullopt : request.archive_uuid};
    if (request.root) {
        job.devices = [root = *request.root] { return devices_of_root(root); };
    } else {
        job.devices = [layout = state.session->context.repo, archive = *request.archive_uuid] {
            return devices_of_archive(layout, archive);
        };
    }
    job.work = [&state, request, interrupted, reached, enclosure_unseen](JobContext& context) -> Report {
        // Not verified, until a result says otherwise.
        context.tone = JobTone::Error;
        // Decision 0029: a root on a network share, or one written in the
        // \\?\ form, is refused before anything of it is read. Asked here,
        // off the window's thread: on Linux it reads the root's filesystem.
        if (request.root) {
            if (auto refusal = app::root_not_taken(*request.root, path_to_utf8(*request.root))) {
                return [refusal = std::move(*refusal)](const app::Text& text) {
                    return text.format("Not verified: {}", text(refusal));
                };
            }
        }
        auto& session = *state.session;
        auto store = ArchiveStore::open(session.context.repo);
        if (!store) {
            return [message = store.error().message](const app::Text& text) {
                return text.format("Cannot open the repository: {}", text(message));
            };
        }
        const auto archive_files = platform::make_file_system_access();
        const auto repository_files = platform::make_file_system_access();
        const auto media = platform::make_media_enumerator();
        const auto processes = platform::make_process_table();
        VerifyServices services{*archive_files, *repository_files, *media, *store, session.context.repo,
            session.context.repository_uuid, state.host, *session.writer, *processes};
        // Decision 0044: a device alert shows in the job as soon as the
        // devices were asked; the operator may stop it, or let it go on.
        auto asked = request;
        asked.devices_observed = context.devices_observed;
        auto outcome = verify_full(services, asked, context.cancel, context.progress);
        if (!outcome) {
            // Decision 0022, point 7: an archive that more than one attached
            // volume holds. The verification names their roots; the report
            // says how to verify the one meant, in the window.
            if (!request.resume_run && request.archive_uuid && !outcome.error().roots_found.empty()) {
                const auto short_uuid = request.archive_uuid->substr(0, 8);
                std::string label;
                if (const auto archive = store->find_archive(*request.archive_uuid); archive && *archive) {
                    label = (*archive)->label;
                }
                std::string roots;
                for (const auto& root : outcome.error().roots_found) {
                    roots += "\n  " + path_to_utf8(root);
                }
                return [label, short_uuid, roots](const app::Text& text) {
                    const auto archive_name = label.empty() ? short_uuid : label + text.parenthesized(short_uuid);
                    return text.format(
                        "Not verified: archive {} is on more than one attached volume, as a device and a "
                        "sector-by-sector clone of it would be:{}\nVeriArchive cannot tell which one to verify. "
                        "Choose \"Verify a folder or volume...\" in the menu beside \"FULL verify\" in the "
                        "archive's details, and choose the folder of the one to verify; or detach the other "
                        "volumes and verify the archive again.",
                        archive_name, roots);
                };
            }
            // MIGRATED: the filesystem of an archive on another device than
            // recorded, which the window relocates (decision 0046), in place of
            // the command line's `archive relocate`; a retired archive takes
            // no new revisions. Named as its record names it, read here. Only
            // the stop that every MIGRATED without a confirmation makes: a
            // refusal of a carrier confirmation (decision 0047: another
            // archive's carrier there, the serial of another device, a retired
            // archive, chunk hashes asked for, enclosures that changed since)
            // carries the identity too, and is said in its own words below.
            if (const auto& stop = outcome.error(); stop.kind == VerifyStop::Kind::Decision &&
                                                    stop.identity.result == MatchResult::Migrated &&
                                                    !stop.identity.archives.empty() &&
                                                    stop.message.starts_with("MIGRATED:")) {
                const auto& migrated = stop.identity.archives.front();
                std::string label = migrated.substr(0, 8);
                bool retired = false;
                if (const auto archive = store->find_archive(migrated); archive && *archive) {
                    if (!(*archive)->label.empty()) {
                        label = (*archive)->label;
                    }
                    retired = (*archive)->status == "RETIRED";
                }
                // Decision 0047: behind another enclosure, which the latest
                // read of the attached devices had not seen yet, or verified
                // by its folder: FULL verify of its details asks first.
                if (!retired) {
                    if (auto where = enclosure_seen(
                            *store, *repository_files, session.context.repo, *media, migrated)) {
                        *enclosure_unseen = true;
                        return [where = std::move(*where)](const app::Text& text) {
                            return enclosure_stop_words(text, where);
                        };
                    }
                }
                return [label, retired](const app::Text& text) {
                    const auto found = text.format("Not verified: the filesystem of this folder or volume is that of "
                                                   "archive {}, but on another device than the one recorded "
                                                   "(MIGRATED).",
                        label);
                    return found + "\n" +
                           (retired ? text("This archive is retired and takes no new revisions, so it is not "
                                           "relocated.")
                                    : text("If it is that archive's device in another enclosure, or you moved the "
                                           "archive to that device on purpose, relocate the archive to it with "
                                           "Relocate to a new carrier... in the menu of its details."));
                };
            }
            // Refused while another operation that creates the archive's
            // next revision is unfinished, it names the window's own ways
            // on, or says that that operation still runs, judged now as
            // Archives judges its row (decision 0033).
            const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
            return [message = app::stop_message_in_window(
                        outcome.error(), *store, *processes, state.host.host, now)](const app::Text& text) {
                return text.format("Not verified: {}", text(message));
            };
        }
        // The devices as the views name them: the sample identity in the
        // screenshot mode, whose pictures are published.
        outcome->devices = displayed_devices(outcome->devices, state.identity_shown);
        context.tone = verification_tone(outcome->result);
        // A verification that returns an outcome has recorded its run, an
        // INCOMPLETE one and one that failed on its manifest included.
        *reached = Reached{outcome->result, outcome->archive_uuid, outcome->label, outcome->run_id, {}};
        (*reached)->carrier.carrier_confirmed = outcome->carrier_confirmed;
        (*reached)->carrier.carrier_revision = outcome->carrier_revision;
        (*reached)->carrier.carrier_not_recorded = outcome->carrier_not_recorded;
        if (outcome->result == "INCOMPLETE") {
            *interrupted = Interrupted{outcome->run_id, outcome->archive_uuid};
        } else if (request.resume_run && !outcome->media_read) {
            // Decision 0028: a resume that failed on the manifest left the
            // run it was to resume as it was, so Resume stays offered for
            // that run, for once the manifest is restored.
            *interrupted = Interrupted{*request.resume_run, outcome->archive_uuid};
        }
        return [outcome = std::move(*outcome)](const app::Text& text) { return report(text, outcome); };
    };
    job.done = [&state, request, interrupted, reached, enclosure_unseen, name](std::uint64_t ended, const Report&) {
        guarded(
            [&] {
                // Its details no longer say that it goes on with a confirmation.
                state.confirmed_verifications.erase(ended);
                // Decision 0047: a pass that recorded the new carrier, whose
                // confirmation named the device read, makes that device the
                // archive's confirmed device (Devices says so of its read);
                // until the attached devices are read again, which they are
                // at once, the details say what it recorded in place of the
                // question, which the read before (of the earlier carrier)
                // would still ask.
                const bool carrier_recorded = *reached && (*reached)->carrier.carrier_revision > 0;
                if (carrier_recorded) {
                    const auto& found = **reached;
                    if (found.carrier.carrier_confirmed && found.carrier.carrier_confirmed->confirmation.device) {
                        note_confirmed(state.device_reads, *found.carrier.carrier_confirmed->confirmation.device,
                            found.archive_uuid);
                    }
                    state.carriers_recorded[found.archive_uuid] =
                        GuiState::CarrierRecorded{found.carrier, std::chrono::steady_clock::now()};
                }
                if (*interrupted) {
                    state.interrupted_run = InterruptedRun{
                        (*interrupted)->run_id, request.root, (*interrupted)->archive_uuid, request.strict_metadata};
                } else if (request.resume_run && state.interrupted_run &&
                           *request.resume_run == state.interrupted_run->run_id) {
                    state.interrupted_run.reset();
                }
                // The run it recorded, which View result on its rows in Jobs
                // and in its details opens; the rows are made again once
                // `done` returns, as the job ended.
                if (*reached && !(*reached)->run_id.empty()) {
                    state.job_runs[ended] = {(*reached)->run_id, (*reached)->archive_uuid};
                }
                // Its new result, its revision with chunk hashes, or the
                // verification that stopped, at once; in History too.
                refresh_archives(state);
                refresh_history(state);
                start_poll(state);
                if (*enclosure_unseen || carrier_recorded) {
                    read_devices(state, DevicesReadWhy::Refresh);
                }
                // The archive as the list names it: by its uuid as the job's
                // title does, by the root's archive once the verification
                // found it; or, without a result, what the job verified.
                // View result opens the result page of the run it recorded;
                // a job that recorded none has its report in Details.
                if (*reached && !(*reached)->run_id.empty()) {
                    const auto& found = **reached;
                    const auto archive = !request.root          ? name
                                         : found.archive_uuid.empty() ? found.label
                                                                      : archive_name(state, found.archive_uuid);
                    // Decision 0047: and what it recorded of the new
                    // enclosure, or that it recorded none.
                    show_toast(state,
                        confirmed_toast(state.text, verified_toast(state.text, archive.empty() ? name : archive,
                                                        found.result),
                            found.carrier),
                        ToastAction{state.text("View result"),
                            [run = found.run_id, uuid = found.archive_uuid](GuiState& state) {
                                // Over Archives, the details of its archive behind it.
                                open_result_of(state, uuid, run);
                            }});
                } else {
                    show_toast(state, not_verified_toast(state.text, name),
                        ToastAction{state.text("Details"), [ended](GuiState& state) { open_job_details(state, ended); }});
                }
            },
            [&](const Report& error) { show_archives_status(state, error); });
    };
    const auto id = state.jobs.start(state.text, std::move(job));
    state.verification_jobs[shown_in] = id;
    if (request.carrier_confirmation) {
        state.confirmed_verifications[id] = *request.carrier_confirmation;
    } else if (const auto carried = request.resume_run ? confirmation_carried(state, *request.resume_run)
                                                       : std::nullopt) {
        // Decision 0047: a resume goes on with the confirmation its run's
        // record carries (as the engine reads it); its details say so as for
        // the first attempt.
        state.confirmed_verifications[id] = *carried;
    }
    job_changed(state, id);
    // The details that started it show it.
    if (state.archive_selected == shown_in) {
        show_details(state);
    }
}

// What the dialog of "Verify a folder or volume..." answers into: the archive
// whose details opened it, and its rows, which the dialog owns and which are
// read when it answers.
struct RootChoice {
    GuiState* state;
    Uuid shown_in;
    GtkWidget* folder;  // AdwEntryRow
    GtkWidget* strict;  // AdwSwitchRow
};

// The root that the current revision of `archive` recorded, as the list read
// it; empty where it is not known.
std::string recorded_root(const GuiState& state, const Uuid& archive)
{
    for (const auto& status : state.archives_page.statuses) {
        if (status.archive.archive_uuid == archive && status.header) {
            return status.header->root_hint;
        }
    }
    return {};
}

} // namespace

bool refused_while_the_archive_has_a_job(GuiState& state, const Uuid& archive, const std::string& root)
{
    const auto busy = state.jobs.reads(root.empty() ? std::nullopt : std::optional(path_from_utf8(root)),
        archive.empty() ? std::nullopt : std::optional(archive));
    if (busy) {
        show_message(state, [label = archive_name(state, archive)](const app::Text& text) {
            return archive_has_a_job(text, label);
        });
    }
    return busy;
}

namespace {

// Decision 0052: how long the SMART reads of a run of the device identity
// reader serve a FULL verification started after it without asking Windows
// again (as one started from the enclosure question, which read them).
constexpr auto recent_smart_serves = std::chrono::minutes(5);

// A verification to start, and the archive whose details show it.
struct ToStart {
    VerifyRequest request;
    Uuid shown_in;
};

// Whether a FULL verification started now reads SMART first: the operator
// turned it on in Settings, and this computer has a device identity reader.
bool reads_smart_first(const GuiState& state)
{
    return state.session && state.session->context.config.read_smart_with_full_verify && state.device_identity &&
           state.device_identity->available();
}

// Starts each of `runs` (run_verification), after reading the SMART of the
// attached devices where reads_smart_first (decision 0052): each run records
// the reads of the devices of the volume it reads. The reads of a run of the
// reader that ended a few minutes ago serve; otherwise the reader reads every
// attached device, once for all of `runs`, after Windows asked the operator.
// Where nothing is read (declined, no reader, another read waiting, nothing
// attached), they start without SMART, and a toast says why.
void start_verifications(GuiState& state, std::vector<ToStart> runs)
{
    const auto start = [](GuiState& state, std::vector<ToStart>& runs, const std::vector<SmartRead>& reads) {
        for (auto& run : runs) {
            run.request.smart_reads = reads;
            run_verification(state, std::move(run.request), run.shown_in);
        }
    };
    if (runs.empty()) {
        return;
    }
    if (!reads_smart_first(state)) {
        start(state, runs, {});
        return;
    }
    if (!state.recent_smart.empty() && std::chrono::steady_clock::now() - state.recent_smart_at < recent_smart_serves) {
        start(state, runs, state.recent_smart);
        return;
    }
    std::vector<PhysicalDeviceInfo> devices;
    if (const auto* read = attached_now(state)) {
        for (const auto& attached : read->devices) {
            if (!attached.device.device_ref.empty()) {
                devices.push_back(attached.device);
            }
        }
    }
    const auto without = [](GuiState& state, std::vector<ToStart>& runs, const std::string& why) {
        show_toast(state, state.text.format("Started without SMART: {}", why));
        for (auto& run : runs) {
            run_verification(state, std::move(run.request), run.shown_in);
        }
    };
    if (devices.empty()) {
        without(state, runs, state.text("No attached device whose SMART can be read."));
        return;
    }
    if (state.device_reading) {
        without(state, runs,
            state.text("another read with administrator permission is waiting for the User Account Control prompt "
                       "or for its devices"));
        return;
    }
    auto pending = std::make_shared<std::vector<ToStart>>(std::move(runs));
    const bool started = run_reader(state, std::move(devices), "*",
        [pending, start, without](GuiState& state, ReaderRun& run) {
            if (run.failure) {
                show_message(state, run.failure);
                without(state, *pending, state.text("the device identity reader failed"));
                return;
            }
            if (!run.reads) {
                // Cancelled: the window is closing, and starts nothing.
                if (const auto words = smart_run_words(state.text, run.reads, state.smart_reads)) {
                    without(state, *pending, *words);
                }
                return;
            }
            std::vector<SmartRead> reads;
            for (const auto& read : *run.reads) {
                if (read.smart) {
                    reads.push_back(*read.smart);
                }
            }
            start(state, *pending, reads);
        });
    if (!started) {
        without(state, *pending, state.text("the device identity reader could not be started"));
    }
}

// The request of FULL verify of `archive` by its uuid as `choice` asks for
// it, or nothing where it is not started now: a job of the window works on
// the archive (a message says so), or its device is seen behind another
// enclosure, where the enclosure question opens first (decision 0047).
std::optional<VerifyRequest> archive_verification(GuiState& state, const Uuid& archive, VerifyChoice choice)
{
    if (refused_while_the_archive_has_a_job(state, archive, recorded_root(state, archive))) {
        return std::nullopt;
    }
    // Decision 0047: its device seen behind another enclosure, as the latest
    // read of the attached devices found it: the question first. The
    // verification that accepts another enclosure adds no chunk hashes, which
    // the engine would refuse.
    const auto status = std::ranges::find_if(state.archives_page.statuses,
        [&](const ArchiveStatus& listed) { return listed.archive.archive_uuid == archive; });
    if (status != state.archives_page.statuses.end()) {
        if (const auto found = enclosure_case(attached_now(state), *status, state.identity_shown)) {
            if (choice == VerifyChoice::AddingChunkHashes) {
                show_message(state, [](const app::Text& text) {
                    return text.format("Not verified: {}",
                        text("chunk hashes are not added by the verification that accepts another enclosure; verify "
                             "the archive first, then add chunk hashes"));
                });
                return std::nullopt;
            }
            ask_about_enclosure(state, archive, choice == VerifyChoice::StrictMetadata, *found);
            return std::nullopt;
        }
    }
    return archive_request(state, archive, choice);
}

} // namespace

void verify_archive(GuiState& state, const Uuid& archive, VerifyChoice choice)
{
    verify_archives(state, {archive}, choice);
}

void verify_archives(GuiState& state, const std::vector<Uuid>& archives, VerifyChoice choice)
{
    if (!state.session) {
        show_message(state, fixed_message("Open a repository first (Settings)."));
        return;
    }
    std::vector<ToStart> runs;
    for (const auto& archive : archives) {
        if (auto request = archive_verification(state, archive, choice)) {
            runs.push_back({std::move(*request), archive});
        }
    }
    start_verifications(state, std::move(runs));
}

bool verify_confirmed(GuiState& state, const Uuid& archive, bool strict, CarrierConfirmation confirmation)
{
    if (!state.session) {
        show_message(state, fixed_message("Open a repository first (Settings)."));
        return false;
    }
    // A job may have started on it while the question was shown.
    if (refused_while_the_archive_has_a_job(state, archive, recorded_root(state, archive))) {
        return false;
    }
    auto request = archive_request(state, archive, strict ? VerifyChoice::StrictMetadata : VerifyChoice::Full);
    request.carrier_confirmation = std::move(confirmation);
    start_verifications(state, {ToStart{std::move(request), archive}});
    return true;
}

void choose_and_verify_root(GuiState& state, const Uuid& shown_in)
{
    if (!state.window) {
        return;
    }
    if (!state.session) {
        show_message(state, fixed_message("Open a repository first (Settings)."));
        return;
    }
    if (refused_while_the_archive_has_a_job(state, shown_in, recorded_root(state, shown_in))) {
        return;
    }
    const auto& text = state.text;
    AdwDialog* dialog = adw_alert_dialog_new(text("Verify a folder or volume").c_str(),
        text("Verifies the archive on the folder or volume you choose, for when more than one attached volume holds "
             "this archive")
            .c_str());
    auto* alert = ADW_ALERT_DIALOG(dialog);

    // The folder or volume, typed or pasted (a root that a refusal named, a
    // volume's GUID path, which a folder chooser may not offer), or chosen.
    GtkWidget* rows = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(rows), GTK_SELECTION_NONE);
    gtk_widget_add_css_class(rows, "boxed-list");
    GtkWidget* folder = adw_entry_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(folder), text("Folder or volume").c_str());
    GtkWidget* choose = gtk_button_new_with_label(text("Choose...").c_str());
    gtk_widget_set_valign(choose, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(choose, "flat");
    g_object_set_data(G_OBJECT(choose), "va-folder", folder);
    g_signal_connect(choose, "clicked", G_CALLBACK(+[](GtkButton* clicked, gpointer data) {
        auto& state = *static_cast<GuiState*>(data);
        auto* entry = static_cast<GtkWidget*>(g_object_get_data(G_OBJECT(clicked), "va-folder"));
        if (state.window && entry) {
            choose_folder(state.window, entry, state.text("Choose the archive folder or volume"));
        }
    }), &state);
    adw_entry_row_add_suffix(ADW_ENTRY_ROW(folder), choose);
    gtk_list_box_append(GTK_LIST_BOX(rows), folder);
    // R-51: as the command line's --strict-metadata; on, and fixed, where the
    // policy sets it.
    GtkWidget* strict = adw_switch_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(strict), text("Strict metadata").c_str());
    adw_action_row_set_subtitle(
        ADW_ACTION_ROW(strict), text("Timestamp and attribute differences fail the run").c_str());
    if (state.session->context.config.policy.strict_metadata) {
        adw_switch_row_set_active(ADW_SWITCH_ROW(strict), TRUE);
        gtk_widget_set_sensitive(strict, FALSE);
    }
    gtk_list_box_append(GTK_LIST_BOX(rows), strict);
    adw_alert_dialog_set_extra_child(alert, rows);

    adw_alert_dialog_add_response(alert, "cancel", text("Cancel").c_str());
    adw_alert_dialog_add_response(alert, "verify", text("FULL verify").c_str());
    adw_alert_dialog_set_response_appearance(alert, "verify", ADW_RESPONSE_SUGGESTED);
    adw_alert_dialog_set_default_response(alert, "cancel");
    adw_alert_dialog_set_close_response(alert, "cancel");
    // Offered once a folder or volume is given.
    adw_alert_dialog_set_response_enabled(alert, "verify", FALSE);
    g_signal_connect(folder, "changed", G_CALLBACK(+[](GtkEditable* editable, gpointer data) {
        const char* typed = gtk_editable_get_text(editable);
        adw_alert_dialog_set_response_enabled(ADW_ALERT_DIALOG(data), "verify", typed && *typed);
    }), dialog);
    g_signal_connect_data(dialog, "response",
        G_CALLBACK(+[](AdwAlertDialog*, const char* response, gpointer data) {
            const auto* choice = static_cast<const RootChoice*>(data);
            if (!response || std::string_view(response) != "verify") {
                return;
            }
            auto& state = *choice->state;
            // Made into a root exactly as the command line makes one typed
            // there (app::typed_root).
            const auto root = entry_root(choice->folder);
            const bool strict = adw_switch_row_get_active(ADW_SWITCH_ROW(choice->strict));
            const auto shown_in = choice->shown_in;
            guarded(
                [&] {
                    if (state.window) {
                        verify_root(state, shown_in, root, strict);
                    }
                },
                [&](const Report& report) { show_archives_status(state, report); });
        }),
        new RootChoice{&state, shown_in, folder, strict},
        +[](gpointer data, GClosure*) { delete static_cast<RootChoice*>(data); }, GConnectFlags{});
    adw_dialog_present(dialog, GTK_WIDGET(state.window));
    gtk_widget_grab_focus(folder);
}

void verify_root(GuiState& state, const Uuid& shown_in, const std::filesystem::path& root, bool strict)
{
    if (!state.session) {
        show_message(state, fixed_message("Open a repository first (Settings)."));
        return;
    }
    if (root.empty()) {
        show_message(state, fixed_message("Choose the folder or volume to verify."));
        return;
    }
    // Not while a job of the window waits or works on the archive whose
    // details started it, or reads at the root chosen.
    if (refused_while_the_archive_has_a_job(state, shown_in, path_to_utf8(root))) {
        return;
    }
    VerifyRequest request;
    request.root = root;
    request.strict_metadata = strict || state.session->context.config.policy.strict_metadata;
    request.rotational_override = state.session->context.config.rotational_override;
    start_verifications(state, {ToStart{std::move(request), shown_in}});
}

void resume_verification(GuiState& state, const app::UnfinishedOperation& verification)
{
    // Exactly `verify --full <archive_uuid> --resume <run_id>`: strict
    // metadata as the policy sets it (decision 0022, point 7). Shown in its
    // archive's details, which a narrow window brings into view.
    select_archive(state, verification.archive_uuid, true);
    run_verification(state, app::verification_resume(verification, state.session->context.config),
        verification.archive_uuid);
}

void resume_in_details(GuiState& state, const Uuid& archive, const std::string& key)
{
    if (!can_resume(state)) {
        return;
    }
    const auto& watches = state.operations.watches;
    const auto found = std::ranges::find(watches, key,
        [](const app::OperationWatch& watch) -> const std::string& { return watch.operation().key; });
    if (found == watches.end() || found->operation().archive_uuid != archive ||
        found->operation().kind == app::OperationKind::Registration) {
        show_message(state, fixed_message(key.starts_with("relocation:")
                                              ? "This relocation is no longer listed as unfinished."
                                              : "This verification is no longer listed as unfinished."));
        return;
    }
    // A copy: the job it starts may have the operations read again.
    const auto stopped = found->operation();
    // A stopped relocation (decision 0046): exactly as Resume on its row,
    // never while a job of the window reads it.
    if (stopped.kind == app::OperationKind::Relocation) {
        if (!refused_while_a_job_reads(state, stopped.root, stopped.archive_uuid)) {
            resume_relocation(state, stopped);
        }
        return;
    }
    auto request = details_resume(stopped, state.interrupted_run, state.session->context.config);
    if (refused_while_the_archive_has_a_job(
            state, archive, request.root ? path_to_utf8(*request.root) : stopped.root)) {
        return;
    }
    run_verification(state, std::move(request), archive);
}

} // namespace va::gui
