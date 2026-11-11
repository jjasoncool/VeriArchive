// A device's own identity behind its enclosure (decision 0047), in the
// window: the enclosure question that FULL verify of an archive whose device
// is seen behind another enclosure asks first (enclosure_flow.h words its
// steps), and the reads of a device's own serial that it, Devices and
// Register Archive ask for. The main program never holds administrator
// permission (R-08, R-12): a read starts the device identity reader, which
// Windows starts only once the operator allows it in its own question; the
// read waits for that answer and for the reader off the window's thread, so
// the window is never held up, one read at a time. A read that gave the
// device's identity is kept while the system describes the device as it did
// (device_reads.h), for Devices and Register Archive; the question itself
// always reads the device anew.
#include "gui/gui_app.h"
#include "gui/off_thread.h"

#include "app/enclosure_words.h"
#include "common/utf8.h"
#include "core/engine/carrier_confirmation.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace va::gui {
namespace {

// The archives whose confirmed device `device` is (confirmed_device), read
// from the repository on the read's thread; none where it cannot be read.
std::vector<Uuid> confirmed_owners(const RepositoryLayout& layout, const DeviceIdentity& device)
{
    std::vector<Uuid> owners;
    auto store = ArchiveStore::open_read_only(layout);
    if (!store) {
        return owners;
    }
    const auto archives = store->archives();
    if (!archives) {
        return owners;
    }
    for (const auto& archive : *archives) {
        if (const auto confirmed = read_confirmed_device(*store, archive.archive_uuid);
            confirmed && *confirmed && same_device_identity(device, (*confirmed)->device)) {
            owners.push_back(archive.archive_uuid);
        }
    }
    return owners;
}

// ---- The enclosure question -------------------------------------------------

bool is_open(const GuiState& state, std::uint64_t opened)
{
    return state.enclosure_dialog.dialog && state.enclosure_dialog.opened == opened;
}

// The dialog is as wide as its paragraphs, which wrap at about this many
// characters (a Chinese character counts about twice), or as its answers in
// a row, where the window is wide enough.
constexpr int paragraph_chars = 62;

// ---- Its answers: in a row where they fit, else one under another ----------
//
// As AdwAlertDialog lays out its responses: in one row at the end where the
// dialog is wide enough for all of them, each as wide as its words; otherwise
// (a narrow window, which may be 360 px wide, or answers too long for the
// dialog) one under another, each as wide as the dialog, the last one (the
// suggested one, where one is) on top. Never wider than the widest answer.

constexpr int answer_spacing = 12;

std::vector<GtkWidget*> answers_of(GtkWidget* box)
{
    std::vector<GtkWidget*> answers;
    for (GtkWidget* child = gtk_widget_get_first_child(box); child; child = gtk_widget_get_next_sibling(child)) {
        if (gtk_widget_should_layout(child)) {
            answers.push_back(child);
        }
    }
    return answers;
}

int natural_width(GtkWidget* widget)
{
    int least = 0;
    int natural = 0;
    gtk_widget_measure(widget, GTK_ORIENTATION_HORIZONTAL, -1, &least, &natural, nullptr, nullptr);
    return natural;
}

// The width of the answers side by side, each as wide as its words.
int row_width(const std::vector<GtkWidget*>& answers)
{
    int width = 0;
    for (GtkWidget* answer : answers) {
        width += natural_width(answer);
    }
    return answers.empty() ? 0 : width + answer_spacing * static_cast<int>(answers.size() - 1);
}

GtkSizeRequestMode answers_request_mode(GtkWidget*)
{
    return GTK_SIZE_REQUEST_HEIGHT_FOR_WIDTH;
}

void measure_answers(GtkWidget* box, GtkOrientation orientation, int for_size, int* minimum, int* natural,
    int* minimum_baseline, int* natural_baseline)
{
    const auto answers = answers_of(box);
    int least = 0;
    int wanted = 0;
    if (orientation == GTK_ORIENTATION_HORIZONTAL) {
        for (GtkWidget* answer : answers) {
            int answer_least = 0;
            int answer_natural = 0;
            gtk_widget_measure(answer, GTK_ORIENTATION_HORIZONTAL, -1, &answer_least, &answer_natural, nullptr, nullptr);
            least = std::max(least, answer_least);
        }
        wanted = std::max(least, row_width(answers));
    } else {
        const bool stacked = for_size >= 0 && for_size < row_width(answers);
        for (GtkWidget* answer : answers) {
            int answer_least = 0;
            int answer_natural = 0;
            gtk_widget_measure(
                answer, GTK_ORIENTATION_VERTICAL, stacked ? for_size : -1, &answer_least, &answer_natural, nullptr, nullptr);
            least = stacked ? least + answer_least : std::max(least, answer_least);
            wanted = stacked ? wanted + answer_natural : std::max(wanted, answer_natural);
        }
        if (stacked && !answers.empty()) {
            least += answer_spacing * static_cast<int>(answers.size() - 1);
            wanted += answer_spacing * static_cast<int>(answers.size() - 1);
        }
    }
    if (minimum) {
        *minimum = least;
    }
    if (natural) {
        *natural = wanted;
    }
    if (minimum_baseline) {
        *minimum_baseline = -1;
    }
    if (natural_baseline) {
        *natural_baseline = -1;
    }
}

void allocate_answers(GtkWidget* box, int width, int height, int)
{
    const auto answers = answers_of(box);
    const int row = row_width(answers);
    if (width >= row) {
        int x = width - row;
        for (GtkWidget* answer : answers) {
            const int wide = natural_width(answer);
            const GtkAllocation place{x, 0, wide, height};
            gtk_widget_size_allocate(answer, &place, -1);
            x += wide + answer_spacing;
        }
        return;
    }
    int y = 0;
    for (auto answer = answers.rbegin(); answer != answers.rend(); ++answer) {
        int least = 0;
        int tall = 0;
        gtk_widget_measure(*answer, GTK_ORIENTATION_VERTICAL, width, &least, &tall, nullptr, nullptr);
        const GtkAllocation place{0, y, width, tall};
        gtk_widget_size_allocate(*answer, &place, -1);
        y += tall + answer_spacing;
    }
}

// The box of the answers, laid out as above: gtk_box_append and
// gtk_box_remove only give it its children; its own layout is replaced.
GtkWidget* answers_box()
{
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_layout_manager(box, gtk_custom_layout_new(answers_request_mode, measure_answers, allocate_answers));
    return box;
}

GtkWidget* paragraph(const std::string& words, const char* style = nullptr, bool selectable = false)
{
    GtkWidget* label = make_words(words, style, selectable);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
    gtk_label_set_max_width_chars(GTK_LABEL(label), paragraph_chars);
    return label;
}

// Closes the question at once, its read (if one waits) told to stop once
// Windows' question is answered; it is forgotten before its animation ends.
void forget_dialog(GuiState& state)
{
    auto closing = std::exchange(state.enclosure_dialog, {});
    if (closing.cancel) {
        closing.cancel->cancel();
    }
    if (closing.dialog) {
        adw_dialog_close(ADW_DIALOG(closing.dialog));
    }
}

void show_step(GuiState& state);
void act(GuiState& state, EnclosureNext next);

struct AnswerGiven {
    GuiState* state;
    std::string id;
};

GtkWidget* answer_button(GuiState& state, const DialogAnswer& answer)
{
    GtkWidget* button = gtk_button_new();
    if (answer.reads) {
        // What starts the reader, which Windows asks administrator
        // permission for, has the shield beside it.
        GtkWidget* content = adw_button_content_new();
        adw_button_content_set_icon_name(ADW_BUTTON_CONTENT(content), "security-high-symbolic");
        adw_button_content_set_label(ADW_BUTTON_CONTENT(content), answer.label.c_str());
        gtk_button_set_child(GTK_BUTTON(button), content);
    } else {
        gtk_button_set_label(GTK_BUTTON(button), answer.label.c_str());
    }
    if (answer.suggested) {
        gtk_widget_add_css_class(button, "suggested-action");
    }
    g_signal_connect_data(button, "clicked",
        G_CALLBACK(+[](GtkButton*, gpointer data) {
            const auto* given = static_cast<const AnswerGiven*>(data);
            auto& state = *given->state;
            // A copy: the step made anew takes this button away.
            const auto id = given->id;
            guarded(
                [&] {
                    auto& dialog = state.enclosure_dialog;
                    if (dialog.dialog) {
                        act(state, answered(dialog.facts, dialog.flow, id));
                    }
                },
                [&](const Report& report) { show_message(state, report); });
        }),
        new AnswerGiven{&state, answer.id}, +[](gpointer data, GClosure*) { delete static_cast<AnswerGiven*>(data); },
        GConnectFlags{});
    return button;
}

// Makes the step of state.enclosure_dialog.flow anew.
void show_step(GuiState& state)
{
    auto& dialog = state.enclosure_dialog;
    if (!dialog.dialog) {
        return;
    }
    const auto& text = state.text;
    // A read only of a device whose enclosure hides its serial, where this
    // computer can read it: of one that reports its own serial, a read would
    // only repeat it (and an enclosure that passes nothing on is no advice
    // for a USB flash drive).
    const auto offer = dialog.found.now.devices.size() == 1
                           ? serial_read_offer(dialog.found.now.devices.front(),
                                 state.device_identity && state.device_identity->available())
                           : SerialRead::NoReader;
    const auto shown = enclosure_dialog(text, dialog.facts, dialog.found.where, dialog.flow, offer);
    for (GtkWidget* box : {dialog.content, dialog.answers}) {
        while (GtkWidget* child = gtk_widget_get_first_child(box)) {
            gtk_box_remove(GTK_BOX(box), child);
        }
    }
    // Its title, which a screen reader says, is the step's heading.
    adw_dialog_set_title(ADW_DIALOG(dialog.dialog), shown.heading.c_str());
    gtk_box_append(GTK_BOX(dialog.content), paragraph(shown.heading, "title-2"));
    const auto fields = [&] {
        GtkWidget* list = gtk_list_box_new();
        gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_NONE);
        gtk_widget_add_css_class(list, "boxed-list");
        gtk_list_box_append(GTK_LIST_BOX(list), make_fields_row(shown.fields));
        gtk_box_append(GTK_BOX(dialog.content), list);
    };
    for (std::size_t index = 0; index < shown.paragraphs.size(); ++index) {
        if (index == shown.fields_after && !shown.fields.empty()) {
            fields();
        }
        // The serials in them can be copied.
        gtk_box_append(GTK_BOX(dialog.content), paragraph(shown.paragraphs[index], nullptr, true));
    }
    if (shown.fields_after >= shown.paragraphs.size() && !shown.fields.empty()) {
        fields();
    }
    if (shown.problem) {
        GtkWidget* problem = make_line_box("va-warning", "dialog-warning-symbolic", "warning");
        GtkWidget* words = paragraph(*shown.problem);
        gtk_widget_set_hexpand(words, TRUE);
        gtk_box_append(GTK_BOX(problem), words);
        gtk_box_append(GTK_BOX(dialog.content), problem);
    }
    if (!shown.note.empty()) {
        GtkWidget* note = paragraph(shown.note, "dim-label");
        gtk_widget_add_css_class(note, "caption");
        gtk_box_append(GTK_BOX(dialog.content), note);
    }
    if (shown.reading) {
        GtkWidget* spinner = gtk_spinner_new();
        gtk_spinner_start(GTK_SPINNER(spinner));
        gtk_widget_set_size_request(spinner, 32, 32);
        gtk_widget_set_halign(spinner, GTK_ALIGN_CENTER);
        gtk_box_append(GTK_BOX(dialog.content), spinner);
    }
    GtkWidget* cancel = nullptr;
    for (const auto& answer : shown.answers) {
        GtkWidget* button = answer_button(state, answer);
        if (!cancel) {
            cancel = button;
        }
        gtk_box_append(GTK_BOX(dialog.answers), button);
    }
    // Cancel is the default, Enter's answer; Escape closes the dialog.
    if (cancel) {
        adw_dialog_set_default_widget(ADW_DIALOG(dialog.dialog), cancel);
        adw_dialog_set_focus(ADW_DIALOG(dialog.dialog), cancel);
    }
}

