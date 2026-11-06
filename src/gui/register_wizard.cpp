// Register Archive (§B17, decision 0046): a dialog of three pages over the
// window. Choose the source lists the attached volumes, read off the window's
// thread (a fixed sample list in the screenshot mode), each to register as a
// whole, or, where it holds the roots of archives, to choose a folder in it;
// a folder can also be chosen (GtkFileDialog), typed or pasted, as a
// volume's GUID path. Next reads the source off the window's thread, refused
// as the command line refuses a root (app::root_not_taken: a network share,
// the \\?\ form, a root not there), and finds which archives' roots lie on
// its volume and of which it is an earlier carrier, as a registration of it
// would (register_choices.h). Options then offers what applies to it: its
// label, the repository's next one when left empty; a new revision of the
// archive registered there, or another archive; the resume of its unfinished
// registration, the only way on while there is one; a media copy (R-89),
// off; and which archive it is a copy of (decision 0005). Confirm says what the
// registration will do, and Start registration runs it as a job (decision
// 0045): refused as the command line refuses it, in the window's own words
// where another operation refuses it (app::register_message_in_window); a
// resume of a registration that an older version left asks first, then runs
// again confirmed (decision 0022, point 7). The report shows in the job's
// details, and a toast says what it registered, with View archive.
//
// Decision 0047: for a source whose device is behind an enclosure that hides
// the device's own serial, where this computer can read it, Options offers
// Record the device's own serial, off until the operator turns it on; Start
// registration then reads it first, off the window's thread, Windows asking
// for administrator permission, and gives the read to the registration
// (RegisterRequest::device_read). A read that gave nothing leaves Confirm
// saying why, with Start registration without the serial. A serial read
// before (Devices, the enclosure question) of the same device is recorded
// with no second question. Register as another archive... of the enclosure
// question opens it for the root where an archive's filesystem is on another
// device, as a separate archive, a copy of that one.
#include "gui/gui_app.h"
#include "gui/off_thread.h"

#include "app/typed_path.h"
#include "common/unicode.h"
#include "common/utf8.h"
#include "core/engine/candidates.h"
#include "core/engine/register_engine.h"
#include "platform/file_system.h"
#include "platform/media_enumerator.h"
#include "platform/platform_services.h"
#include "platform/process_table.h"

#include <atomic>
#include <chrono>
#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace va::gui {
namespace {

// ---- The registration, a job ------------------------------------------------

// Runs `request` as a job (decision 0045). `typed`: the root as the operator
// gave it, or as the plan of a registration resumed from its row under
// Unfinished operations names it (decision 0022, point 7); its root is then
// checked as `register <root>` checks it (app::root_not_taken), also that it
// is a folder.
void run_registration(GuiState& state, RegisterRequest request, std::optional<std::string> typed)
{
    const auto root = path_to_utf8(request.root);
    // Filled by the worker where a resume waits for the operator's word, as
    // it takes up a registration that an older version left, which may still
    // run it (decision 0022, point 7); read by `done`, which runs only after
    // the worker finished.
    auto unconfirmed = std::make_shared<std::optional<OlderVersionRegistration>>();
    // What it registered, which the toast names once it ended.
    struct Registered {
        Uuid archive_uuid;
        std::string label;
        std::uint32_t revision = 0;
    };
    auto registered = std::make_shared<std::optional<Registered>>();
    // The worker returns its report unworded; it is worded when shown, in the
    // language in use then.
    JobSpec job;
    job.keeps_progress = true;
    job.title = [root](const app::Text& text) { return text.format("Register {}", root); };
    job.target.root = request.root;
    job.devices = [root = request.root] { return devices_of_root(root); };
    job.work = [&state, request, typed, unconfirmed, registered](JobContext& context) -> Report {
        // Not registered, until it is.
        context.tone = JobTone::Error;
        // Decision 0029: a root on a network share, or one written in the
        // \\?\ form, is refused before anything of it is read, and a resumed
        // one that is no folder too, as on the command line. Asked here, off
        // the window's thread: on Linux it reads the root's filesystem.
        if (auto refusal = app::root_not_taken(request.root, typed ? *typed : path_to_utf8(request.root))) {
            return [refusal = std::move(*refusal)](const app::Text& text) {
                return text.format("Not registered: {}", text(refusal));
            };
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
        RegisterServices services{*archive_files, *repository_files, *session.writer, *session.writer, *media, *store,
            session.context.repo, state.host, *processes, platform::final_path};
        // Decision 0044: a device alert shows in the job as soon as the
        // devices were asked; the operator may stop it, or let it go on.
        auto asked = request;
        asked.devices_observed = context.devices_observed;
        auto outcome = register_archive(services, asked, context.cancel, context.progress);
        if (!outcome) {
            // A resume of a registration that an older version left, which
            // changed nothing: the window asks the operator, as the command
            // line asks for --confirm, and says why meanwhile.
            if (outcome.error().kind == RegisterFailure::Kind::Unconfirmed && outcome.error().older_version) {
                *unconfirmed = outcome.error().older_version;
                context.tone = JobTone::Warning;  // nothing went wrong: the question follows
                return [](const app::Text& text) {
                    return text.format("Not registered: {}", text(app::older_version_resume_warning));
                };
            }
            // Stopped, or cut short by a device that went away: it can be
            // resumed.
            if (outcome.error().kind == RegisterFailure::Kind::Incomplete) {
                context.tone = JobTone::Stopped;
            }
            // Refused while another operation that creates the archive's
            // next revision is unfinished, it names the window's own ways
            // on, or says that that operation still runs, judged now as
            // Archives judges its row (decisions 0025, 0033).
            const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
            return [message = app::register_message_in_window(
                        outcome.error(), *store, *processes, state.host.host, now)](const app::Text& text) {
                return text.format("Not registered: {}", text(message));
            };
        }
        // The devices as the views name them: the sample identity in the
        // screenshot mode, whose pictures are published.
        outcome->devices = displayed_devices(outcome->devices, state.identity_shown);
        *registered = Registered{outcome->archive_uuid, outcome->label, outcome->revision};
        // Entries recorded without hashes, and warnings, ask to be read.
        context.tone = outcome->unreadable.empty() && outcome->warnings.empty() ? JobTone::Success : JobTone::Warning;
        // Decision 0047: the device's own serial it recorded, read as it
        // started.
        return [outcome = std::move(*outcome), read = request.device_read](const app::Text& text) {
            auto report = registration_report(text, outcome);
            if (read) {
                report += recorded_device_serial_line(text, *read) + "\n";
            }
            return report;
        };
    };
    job.done = [&state, request, typed, unconfirmed, registered, root](std::uint64_t ended, const Report&) {
        guarded(
            [&] {
                // Devices offers its volume again where nothing was registered.
                state.registering.erase(ended);
                // Decision 0047: the device's own serial it recorded is the
                // archive's confirmed device now, which Devices says of it.
                if (*registered && request.device_read) {
                    note_confirmed(state.device_reads, request.device_read->device, (*registered)->archive_uuid);
                }
                refresh_devices(state);
                // The archive it registered, or the revision; its unfinished
                // operation gone, or stopped.
                refresh_archives(state);
                start_poll(state);
                if (*unconfirmed) {
                    // Decision 0022, point 7: the question, then the same
                    // resume, confirmed as with --confirm, only once the
                    // operator chose Resume in it.
                    const auto& registration = **unconfirmed;
                    const auto label =
                        registration.label.empty() ? registration.archive_uuid.substr(0, 8) : registration.label;
                    ask_before_resuming(state, label, registration.root, [request, typed](GuiState& state) {
                        // A job may have started on it while the question was shown.
                        if (!state.session) {
                            show_message(state, fixed_message("Open a repository first (Settings)."));
                            return;
                        }
                        if (refused_while_a_job_reads(state, path_to_utf8(request.root), {})) {
                            return;
                        }
                        auto confirmed = request;
                        confirmed.confirmed = true;
                        run_registration(state, std::move(confirmed), typed);
                    });
                    return;
                }
                // View archive selects it in Archives; a registration that
                // registered nothing has its reason in Details.
                if (*registered) {
                    const auto& done = **registered;
                    show_toast(state, registered_toast(state.text, done.label, done.revision),
                        ToastAction{state.text("View archive"), [uuid = done.archive_uuid](GuiState& state) {
                                        show_page(state, "archives");
                                        select_archive(state, uuid, true);
                                    }});
                } else {
                    show_toast(state, not_registered_toast(state.text, root),
                        ToastAction{state.text("Details"), [ended](GuiState& state) { open_job_details(state, ended); }});
                }
            },
            [&](const Report& error) { show_archives_status(state, error); });
    };
    const auto& text = state.text;
    const auto title = job.title(text);
    const auto id = state.jobs.start(text, std::move(job));
    // Devices names its root on its volume in place of Register this volume.
    state.registering[id] = request.root;
    refresh_devices(state);
    job_changed(state, id);
    // Nowhere else in the window shows it until it ends: the toast leads to it.
    show_toast(state, text.format("{}: started", title),
        ToastAction{text("Details"), [id](GuiState& state) { open_job_details(state, id); }}, ToastKind::Started);
}

// ---- Reads off the window's thread (off_thread.h) ---------------------------

struct SourceRead {
    std::optional<std::string> refused;   // app::root_not_taken, an English sentence
    std::string unidentified;             // why its volume cannot be told, an engine sentence
    std::optional<ChosenSource> source;
    std::vector<ArchiveCandidate> candidates;
    std::vector<PendingRoot> pending;
    std::vector<EarlierCarrier> earlier;  // the archives its volume is an earlier carrier of
    std::optional<std::string> next_label;  // ArchiveStore::peek_next_label, where it could be read
    Report unread;
    Report failure;
};

// What a registration of the source would find besides the archives on its
// volume (check_existing_archives): the archives whose roots overlap it and
// of which its volume is an earlier carrier (decision 0004), told by
// match_recorded, which reads the manifests of their earlier revisions only
// where that decides it; and the label a new archive would take.
void read_earlier_carriers(const RepositoryLayout& layout, const ChosenSource& source, SourceRead& read)
{
    auto store = ArchiveStore::open_read_only(layout);
    if (!store) {
        read.unread = [message = store.error().message](const app::Text& text) {
            return text.format("Cannot open the repository: {}", text(message));
        };
        return;
    }
    const auto repository_files = platform::make_file_system_access();
    const MediaEvidence evidence{{}, source.volume, std::nullopt};
    for (auto candidate : read.candidates) {  // a copy: match_recorded keeps the earlier carriers in it
        // A retired archive's root refuses no root that contains it or lies
        // in it (decision 0051); at its very root it still decides.
        if (!roots_overlap(candidate.root_subpath, source.root_subpath) ||
            (candidate.retired && unicode_fold(candidate.root_subpath) != unicode_fold(source.root_subpath))) {
            continue;
        }
        const auto match = match_recorded(*store, *repository_files, layout, evidence, candidate.root_subpath,
            std::span(&candidate, 1));
        if (!match) {
            read.unread = [message = match.error().message](const app::Text& text) {
                return text.format("Cannot open the repository: {}", text(message));
            };
            return;
        }
        if (match->result == MatchResult::PreviousCarrier) {
            read.earlier.push_back({candidate.archive_uuid, candidate.root_subpath});
        }
    }
    if (auto label = store->peek_next_label()) {
        read.next_label = std::move(*label);
    }
}

// ---- The dialog -------------------------------------------------------------

using Wizard = GuiState::RegisterWizard;

bool is_open(const GuiState& state, std::uint64_t opened)
{
    return state.register_wizard.dialog && state.register_wizard.opened == opened;
}

// The steps above each page, the one shown in the accent colour.
GtkWidget* steps_line(const app::Text& text, int current)
{
    GtkWidget* line = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 18);
    const char* steps[] = {"Choose the source", "Options", "Confirm"};
    for (int index = 0; index < 3; ++index) {
        GtkWidget* step = gtk_label_new(text.format("{}. {}", index + 1, text(steps[index])).c_str());
        gtk_widget_add_css_class(step, index + 1 == current ? "accent" : "dim-label");
        if (index + 1 == current) {
            gtk_widget_add_css_class(step, "heading");
        }
        gtk_box_append(GTK_BOX(line), step);
    }
    return line;
}

// A line that says what holds the operator up, in the colour of its kind
// (va-error: the source cannot be registered; va-neutral: an option is to be
// chosen first), hidden until it says something.
GtkWidget* notice_line(const char* kind, const char* icon, const char* icon_style, GtkWidget** words)
{
    GtkWidget* line = make_line_box(kind, icon, icon_style);
    *words = make_words("", nullptr, true);
    gtk_widget_set_hexpand(*words, TRUE);
    gtk_box_append(GTK_BOX(line), *words);
    gtk_widget_set_visible(line, FALSE);
    return line;
}

void say(GtkWidget* line, GtkWidget* words, const std::optional<std::string>& what)
{
    if (!line) {
        return;
    }
    gtk_label_set_text(GTK_LABEL(words), what ? what->c_str() : "");
    gtk_widget_set_visible(line, what.has_value());
}

// A page of the dialog: the title above (Register Archive), the steps, its
// content, and its buttons below: `back` at the start, `next` at the end.
GtkWidget* wizard_page(const app::Text& text, const char* step_title, int step, GtkWidget* content, GtkWidget* back,
    GtkWidget* next, GtkWidget* spinner = nullptr)
{
    GtkWidget* view = adw_toolbar_view_new();
    GtkWidget* header = adw_header_bar_new();
    // The buttons below go back; the header's own would say it twice.
    adw_header_bar_set_show_back_button(ADW_HEADER_BAR(header), FALSE);
    adw_header_bar_set_title_widget(ADW_HEADER_BAR(header), adw_window_title_new(text("Register Archive").c_str(), ""));
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(view), header);

    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 18);
    gtk_widget_set_margin_start(box, 24);
    gtk_widget_set_margin_end(box, 24);
    gtk_widget_set_margin_top(box, 6);
    gtk_widget_set_margin_bottom(box, 18);
    gtk_box_append(GTK_BOX(box), steps_line(text, step));
    gtk_box_append(GTK_BOX(box), content);
    GtkWidget* scroller = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scroller), TRUE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), box);
    gtk_widget_set_vexpand(scroller, TRUE);
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(view), scroller);

    GtkWidget* buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_margin_start(buttons, 24);
    gtk_widget_set_margin_end(buttons, 24);
    gtk_widget_set_margin_top(buttons, 12);
    gtk_widget_set_margin_bottom(buttons, 18);
    gtk_box_append(GTK_BOX(buttons), back);
    GtkWidget* gap = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(gap, TRUE);
    gtk_box_append(GTK_BOX(buttons), gap);
    if (spinner) {
        gtk_widget_set_valign(spinner, GTK_ALIGN_CENTER);
        gtk_box_append(GTK_BOX(buttons), spinner);
    }
    gtk_widget_add_css_class(next, "suggested-action");
    gtk_box_append(GTK_BOX(buttons), next);
    adw_toolbar_view_add_bottom_bar(ADW_TOOLBAR_VIEW(view), buttons);
    return GTK_WIDGET(adw_navigation_page_new(view, text(step_title).c_str()));
}