// Reads the device's own serial, the dialog showing that it waits meanwhile.
void start_read(GuiState& state)
{
    auto& dialog = state.enclosure_dialog;
    if (dialog.found.now.devices.size() != 1) {
        dialog.flow = EnclosureFlow{};
        show_step(state);
        return;
    }
    dialog.cancel = std::make_shared<CancelToken>();
    const auto started = read_own_identity(state, dialog.found.now.devices.front(),
        [opened = dialog.opened](GuiState& state, OwnIdentityRead& result) {
            if (!is_open(state, opened)) {
                return;  // closed meanwhile: the read is kept, shown in Devices
            }
            state.enclosure_dialog.cancel.reset();
            if (result.failure) {
                state.enclosure_dialog.flow = EnclosureFlow{};
                show_step(state);
                show_message(state, result.failure);
                return;
            }
            act(state, after_read(state.enclosure_dialog.facts, result.read));
        },
        dialog.cancel);
    if (!started) {
        // Another read waits: back to the question, which says nothing new.
        dialog.cancel.reset();
        dialog.flow = EnclosureFlow{};
        show_step(state);
    }
}

void act(GuiState& state, EnclosureNext next)
{
    auto& dialog = state.enclosure_dialog;
    switch (next.then) {
    case EnclosureNext::Then::Show:
        dialog.flow = std::move(next.flow);
        show_step(state);
        return;
    case EnclosureNext::Then::Read:
        dialog.flow = std::move(next.flow);
        show_step(state);
        start_read(state);
        return;
    case EnclosureNext::Then::Verify: {
        const auto archive = dialog.archive;
        const bool strict = dialog.strict;
        forget_dialog(state);
        if (next.confirmation) {
            const auto by_serial = next.confirmation->how == CarrierConfirmation::How::DeviceSerial;
            const auto read = next.confirmation->device;
            // Started with no question: the toast says why; not where it did
            // not start (a job began on the archive meanwhile), as said.
            if (verify_confirmed(state, archive, strict, std::move(*next.confirmation)) && by_serial && read) {
                show_toast(state, app::same_device_started(state.text, *read), std::nullopt, ToastKind::Started);
            }
        }
        return;
    }
    case EnclosureNext::Then::Register: {
        const auto archive = dialog.archive;
        const auto root = path_to_utf8(dialog.found.root);
        forget_dialog(state);
        register_as_copy(state, root, archive);
        return;
    }
    case EnclosureNext::Then::Close:
        forget_dialog(state);
        return;
    }
}