// Back: the page before, as the navigation view's own back does.
GtkWidget* back_button(GuiState& state)
{
    GtkWidget* back = gtk_button_new_with_label(state.text("Back").c_str());
    g_signal_connect(back, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) {
        auto& state = *static_cast<GuiState*>(data);
        if (state.register_wizard.navigation) {
            adw_navigation_view_pop(ADW_NAVIGATION_VIEW(state.register_wizard.navigation));
        }
    }), &state);
    return back;
}

void read_source(GuiState& state);

// Next of the first page: offered once a folder or volume is given and while
// it is not being read.
void show_source_next(GuiState& state)
{
    auto& wizard = state.register_wizard;
    if (!wizard.source_next) {
        return;
    }
    const char* typed = gtk_editable_get_text(GTK_EDITABLE(wizard.folder));
    gtk_widget_set_sensitive(wizard.source_next, !wizard.reading_source && typed && *typed);
    gtk_widget_set_visible(wizard.reading, wizard.reading_source);
    gtk_spinner_set_spinning(GTK_SPINNER(wizard.reading), wizard.reading_source);
}

// ---- Choose the source: the attached volumes --------------------------------

void clear_volume_rows(Wizard& wizard)
{
    for (GtkWidget* row : wizard.volume_rows) {
        adw_preferences_group_remove(ADW_PREFERENCES_GROUP(wizard.volumes), row);
    }
    wizard.volume_rows.clear();
    wizard.radios.clear();
    wizard.radio_roots.clear();
}

GtkWidget* words_row(const std::string& words, const char* style = nullptr, bool spinning = false)
{
    GtkWidget* row = adw_action_row_new();
    adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), words.c_str());
    if (style) {
        gtk_widget_add_css_class(row, style);
    }
    if (spinning) {
        GtkWidget* spinner = gtk_spinner_new();
        gtk_spinner_start(GTK_SPINNER(spinner));
        adw_action_row_add_prefix(ADW_ACTION_ROW(row), spinner);
    }
    return row;
}

void add_volume_row(Wizard& wizard, GtkWidget* row)
{
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(wizard.volumes), row);
    wizard.volume_rows.push_back(row);
}

// What a button of a volume's row is for: the volume's root, whose folders
// the folder chooser opens in.
struct FolderIn {
    GuiState* state;
    std::string root;
};

GtkWidget* volume_row(GuiState& state, const VolumeRow& shown)
{
    auto& wizard = state.register_wizard;
    GtkWidget* row = adw_action_row_new();
    adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), shown.title.c_str());
    std::string subtitle = shown.where;
    for (const auto& line : {shown.details, shown.note}) {
        if (!line.empty()) {
            subtitle += "\n" + line;
        }
    }
    adw_action_row_set_subtitle(ADW_ACTION_ROW(row), subtitle.c_str());
    gtk_widget_add_css_class(row, "numeric");

    GtkWidget* radio = gtk_check_button_new();
    gtk_widget_set_valign(radio, GTK_ALIGN_CENTER);
    if (!wizard.radios.empty()) {
        gtk_check_button_set_group(GTK_CHECK_BUTTON(radio), GTK_CHECK_BUTTON(wizard.radios.front()));
    }
    wizard.radios.push_back(radio);
    wizard.radio_roots.push_back(shown.root);
    adw_action_row_add_prefix(ADW_ACTION_ROW(row), radio);
    gtk_accessible_update_property(GTK_ACCESSIBLE(radio), GTK_ACCESSIBLE_PROPERTY_LABEL,
        state.text.format("{} · {}", shown.title, shown.where).c_str(), -1);
    if (shown.selectable) {
        adw_action_row_set_activatable_widget(ADW_ACTION_ROW(row), radio);
        g_object_set_data_full(G_OBJECT(radio), "va-root", g_strdup(shown.root.c_str()), g_free);
        g_signal_connect(radio, "toggled", G_CALLBACK(+[](GtkCheckButton* toggled, gpointer data) {
            auto& state = *static_cast<GuiState*>(data);
            auto& wizard = state.register_wizard;
            if (!gtk_check_button_get_active(toggled) || wizard.filling || !wizard.folder) {
                return;
            }
            // The volume chosen is the root, in the folder's row, which
            // Next reads.
            wizard.preset_root.reset();
            wizard.filling = true;
            gtk_editable_set_text(GTK_EDITABLE(wizard.folder),
                static_cast<const char*>(g_object_get_data(G_OBJECT(toggled), "va-root")));
            wizard.filling = false;
            say(wizard.problem, wizard.problem_words, std::nullopt);
            show_source_next(state);
        }), &state);
    } else {
        // Not as a whole: it holds roots of archives (decision 0001).
        gtk_widget_set_sensitive(radio, FALSE);
    }
    if (!shown.folder_button.empty()) {
        GtkWidget* choose = gtk_button_new_with_label(shown.folder_button.c_str());
        gtk_widget_set_valign(choose, GTK_ALIGN_CENTER);
        // The volume in full, where the button is short for a GUID path.
        gtk_widget_set_tooltip_text(choose, shown.folder_tooltip.c_str());
        gtk_accessible_update_property(
            GTK_ACCESSIBLE(choose), GTK_ACCESSIBLE_PROPERTY_LABEL, shown.folder_tooltip.c_str(), -1);
        g_signal_connect_data(choose, "clicked",
            G_CALLBACK(+[](GtkButton*, gpointer data) {
                const auto* folder = static_cast<const FolderIn*>(data);
                auto& state = *folder->state;
                guarded(
                    [&] {
                        if (state.window && state.register_wizard.folder) {
                            choose_folder(state.window, state.register_wizard.folder,
                                state.text("Choose the archive folder or volume"), path_from_utf8(folder->root));
                        }
                    },
                    [&](const Report& report) { show_message(state, report); });
            }),
            new FolderIn{&state, shown.root}, +[](gpointer data, GClosure*) { delete static_cast<FolderIn*>(data); },
            GConnectFlags{});
        adw_action_row_add_suffix(ADW_ACTION_ROW(row), choose);
    }
    return row;
}

// Create a new revision... (register_new_revision), once the attached volumes
// were read: the root where they hold the archive now, or the one its latest
// revision recorded, typed and read as Next reads it.
void read_preset_source(GuiState& state, const VolumesRead& read)
{
    auto& wizard = state.register_wizard;
    if (!wizard.preset_root || !wizard.folder) {
        return;
    }
    const auto recorded = *std::exchange(wizard.preset_root, std::nullopt);
    const auto root = read.failure || read.unread || !wizard.preset_archive
                          ? recorded
                          : new_revision_source(read.volumes, read.candidates, *wizard.preset_archive, recorded);
    wizard.filling = true;
    gtk_editable_set_text(GTK_EDITABLE(wizard.folder), root.c_str());
    wizard.filling = false;
    show_source_next(state);
    read_source(state);
}