// The enclosure question for `archive`, its step `flow` shown, not presented:
// the dialog of state.enclosure_dialog from now on.
AdwDialog* make_dialog(GuiState& state, const Uuid& archive, bool strict, const EnclosureCase& found,
    const app::EnclosureFacts& facts, EnclosureFlow flow)
{
    const auto& text = state.text;
    auto& dialog = state.enclosure_dialog;
    dialog = {};
    dialog.archive = archive;
    dialog.strict = strict;
    dialog.found = found;
    dialog.facts = facts;
    dialog.flow = std::move(flow);
    dialog.opened = ++state.enclosures_opened;
    AdwDialog* shown = adw_dialog_new();
    dialog.dialog = GTK_WIDGET(shown);
    adw_dialog_set_title(shown, app::enclosure_question_title(text, found.where).c_str());
    // As tall as the step shown, which a later step may make shorter; as
    // wide as its paragraphs (paragraph_chars) or its answers in a row,
    // within the window.
    adw_dialog_set_follows_content_size(shown, TRUE);

    GtkWidget* view = adw_toolbar_view_new();
    GtkWidget* header = adw_header_bar_new();
    // The heading is the step's own, in the content.
    adw_header_bar_set_show_title(ADW_HEADER_BAR(header), FALSE);
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(view), header);
    dialog.content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_start(dialog.content, 24);
    gtk_widget_set_margin_end(dialog.content, 24);
    gtk_widget_set_margin_bottom(dialog.content, 6);
    GtkWidget* scroller = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scroller), TRUE);
    gtk_scrolled_window_set_propagate_natural_width(GTK_SCROLLED_WINDOW(scroller), TRUE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), dialog.content);
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(view), scroller);
    // In a row at the end where they fit, else one under another.
    dialog.answers = answers_box();
    gtk_widget_set_margin_start(dialog.answers, 24);
    gtk_widget_set_margin_end(dialog.answers, 24);
    gtk_widget_set_margin_top(dialog.answers, 12);
    gtk_widget_set_margin_bottom(dialog.answers, 18);
    adw_toolbar_view_add_bottom_bar(ADW_TOOLBAR_VIEW(view), dialog.answers);
    adw_dialog_set_child(shown, view);
    // Closed, by Cancel, by Escape, or with the window: its read, if one
    // waits, stops once Windows' question is answered.
    g_signal_connect(shown, "closed", G_CALLBACK(+[](AdwDialog* closed, gpointer data) {
        auto& state = *static_cast<GuiState*>(data);
        if (state.enclosure_dialog.dialog == GTK_WIDGET(closed)) {
            if (state.enclosure_dialog.cancel) {
                state.enclosure_dialog.cancel->cancel();
            }
            state.enclosure_dialog = {};
        }
    }), &state);
    show_step(state);
    return shown;
}