// Lists the volumes that a read found, or why it found none.
void show_volumes(GuiState& state, VolumesRead& read)
{
    auto& wizard = state.register_wizard;
    const auto& text = state.text;
    clear_volume_rows(wizard);
    if (read.failure) {
        add_volume_row(wizard, words_row(read.failure(text), "error"));
        read_preset_source(state, read);
        return;
    }
    if (read.unread) {
        // The volumes, without what lies on them: Next reads that again.
        add_volume_row(wizard, words_row(read.unread(text), "error"));
    }
    const auto rows = volume_rows(text, read.volumes, read.candidates, read.pending, wizard.names,
        read.sample ? IdentityShown::Recorded : state.identity_shown);
    if (rows.empty()) {
        add_volume_row(wizard, words_row(text("No volume is attached."), "dim-label"));
    }
    const std::string typed = gtk_editable_get_text(GTK_EDITABLE(wizard.folder));
    for (const auto& shown : rows) {
        add_volume_row(wizard, volume_row(state, shown));
        // The volume typed already, chosen.
        if (shown.selectable && shown.root == typed) {
            wizard.filling = true;
            gtk_check_button_set_active(GTK_CHECK_BUTTON(wizard.radios.back()), TRUE);
            wizard.filling = false;
        }
    }
    read_preset_source(state, read);
}

// Reads the attached volumes and the archives on them off the window's
// thread; the screenshot mode lists its sample volumes, never this
// computer's.
void read_volumes(GuiState& state)
{
    auto& wizard = state.register_wizard;
    if (!state.session || !wizard.volumes) {
        return;
    }
    clear_volume_rows(wizard);
    add_volume_row(wizard, words_row(state.text("Reading the attached volumes..."), "dim-label", true));
    const auto layout = state.session->context.repo;
    const bool sample = state.identity_shown == IdentityShown::Sample;
    read_off_thread<VolumesRead>(state, [layout, sample] { return read_attached_volumes(layout, sample); },
        [opened = wizard.opened](GuiState& state, VolumesRead& read) {
            if (is_open(state, opened)) {
                show_volumes(state, read);
            }
        });
}

// ---- Options ----------------------------------------------------------------

// What the options' rows say now.
OptionsChosen chosen_options(const Wizard& wizard)
{
    OptionsChosen chosen = wizard.chosen;
    if (!wizard.options_page) {
        return chosen;
    }
    chosen.label = gtk_editable_get_text(GTK_EDITABLE(wizard.label));
    chosen.new_revision = adw_switch_row_get_active(ADW_SWITCH_ROW(wizard.new_revision));
    chosen.another_archive = adw_switch_row_get_active(ADW_SWITCH_ROW(wizard.another_archive));
    chosen.media_copy = adw_switch_row_get_active(ADW_SWITCH_ROW(wizard.media_copy));
    const auto selected = adw_combo_row_get_selected(ADW_COMBO_ROW(wizard.copy_of));
    chosen.copy_of = selected != GTK_INVALID_LIST_POSITION && selected > 0 && selected <= wizard.copy_of_uuids.size()
                         ? std::optional(wizard.copy_of_uuids[selected - 1])
                         : std::nullopt;
    chosen.record_device_serial =
        wizard.device_serial && adw_switch_row_get_active(ADW_SWITCH_ROW(wizard.device_serial));
    return chosen;
}

// The rows that apply to what is chosen now, and Next where nothing holds it up.
void show_options(GuiState& state)
{
    auto& wizard = state.register_wizard;
    if (!wizard.options_page) {
        return;
    }
    wizard.chosen = chosen_options(wizard);
    const auto shown = options_shown(state.text, wizard.options, wizard.chosen, wizard.names);
    gtk_widget_set_visible(wizard.label_group, shown.label);
    adw_preferences_group_set_description(ADW_PREFERENCES_GROUP(wizard.label_group), shown.label_note.c_str());
    gtk_widget_set_visible(wizard.new_revision, shown.new_revision);
    adw_action_row_set_subtitle(ADW_ACTION_ROW(wizard.new_revision), shown.new_revision_note.c_str());
    gtk_widget_set_visible(wizard.another_archive, shown.another_archive);
    adw_action_row_set_subtitle(ADW_ACTION_ROW(wizard.another_archive), shown.another_archive_note.c_str());
    gtk_widget_set_visible(wizard.resume, shown.resume);
    adw_action_row_set_subtitle(ADW_ACTION_ROW(wizard.resume), shown.resume_note.c_str());
    gtk_widget_set_visible(wizard.copy_of, shown.copy_of);
    say(wizard.blocked, wizard.blocked_words, shown.blocked);
    gtk_widget_set_sensitive(wizard.options_next, !shown.blocked);
}

GtkWidget* switch_row(const std::string& title, bool active)
{
    GtkWidget* row = adw_switch_row_new();
    adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title.c_str());
    adw_switch_row_set_active(ADW_SWITCH_ROW(row), active);
    return row;
}

void push_confirm(GuiState& state);

// A switch whose change shows the options anew; on, it turns `other` off (a
// new revision and another archive exclude each other).
void connect_switch(GuiState& state, GtkWidget* row, GtkWidget* other)
{
    g_object_set_data(G_OBJECT(row), "va-other", other);
    g_signal_connect(row, "notify::active", G_CALLBACK(+[](GObject* changed, GParamSpec*, gpointer data) {
        auto& state = *static_cast<GuiState*>(data);
        auto& wizard = state.register_wizard;
        if (wizard.filling) {
            return;
        }
        guarded(
            [&] {
                auto* other = static_cast<GtkWidget*>(g_object_get_data(changed, "va-other"));
                if (other && adw_switch_row_get_active(ADW_SWITCH_ROW(changed))) {
                    wizard.filling = true;
                    adw_switch_row_set_active(ADW_SWITCH_ROW(other), FALSE);
                    wizard.filling = false;
                }
                show_options(state);
            },
            [&](const Report& report) { show_message(state, report); });
    }), &state);
}

// The Options page for the source read, its rows as wizard.chosen says them.
void push_options(GuiState& state)
{
    auto& wizard = state.register_wizard;
    const auto& text = state.text;
    const auto& source = *wizard.source;
    GtkWidget* content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 18);

    // The source, whose options these are. Not selectable: the page's first
    // to take the focus would show its words selected; Confirm's can be.
    GtkWidget* head = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_box_append(GTK_BOX(head), make_words(path_to_utf8(source.root), "heading"));
    gtk_box_append(GTK_BOX(head), make_words(source_volume_words(text, source, state.identity_shown), "dim-label"));
    gtk_box_append(GTK_BOX(content), head);

    wizard.filling = true;
    // The label, where it applies, with what an empty one means
    // (show_options says it).
    wizard.label_group = adw_preferences_group_new();
    wizard.label = adw_entry_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(wizard.label), text("Label").c_str());
    gtk_editable_set_text(GTK_EDITABLE(wizard.label), wizard.chosen.label.c_str());
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(wizard.label_group), wizard.label);
    gtk_box_append(GTK_BOX(content), wizard.label_group);

    GtkWidget* options = adw_preferences_group_new();
    wizard.new_revision = switch_row(text("Create a new revision"), wizard.chosen.new_revision);
    wizard.another_archive = switch_row(
        text("Register as a separate archive even though this folder or volume is registered"),
        wizard.chosen.another_archive);
    // No switch: where an interrupted registration of the root exists, its
    // resume is the only way on.
    wizard.resume = adw_action_row_new();
    adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(wizard.resume), FALSE);
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(wizard.resume), text("Resume the interrupted registration").c_str());
    wizard.media_copy = switch_row(text("Also write a manifest copy to the media (.veriarchive)"), wizard.chosen.media_copy);
    adw_action_row_set_subtitle(ADW_ACTION_ROW(wizard.media_copy),
        text("Writes the manifest into the .veriarchive folder in the archive's root. Otherwise VeriArchive writes "
             "nothing to the archive.")
            .c_str());
    wizard.copy_of = adw_combo_row_new();
    show_whole_items(wizard.copy_of);
    adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(wizard.copy_of), FALSE);
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(wizard.copy_of), text("Copy of which archive (optional)").c_str());
    const auto choices = copy_of_choices(wizard.names, wizard.options);
    std::vector<std::string> items{text("Not a copy")};
    wizard.copy_of_uuids.clear();
    guint selected = 0;
    for (const auto& name : choices) {
        items.push_back(choice_words(text, name));
        wizard.copy_of_uuids.push_back(name.archive_uuid);
        if (wizard.chosen.copy_of == name.archive_uuid) {
            selected = static_cast<guint>(wizard.copy_of_uuids.size());
        }
    }
    std::vector<const char*> pointers;
    for (const auto& item : items) {
        pointers.push_back(item.c_str());
    }
    pointers.push_back(nullptr);
    GtkStringList* model = gtk_string_list_new(pointers.data());
    adw_combo_row_set_model(ADW_COMBO_ROW(wizard.copy_of), G_LIST_MODEL(model));
    g_object_unref(model);
    adw_combo_row_set_selected(ADW_COMBO_ROW(wizard.copy_of), selected);
    for (GtkWidget* row : {wizard.new_revision, wizard.another_archive, wizard.resume, wizard.media_copy,
             wizard.copy_of}) {
        adw_preferences_group_add(ADW_PREFERENCES_GROUP(options), row);
    }
    // Decision 0047: the device's own serial behind its enclosure, where it
    // can be read: the switch, off until the operator turns it on; or the
    // serial read before, which the registration records.
    wizard.device_serial = nullptr;
    wizard.device_serial_kept = nullptr;
    if (wizard.serial_offer && wizard.serial_offer->kept) {
        wizard.device_serial_kept = adw_action_row_new();
        adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(wizard.device_serial_kept), FALSE);
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(wizard.device_serial_kept), text("Device's own serial").c_str());
        adw_action_row_set_subtitle(ADW_ACTION_ROW(wizard.device_serial_kept),
            text.format("{} (read)", read_serial(wizard.serial_offer->kept->device)).c_str());
        adw_preferences_group_add(ADW_PREFERENCES_GROUP(options), wizard.device_serial_kept);
    } else if (wizard.serial_offer) {
        wizard.device_serial = switch_row(text("Record the device's own serial"), wizard.chosen.record_device_serial);
        adw_action_row_set_subtitle(ADW_ACTION_ROW(wizard.device_serial), device_serial_note(text).c_str());
        adw_action_row_add_prefix(ADW_ACTION_ROW(wizard.device_serial),
            gtk_image_new_from_icon_name("security-high-symbolic"));
        adw_preferences_group_add(ADW_PREFERENCES_GROUP(options), wizard.device_serial);
    }
    gtk_box_append(GTK_BOX(content), options);
    gtk_box_append(GTK_BOX(content),
        notice_line("va-neutral", "dialog-information-symbolic", "dim-label", &wizard.blocked_words));
    wizard.blocked = gtk_widget_get_last_child(content);
    wizard.filling = false;

    connect_switch(state, wizard.new_revision, wizard.another_archive);
    connect_switch(state, wizard.another_archive, wizard.new_revision);
    connect_switch(state, wizard.media_copy, nullptr);
    if (wizard.device_serial) {
        connect_switch(state, wizard.device_serial, nullptr);
    }
    g_signal_connect(wizard.label, "changed", G_CALLBACK(+[](GtkEditable*, gpointer data) {
        auto& state = *static_cast<GuiState*>(data);
        if (!state.register_wizard.filling) {
            guarded([&] { show_options(state); }, [&](const Report& report) { show_message(state, report); });
        }
    }), &state);
    g_signal_connect(wizard.copy_of, "notify::selected", G_CALLBACK(+[](GObject*, GParamSpec*, gpointer data) {
        auto& state = *static_cast<GuiState*>(data);
        if (!state.register_wizard.filling) {
            guarded([&] { show_options(state); }, [&](const Report& report) { show_message(state, report); });
        }
    }), &state);

    wizard.options_next = gtk_button_new_with_label(text("Next").c_str());
    g_signal_connect(wizard.options_next, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) {
        auto& state = *static_cast<GuiState*>(data);
        guarded([&] { push_confirm(state); }, [&](const Report& report) { show_message(state, report); });
    }), &state);
    wizard.options_page = wizard_page(text, "Options", 2, content, back_button(state), wizard.options_next);
    adw_navigation_view_push(ADW_NAVIGATION_VIEW(wizard.navigation), ADW_NAVIGATION_PAGE(wizard.options_page));
    show_options(state);
}

// ---- Confirm ----------------------------------------------------------------

// The Confirm page's field of the device's own serial, its notice of why a
// read gave nothing, and its Start registration, as the read stands now
// (decision 0047).
void show_confirm_serial(GuiState& state)
{
    auto& wizard = state.register_wizard;
    if (!wizard.start) {
        return;
    }
    const auto& text = state.text;
    say(wizard.serial_notice, wizard.serial_notice_words,
        wizard.serial_problem
            ? std::optional(read_problem_words(text, *wizard.serial_problem, ReadShownIn::Registration))
            : std::nullopt);
    // What it will do, the device's own serial as the read stands now.
    if (wizard.summary) {
        while (GtkWidget* row = gtk_widget_get_first_child(wizard.summary)) {
            gtk_list_box_remove(GTK_LIST_BOX(wizard.summary), row);
        }
        auto fields = registration_summary(
            text, *wizard.source, wizard.options, wizard.chosen, wizard.names, state.identity_shown);
        if (auto serial =
                device_serial_field(text, wizard.serial_offer, wizard.chosen, wizard.serial_problem.has_value())) {
            fields.push_back(std::move(*serial));
        }
        gtk_list_box_append(GTK_LIST_BOX(wizard.summary), make_fields_row(fields));
    }
    gtk_button_set_label(GTK_BUTTON(wizard.start),
        wizard.serial_problem ? text("Start registration without the serial").c_str()
                              : text("Start registration").c_str());
    gtk_widget_set_sensitive(wizard.start, !wizard.reading_serial);
    gtk_widget_set_visible(wizard.starting, wizard.reading_serial);
    gtk_spinner_set_spinning(GTK_SPINNER(wizard.starting), wizard.reading_serial);
}