// The archive as the list names it (display_label), or by the start of its uuid.
std::string label_in_list(const GuiState& state, const Uuid& archive)
{
    for (const auto& status : state.archives_page.statuses) {
        if (status.archive.archive_uuid == archive) {
            return display_label(status);
        }
    }
    return archive.substr(0, 8);
}

} // namespace

bool run_reader(GuiState& state, std::vector<PhysicalDeviceInfo> devices, std::string reading,
    std::function<void(GuiState&, ReaderRun&)> done, std::shared_ptr<CancelToken> cancel)
{
    if (state.device_reading) {
        show_message(state, fixed_message("Another read with administrator permission is waiting for the User "
                                          "Account Control prompt or for its devices; answer it or let it finish "
                                          "first."));
        return false;
    }
    if (!state.device_identity || devices.empty()) {
        return false;
    }
    if (!cancel) {
        cancel = std::make_shared<CancelToken>();
    }
    const auto id = ++state.device_reads_started;
    state.device_reading = GuiState::DeviceReading{reading, cancel, id};
    // The reader outlives every read in flight (GuiState::device_identity).
    auto* reader = state.device_identity.get();
    const auto layout = state.session ? std::optional(state.session->context.repo) : std::nullopt;
    std::vector<std::string> refs;
    for (const auto& device : devices) {
        refs.push_back(device.device_ref);
    }
    read_off_thread<ReaderRun>(state,
        [reader, refs, cancel, layout] {
            ReaderRun run;
            run.reads = reader->read_devices(refs, *cancel);
            if (!run.reads || !layout) {
                return run;
            }
            for (const auto& read : *run.reads) {
                if (read.identity) {
                    run.confirmed_of[read.device_ref] = confirmed_owners(*layout, read.identity->device);
                }
                if (read.smart) {
                    run.recorded.push_back(*read.smart);
                }
            }
            // Decision 0052: every SMART read is recorded, a failed one too.
            if (!run.recorded.empty()) {
                auto store = ArchiveStore::open(*layout);
                auto recorded = store ? store->record_smart_reads(run.recorded)
                                      : std::expected<void, IoError>(std::unexpected(store.error()));
                if (!recorded) {
                    run.not_recorded = recorded.error().message;
                    run.recorded.clear();
                }
            }
            return run;
        },
        [id, devices = std::move(devices), done = std::move(done)](GuiState& state, ReaderRun& run) {
            if (state.device_reading && state.device_reading->id == id) {
                state.device_reading.reset();
            }
            if (run.reads) {
                // Decision 0052: for a FULL verification started next.
                state.recent_smart.clear();
                for (const auto& read : *run.reads) {
                    if (read.smart) {
                        state.recent_smart.push_back(*read.smart);
                    }
                }
                state.recent_smart_at = std::chrono::steady_clock::now();
                for (const auto& read : *run.reads) {
                    const auto device = std::ranges::find(devices, read.device_ref, &PhysicalDeviceInfo::device_ref);
                    // The one asked for, and every other that its enclosure
                    // hides the identity of, as Devices shows them.
                    if (read.identity && device != devices.end() &&
                        (device == devices.begin() || device->enclosure_hides_identity)) {
                        state.device_reads[read.device_ref] =
                            KeptDeviceRead{*read.identity, *device, run.confirmed_of[read.device_ref]};
                    }
                }
            }
            if (!run.recorded.empty()) {
                state.smart_reads.insert(state.smart_reads.end(), run.recorded.begin(), run.recorded.end());
                refresh_archives(state);
            }
            refresh_devices(state);
            if (done) {
                done(state, run);
            }
            if (run.not_recorded) {
                show_message(state, [message = *run.not_recorded](const app::Text& text) {
                    return text.format("The SMART reads could not be recorded: {}", text(message));
                });
            }
        });
    refresh_devices(state);
    return true;
}