void push_confirm(GuiState& state);

// Starts the registration, with `device_read` where the device's own serial
// was read for it.
void start_registration(GuiState& state, std::optional<DeviceIdentityRead> device_read)
{
    auto& wizard = state.register_wizard;
    if (!wizard.source) {
        return;
    }
    auto request = registration_request(
        *wizard.source, wizard.options, wizard.chosen, state.session->context.config.rotational_override);
    request.device_read = std::move(device_read);
    // The dialog goes; the job's toast and Jobs show the registration.
    // Forgotten at once (as close_dialogs does): it closes once its animation
    // has ended, and no read in flight may fill it meanwhile.
    GtkWidget* closing = std::exchange(state.register_wizard, {}).dialog;
    adw_dialog_close(ADW_DIALOG(closing));
    run_registration(state, std::move(request), std::nullopt);
}

void start_from_wizard(GuiState& state)
{
    auto& wizard = state.register_wizard;
    if (!state.session) {
        show_message(state, fixed_message("Open a repository first (Settings)."));
        return;
    }
    if (!wizard.source || wizard.reading_serial) {
        return;
    }
    // Decision 0047: the serial read before is recorded, while the window
    // still keeps it (it drops a read of a device detached or described
    // otherwise since, and every read while it did not watch the devices);
    // one asked for is read first, Windows asking for administrator
    // permission; a read that gave nothing leaves Start registration without
    // the serial.
    if (wizard.serial_offer && wizard.serial_offer->kept) {
        if (const auto* kept = kept_read_of(state.device_reads, wizard.serial_offer->device);
            kept && same_device_identity(kept->read.device, wizard.serial_offer->kept->device)) {
            start_registration(state, kept->read);
            return;
        }
        // Gone since Options showed it: Confirm says so, and registers
        // without it.
        wizard.serial_offer->kept.reset();
        wizard.serial_problem = ReadProblem{DeviceIdentityFailure::Unreadable,
            "the serial read before is no longer kept, as the device was detached or changed since"};
        show_confirm_serial(state);
        return;
    }
    if (!wizard.serial_offer || !wizard.chosen.record_device_serial || wizard.serial_problem) {
        start_registration(state, std::nullopt);
        return;
    }
    // Read since the options were shown (in Devices, or here before Back).
    if (const auto* kept = kept_read_of(state.device_reads, wizard.serial_offer->device)) {
        start_registration(state, kept->read);
        return;
    }
    wizard.reading_serial = true;
    wizard.serial_cancel = std::make_shared<CancelToken>();
    show_confirm_serial(state);
    const auto started = read_own_identity(state, wizard.serial_offer->device,
        [opened = wizard.opened](GuiState& state, OwnIdentityRead& result) {
            auto& wizard = state.register_wizard;
            if (!is_open(state, opened)) {
                return;  // closed meanwhile: nothing is registered; the read is kept, shown in Devices
            }
            wizard.reading_serial = false;
            // Back to Options meanwhile: nothing starts; the read is kept,
            // and Start registration takes it.
            if (!wizard.confirm_page) {
                return;
            }
            if (result.failure) {
                show_confirm_serial(state);
                show_message(state, result.failure);
                return;
            }
            if (result.read) {
                // Only while Confirm still asks for this device's serial: the
                // operator may have gone Back, turned the switch off or chosen
                // another source meanwhile; the read stays kept for Start.
                if (wizard.serial_offer && wizard.chosen.record_device_serial && !wizard.serial_offer->kept &&
                    wizard.serial_offer->device.device_ref == result.read->device_ref) {
                    start_registration(state, *result.read);
                } else {
                    show_confirm_serial(state);
                }
                return;
            }
            // Back on Confirm, saying why; Start registration then registers
            // without it. A read cancelled says nothing.
            wizard.serial_problem = read_problem(result.read.error());
            show_confirm_serial(state);
        },
        wizard.serial_cancel);
    if (!started) {
        wizard.reading_serial = false;
        show_confirm_serial(state);
    }
}

void push_confirm(GuiState& state)
{
    auto& wizard = state.register_wizard;
    const auto& text = state.text;
    wizard.chosen = chosen_options(wizard);
    if (options_shown(text, wizard.options, wizard.chosen, wizard.names).blocked) {
        return;
    }
    GtkWidget* content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 18);
    // Decision 0047: why the read of the device's own serial gave nothing,
    // hidden until one did; a new visit to Confirm asks again.
    wizard.serial_problem.reset();
    gtk_box_append(GTK_BOX(content),
        notice_line("va-warning", "dialog-warning-symbolic", "warning", &wizard.serial_notice_words));
    wizard.serial_notice = gtk_widget_get_last_child(content);
    GtkWidget* summary = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(summary), GTK_SELECTION_NONE);
    gtk_widget_add_css_class(summary, "boxed-list");
    wizard.summary = summary;
    gtk_box_append(GTK_BOX(content), summary);
    gtk_box_append(GTK_BOX(content),
        make_words(text("Registration reads every file of the folder or volume and records its fingerprints (BLAKE3 "
                        "and SHA-256) as the baseline that later verifications compare against. Nothing is written to "
                        "the archive unless you ask for a media copy."),
            "dim-label"));
    gtk_box_append(GTK_BOX(content),
        make_words(text("Once it starts, the time left is estimated from the actual read speed. While it registers, "
                        "the computer does not go to sleep by itself; you may leave, and a notification says when it "
                        "ends while the window is not in front. Stopping it halfway is fine: it can be resumed later "
                        "without reading again what it read."),
            "dim-label"));
    GtkWidget* start = gtk_button_new_with_label(text("Start registration").c_str());
    g_signal_connect(start, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) {
        auto& state = *static_cast<GuiState*>(data);
        guarded([&] { start_from_wizard(state); }, [&](const Report& report) { show_message(state, report); });
    }), &state);
    wizard.start = start;
    wizard.starting = gtk_spinner_new();
    wizard.confirm_page = wizard_page(text, "Confirm", 3, content, back_button(state), start, wizard.starting);
    adw_navigation_view_push(ADW_NAVIGATION_VIEW(wizard.navigation), ADW_NAVIGATION_PAGE(wizard.confirm_page));
    show_confirm_serial(state);
    gtk_widget_grab_focus(start);
}

// ---- Choose the source: Next ------------------------------------------------