bool read_own_identity(GuiState& state, const PhysicalDeviceInfo& device,
    std::function<void(GuiState&, OwnIdentityRead&)> done, std::shared_ptr<CancelToken> cancel)
{
    // Decision 0052: the one asked for, then every other attached device,
    // whose SMART is read under the same question of Windows.
    std::vector<PhysicalDeviceInfo> devices{device};
    if (const auto* read = attached_now(state)) {
        for (const auto& attached : read->devices) {
            if (!attached.device.device_ref.empty() && attached.device.device_ref != device.device_ref) {
                devices.push_back(attached.device);
            }
        }
    }
    return run_reader(state, std::move(devices), device.device_ref,
        [device_ref = device.device_ref, done = std::move(done)](GuiState& state, ReaderRun& run) {
            OwnIdentityRead result;
            result.failure = run.failure;
            if (!run.reads) {
                result.read = std::unexpected(run.reads.error());
            } else {
                const auto read = std::ranges::find(*run.reads, device_ref, &DeviceRead::device_ref);
                if (read == run.reads->end()) {
                    result.read = std::unexpected(
                        IoError{IoErrorKind::Other, 0, "the device identity reader read no such device"});
                } else {
                    result.read = read->identity;
                    result.confirmed_of = run.confirmed_of[device_ref];
                }
            }
            if (done) {
                done(state, result);
            }
        },
        std::move(cancel));
}