// What Next found of the source: why it cannot be registered, on the first
// page; otherwise the Options page.
void source_read(GuiState& state, SourceRead& read)
{
    auto& wizard = state.register_wizard;
    const auto& text = state.text;
    wizard.reading_source = false;
    show_source_next(state);
    const auto problem = [&](std::string words) { say(wizard.problem, wizard.problem_words, std::move(words)); };
    if (read.failure) {
        problem(read.failure(text));
        return;
    }
    if (read.refused) {
        problem(text.format("This folder or volume cannot be registered: {}", text(*read.refused)));
        return;
    }
    if (!read.source) {
        problem(text.format("Cannot identify the volume of this folder or volume: {}", text(read.unidentified)));
        return;
    }
    if (read.unread) {
        problem(read.unread(text));
        return;
    }
    auto options = source_options(text, *read.source, read.candidates, read.pending, wizard.names, read.earlier);
    options.next_label = std::move(read.next_label);
    if (options.refusal) {
        problem(*options.refusal);
        return;
    }
    // Create a new revision... of an archive that is not at this root now:
    // its volume may have another drive letter. The operator chooses where it
    // is; the new revision stays asked for until Options shows.
    if (wizard.preset_archive) {
        if (auto refusal = new_revision_refusal(
                text, options, *wizard.preset_archive, wizard.names, path_to_utf8(read.source->root))) {
            problem(*refusal);
            return;
        }
    }
    // What was chosen for the same root before stays; another root starts afresh.
    if (!wizard.source || wizard.source->root != read.source->root) {
        wizard.chosen = OptionsChosen{};
        wizard.chosen.copy_of = default_copy_of(options);
    }
    if (wizard.preset_archive && options.new_revision) {
        wizard.chosen.new_revision = true;
        wizard.chosen.another_archive = false;
    }
    wizard.preset_archive.reset();
    // Register as another archive... of the enclosure question (decision
    // 0047): where the filesystem of that archive is here on another device,
    // a separate archive, a copy of it.
    if (wizard.preset_copy) {
        if (std::ranges::find(options.clones_here, *wizard.preset_copy) != options.clones_here.end()) {
            wizard.chosen.another_archive = true;
            wizard.chosen.new_revision = false;
            wizard.chosen.copy_of = *wizard.preset_copy;
        }
        wizard.preset_copy.reset();
    }
    // Decision 0047: the device's own serial behind its enclosure, where it
    // can be read, or was read before.
    wizard.serial_offer = device_serial_offer(*read.source, options, state.identity_shown,
        state.device_identity && state.device_identity->available(), state.device_reads);
    wizard.source = std::move(read.source);
    wizard.options = std::move(options);
    say(wizard.problem, wizard.problem_words, std::nullopt);
    push_options(state);
}

// Next: reads the folder or volume given, off the window's thread, made into
// a root as the command line makes one typed there (app::typed_root).
void read_source(GuiState& state)
{
    auto& wizard = state.register_wizard;
    if (!state.session || !wizard.folder || wizard.reading_source) {
        return;
    }
    const std::string typed = gtk_editable_get_text(GTK_EDITABLE(wizard.folder));
    if (typed.empty()) {
        return;
    }
    const auto root = app::typed_root(typed);
    wizard.reading_source = true;
    say(wizard.problem, wizard.problem_words, std::nullopt);
    show_source_next(state);
    read_off_thread<SourceRead>(state,
        [root, typed, layout = state.session->context.repo] {
            SourceRead read;
            // Decision 0029: refused before anything of it is read, as on
            // the command line; on Linux this reads the root's filesystem.
            if (auto refusal = app::root_not_taken(root, typed)) {
                read.refused = std::move(*refusal);
                return read;
            }
            const auto media = platform::make_media_enumerator();
            const auto volume = media->volume_for_path(root);
            if (!volume) {
                read.unidentified = volume.error().message;
                return read;
            }
            const auto subpath = root_subpath_of(root, *volume);
            if (!subpath) {
                read.unidentified = subpath.error().message;
                return read;
            }
            ChosenSource source{root, *volume, *subpath, std::nullopt};
            if (subpath->empty()) {
                source.used_bytes = app::volume_used_bytes(volume->mount_point);
            }
            read_archives(layout, read.candidates, read.pending, read.unread);
            if (!read.unread) {
                read_earlier_carriers(layout, source, read);
            }
            read.source = std::move(source);
            return read;
        },
        [opened = wizard.opened](GuiState& state, SourceRead& read) {
            if (is_open(state, opened)) {
                source_read(state, read);
            }
        });
}

void push_source_page(GuiState& state)
{
    auto& wizard = state.register_wizard;
    const auto& text = state.text;
    GtkWidget* content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 18);
    gtk_box_append(GTK_BOX(content),
        make_words(text("Choose a volume to register it as a whole, or a folder in one."), "dim-label"));

    wizard.volumes = adw_preferences_group_new();
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(wizard.volumes), text("Attached volumes").c_str());
    GtkWidget* again = gtk_button_new_from_icon_name("view-refresh-symbolic");
    gtk_widget_add_css_class(again, "flat");
    gtk_widget_set_valign(again, GTK_ALIGN_CENTER);
    gtk_widget_set_tooltip_text(again, text("Read the attached volumes again").c_str());
    gtk_accessible_update_property(GTK_ACCESSIBLE(again), GTK_ACCESSIBLE_PROPERTY_LABEL,
        text("Read the attached volumes again").c_str(), -1);
    g_signal_connect(again, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) {
        auto& state = *static_cast<GuiState*>(data);
        guarded([&] { read_volumes(state); }, [&](const Report& report) { show_message(state, report); });
    }), &state);
    adw_preferences_group_set_header_suffix(ADW_PREFERENCES_GROUP(wizard.volumes), again);
    gtk_box_append(GTK_BOX(content), wizard.volumes);

    // A folder, chosen or typed: also a volume's GUID path, which the folder
    // chooser may not offer, or a root that a refusal named.
    GtkWidget* folders = adw_preferences_group_new();
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(folders), text("Folder").c_str());
    adw_preferences_group_set_description(ADW_PREFERENCES_GROUP(folders),
        text("You can also type or paste a folder or volume, including a volume's GUID path.").c_str());
    wizard.folder = adw_entry_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(wizard.folder), text("Folder or volume").c_str());
    GtkWidget* choose = gtk_button_new_with_label(text("Choose a folder...").c_str());
    gtk_widget_set_valign(choose, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(choose, "flat");
    g_signal_connect(choose, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) {
        auto& state = *static_cast<GuiState*>(data);
        guarded(
            [&] {
                if (state.window && state.register_wizard.folder) {
                    choose_folder(state.window, state.register_wizard.folder,
                        state.text("Choose the archive folder or volume"));
                }
            },
            [&](const Report& report) { show_message(state, report); });
    }), &state);
    adw_entry_row_add_suffix(ADW_ENTRY_ROW(wizard.folder), choose);
    g_signal_connect(wizard.folder, "changed", G_CALLBACK(+[](GtkEditable* editable, gpointer data) {
        auto& state = *static_cast<GuiState*>(data);
        auto& wizard = state.register_wizard;
        if (wizard.filling) {
            return;
        }
        // The operator's own choice: Create a new revision... no longer
        // types the archive's root once the volumes were read.
        wizard.preset_root.reset();
        // Another root than the volume chosen: no volume is chosen.
        const std::string typed = gtk_editable_get_text(editable);
        wizard.filling = true;
        for (std::size_t index = 0; index < wizard.radios.size(); ++index) {
            if (wizard.radio_roots[index] != typed &&
                gtk_check_button_get_active(GTK_CHECK_BUTTON(wizard.radios[index]))) {
                gtk_check_button_set_active(GTK_CHECK_BUTTON(wizard.radios[index]), FALSE);
            }
        }
        wizard.filling = false;
        say(wizard.problem, wizard.problem_words, std::nullopt);
        show_source_next(state);
    }), &state);
    g_signal_connect(wizard.folder, "entry-activated", G_CALLBACK(+[](AdwEntryRow*, gpointer data) {
        auto& state = *static_cast<GuiState*>(data);
        guarded([&] { read_source(state); }, [&](const Report& report) { show_message(state, report); });
    }), &state);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(folders), wizard.folder);
    gtk_box_append(GTK_BOX(content), folders);
    gtk_box_append(GTK_BOX(content), notice_line("va-error", "dialog-warning-symbolic", "error", &wizard.problem_words));
    wizard.problem = gtk_widget_get_last_child(content);

    GtkWidget* cancel = gtk_button_new_with_label(text("Cancel").c_str());
    g_signal_connect(cancel, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) {
        auto& state = *static_cast<GuiState*>(data);
        if (state.register_wizard.dialog) {
            adw_dialog_close(ADW_DIALOG(state.register_wizard.dialog));
        }
    }), &state);
    wizard.source_next = gtk_button_new_with_label(text("Next").c_str());
    g_signal_connect(wizard.source_next, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) {
        auto& state = *static_cast<GuiState*>(data);
        guarded([&] { read_source(state); }, [&](const Report& report) { show_message(state, report); });
    }), &state);
    wizard.reading = gtk_spinner_new();
    wizard.source_page =
        wizard_page(text, "Choose the source", 1, content, cancel, wizard.source_next, wizard.reading);
    adw_navigation_view_add(ADW_NAVIGATION_VIEW(wizard.navigation), ADW_NAVIGATION_PAGE(wizard.source_page));
    show_source_next(state);
}

// Opens the dialog on its first page and reads the attached volumes; for
// Create a new revision... of `preset_archive`, then the source where they
// hold it, or `preset_root`.
void open_wizard(GuiState& state, std::optional<Uuid> preset_archive = std::nullopt,
    std::optional<std::string> preset_root = std::nullopt)
{
    if (!state.window) {
        return;
    }
    if (!state.session) {
        show_message(state, fixed_message("Open a repository first (Settings)."));
        return;
    }
    close_dialogs(state);
    auto& wizard = state.register_wizard;
    wizard = {};
    wizard.opened = ++state.wizards_opened;
    wizard.preset_archive = std::move(preset_archive);
    wizard.preset_root = std::move(preset_root);
    // As Archives names them, for the volumes' rows and Copy of.
    wizard.names = archive_names(state.archives_page.statuses, state.identity_shown);
    const auto& text = state.text;
    AdwDialog* dialog = adw_dialog_new();
    wizard.dialog = GTK_WIDGET(dialog);
    adw_dialog_set_title(dialog, text("Register Archive").c_str());
    adw_dialog_set_content_width(dialog, 680);
    adw_dialog_set_content_height(dialog, 640);
    wizard.navigation = adw_navigation_view_new();
    // A page popped (Back, Escape's own back) is gone, with its widgets.
    g_signal_connect(wizard.navigation, "popped", G_CALLBACK(+[](AdwNavigationView*, AdwNavigationPage* page,
                                                                  gpointer data) {
        auto& wizard = static_cast<GuiState*>(data)->register_wizard;
        if (GTK_WIDGET(page) == wizard.confirm_page) {
            wizard.confirm_page = wizard.start = nullptr;
            wizard.serial_notice = wizard.serial_notice_words = wizard.summary = wizard.starting = nullptr;
        } else if (GTK_WIDGET(page) == wizard.options_page) {
            wizard.options_page = nullptr;
            wizard.label = wizard.label_group = wizard.new_revision = wizard.another_archive = nullptr;
            wizard.resume = wizard.media_copy = wizard.copy_of = nullptr;
            wizard.blocked = wizard.blocked_words = wizard.options_next = nullptr;
            wizard.device_serial = wizard.device_serial_kept = nullptr;
        }
    }), &state);
    adw_dialog_set_child(dialog, wizard.navigation);
    push_source_page(state);
    // Closed, by its buttons, by Escape, or with the window: nothing of it
    // stays; the reads in flight find it gone.
    g_signal_connect(dialog, "closed", G_CALLBACK(+[](AdwDialog* closed, gpointer data) {
        auto& state = *static_cast<GuiState*>(data);
        if (state.register_wizard.dialog == GTK_WIDGET(closed)) {
            // A read of the device's own serial stops once Windows' question
            // is answered (decision 0047).
            if (state.register_wizard.serial_cancel) {
                state.register_wizard.serial_cancel->cancel();
            }
            state.register_wizard = {};
        }
    }), &state);
    adw_dialog_present(dialog, GTK_WIDGET(state.window));
    gtk_widget_grab_focus(wizard.folder);
    read_volumes(state);
}

} // namespace

void open_register_wizard(GuiState& state)
{
    open_wizard(state);
}

void register_new_revision(GuiState& state, const Uuid& archive, const std::string& root)
{
    // The volumes are read first (open_wizard), then the source where they
    // hold the archive (read_preset_source).
    open_wizard(state, archive, root);
}

void register_volume(GuiState& state, const std::string& root)
{
    open_wizard(state);
    auto& wizard = state.register_wizard;
    if (!wizard.dialog || !wizard.folder) {
        return;  // not opened: it said why
    }
    // Typed as the volume's root: once the volumes are read, its row is
    // chosen (show_volumes); Next reads it as any root typed.
    wizard.filling = true;
    gtk_editable_set_text(GTK_EDITABLE(wizard.folder), root.c_str());
    wizard.filling = false;
    show_source_next(state);
}

void register_as_copy(GuiState& state, const std::string& root, const Uuid& archive)
{
    open_wizard(state);
    auto& wizard = state.register_wizard;
    if (!wizard.dialog || !wizard.folder) {
        return;  // not opened: it said why
    }
    // Typed as the root where its filesystem is, and read at once, as Next
    // reads it: Options then shows it as another archive, a copy of it.
    wizard.preset_copy = archive;
    wizard.filling = true;
    gtk_editable_set_text(GTK_EDITABLE(wizard.folder), root.c_str());
    wizard.filling = false;
    show_source_next(state);
    read_source(state);
}

void resume_registration(GuiState& state, const app::UnfinishedOperation& registration, bool confirmed)
{
    // Exactly `register <root> --resume`, the root its plan names: no media
    // copy (decision 0022, point 7). A resume with a media copy is started in
    // Register Archive, whose options offer it beside the resume once the
    // root is chosen there. With --confirm where the operator confirmed it in
    // the question about an older version's registration.
    run_registration(state, app::registration_resume(registration, state.session->context.config, confirmed),
        registration.root);
}

void close_dialogs(GuiState& state)
{
    // Each forgets its widgets at once: a dialog closes once its animation
    // has ended, and the window it belongs to may be going first.
    auto wizard = std::exchange(state.register_wizard, {});
    if (wizard.serial_cancel) {
        wizard.serial_cancel->cancel();
    }
    if (wizard.dialog) {
        adw_dialog_force_close(ADW_DIALOG(wizard.dialog));
    }
    if (GtkWidget* compare = std::exchange(state.compare_dialog, {}).dialog) {
        adw_dialog_force_close(ADW_DIALOG(compare));
    }
    if (GtkWidget* relocation = std::exchange(state.relocate_dialog, {}).dialog) {
        adw_dialog_force_close(ADW_DIALOG(relocation));
    }
    // Decision 0047: its read, if one waits, stops once Windows' question is
    // answered.
    auto enclosure = std::exchange(state.enclosure_dialog, {});
    if (enclosure.cancel) {
        enclosure.cancel->cancel();
    }
    if (enclosure.dialog) {
        adw_dialog_force_close(ADW_DIALOG(enclosure.dialog));
    }
}

} // namespace va::gui