void ask_about_enclosure(GuiState& state, const Uuid& archive, bool strict, const EnclosureCase& found)
{
    if (!state.window || !state.session || found.now.devices.size() != 1) {
        return;
    }
    // The archive's confirmed device, from its history and its media
    // evidence (the settings only cache it).
    auto store = ArchiveStore::open_read_only(state.session->context.repo);
    if (!store) {
        show_message(state, [message = store.error().message](const app::Text& text) {
            return text.format("Cannot open the repository: {}", text(message));
        });
        return;
    }
    const auto confirmed = read_confirmed_device(*store, archive);
    if (!confirmed) {
        show_message(state, [message = confirmed.error().message](const app::Text& text) {
            return text.format("Cannot read the confirmed device of this archive: {}", text(message));
        });
        return;
    }
    const auto facts = app::enclosure_facts(label_in_list(state, archive), found.recorded, found.now, *confirmed);
    // Always the question, whose read reads the device anew: a read kept
    // from before (Devices, an earlier question) cannot tell that the device
    // behind this enclosure is still the one read, as another of its model
    // put in the same enclosure or dock since is described alike.
    close_dialogs(state);
    AdwDialog* shown = make_dialog(state, archive, strict, found, facts, EnclosureFlow{});
    adw_dialog_present(shown, GTK_WIDGET(state.window));
}

void check_sample_enclosure_dialog(GuiState& state)
{
    // SAMPLE DATA, made up for the check: an archive recorded behind the
    // enclosure SAMPLE-ENCLOSURE-A, seen behind SAMPLE-ENCLOSURE-B (which
    // hides the device's own serial), without and with a confirmed device,
    // and a read of SAMPLE-OWN-SERIAL-2.
    PhysicalDeviceInfo device;
    device.device_ref = "SAMPLE-DEVICE-REF-CHECK";
    device.model = "SAMPLE-BRIDGE";
    device.serial = "SAMPLE-ENCLOSURE-A";
    device.bus = "usb";
    device.enclosure_hides_identity = true;
    VolumeInfo recorded;
    recorded.filesystem_uuid = "c0ffee00-0000-4000-8000-0000000047c1";
    recorded.volume_label = "SAMPLE-VOLUME";
    recorded.devices = {device};
    auto now = recorded;
    now.mount_point = std::filesystem::path("/media/SAMPLE-CHECK");
    now.devices.front().serial = "SAMPLE-ENCLOSURE-B";
    const EnclosureCase found{"/media/SAMPLE-CHECK", now.mount_point / "SAMPLE", recorded, now};
    const ConfirmedDevice confirmed{DeviceIdentity{"SAMPLE-OWN-MODEL", "SAMPLE-OWN-SERIAL-1", ""}, "", ""};
    const DeviceIdentity read{"SAMPLE-OWN-MODEL", "SAMPLE-OWN-SERIAL-2", "SAMPLE-OWN-WWN-2"};
    const ReadProblem declined{DeviceIdentityFailure::Declined, ""};
    const std::vector<EnclosureFlow> flows{EnclosureFlow{}, EnclosureFlow{EnclosureStep::Question, std::nullopt, declined},
        EnclosureFlow{EnclosureStep::Reading, std::nullopt, std::nullopt},
        EnclosureFlow{EnclosureStep::OperatorConfirmation, std::nullopt, std::nullopt},
        EnclosureFlow{EnclosureStep::DeviceRead, read, std::nullopt},
        EnclosureFlow{EnclosureStep::OtherDevice, read, std::nullopt}};
    const auto saved = std::exchange(state.enclosure_dialog, {});
    for (const auto& known : {std::optional<ConfirmedDevice>{}, std::optional(confirmed)}) {
        const auto facts = app::enclosure_facts("ARCH-SAMPLE", recorded, now, known);
        // Made, never shown: no answer is given, nothing is read or verified.
        AdwDialog* dialog = make_dialog(state, "c0ffee00-0000-4000-8000-0000000047c0", false, found, facts, {});
        g_object_ref_sink(dialog);
        for (const auto& flow : flows) {
            state.enclosure_dialog.flow = flow;
            show_step(state);
            if (!gtk_widget_get_first_child(state.enclosure_dialog.content) ||
                !gtk_widget_get_first_child(state.enclosure_dialog.answers)) {
                state.enclosure_dialog = saved;
                g_object_unref(dialog);
                throw std::runtime_error("the enclosure question shows nothing for one of its steps");
            }
            // Its answers: in a row as wide as they are, or, as narrow as the
            // widest of them, one under another, taller.
            GtkWidget* answers = state.enclosure_dialog.answers;
            int least = 0;
            int natural = 0;
            int unused = 0;
            int row_height = 0;
            int stacked_height = 0;
            gtk_widget_measure(answers, GTK_ORIENTATION_HORIZONTAL, -1, &least, &natural, nullptr, nullptr);
            gtk_widget_measure(answers, GTK_ORIENTATION_VERTICAL, natural, &unused, &row_height, nullptr, nullptr);
            gtk_widget_measure(answers, GTK_ORIENTATION_VERTICAL, least, &unused, &stacked_height, nullptr, nullptr);
            if (least <= 0 || least > natural ||
                (answers_of(answers).size() > 1 && (least >= natural || stacked_height <= row_height))) {
                state.enclosure_dialog = saved;
                g_object_unref(dialog);
                throw std::runtime_error("the enclosure question's answers do not stack where the dialog is narrow");
            }
        }
        state.enclosure_dialog = {};
        g_object_unref(dialog);
    }
    state.enclosure_dialog = saved;
}

void read_device_serial(GuiState& state, const std::string& device_ref)
{
    const auto* read = attached_now(state);
    const PhysicalDeviceInfo* device = nullptr;
    if (read) {
        for (const auto& attached : read->devices) {
            if (attached.device.device_ref == device_ref) {
                device = &attached.device;
            }
        }
    }
    if (!device) {
        show_toast(state, state.text.format("The device's own serial could not be read: {}",
                              state.text("the device is no longer attached: " + device_ref)));
        return;
    }
    read_own_identity(state, *device, [](GuiState& state, OwnIdentityRead& result) {
        if (result.failure) {
            show_message(state, result.failure);
            return;
        }
        if (result.read) {
            show_toast(state, state.text.format("The device's own serial is {}", read_serial(result.read->device)));
            return;
        }
        if (const auto problem = read_problem(result.read.error())) {
            show_toast(state, read_problem_words(state.text, *problem, ReadShownIn::Devices));
        }
    });
}

} // namespace va::gui
