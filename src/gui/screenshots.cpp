// Documentation screenshots (tools/screenshots/README.md). With
// VERIARCHIVE_GUI_SCREENSHOTS=<folder> set, the GUI tours its views once the
// window is shown, as a reader of the user guide would: it fills in forms,
// opens Register Archive, the comparison dialog and the relocation dialog,
// starts a registration, a verification and a comparison through the
// buttons' own handlers, saves each view as a PNG in the folder, and quits.
//
// The archives and the folders come from the environment:
//   VERIARCHIVE_SCREENSHOT_REFERENCE  uuid of archive A: selected in Archives, verified by its
//                                     folder, compared, its runs in History, the result page of
//                                     its FULL verification shown
//   VERIARCHIVE_SCREENSHOT_COPY       uuid of archive B: compared with A; its relocation dialog
//                                     shown, a sample folder of a sample volume chosen
//   VERIARCHIVE_SCREENSHOT_EARLIER    uuid of archive C, registered without chunk hashes as an
//                                     earlier version registered archives: its details shown in
//                                     Archives with the menu of FULL verify open, which offers
//                                     "FULL verify, and add chunk hashes"
//   VERIARCHIVE_SCREENSHOT_FOLDER     archive A's folder: verified by its folder
//   VERIARCHIVE_SCREENSHOT_NEW_FOLDER a sample folder that no archive holds: typed into Register
//                                     Archive with a sample label, then registered there
//
// GTK renders the pictures itself from what the window drew, so they show only
// the window, never other windows on the desktop, and need no screen access.
//
// The pictures are published, and the sample archives are folders on a real
// volume, whose label, device model and capacity Archives would show. So
// while this mode runs, the views show a fixed sample identity instead
// (displayed_identity; main turns it on together with the mode), and Register
// Archive and Devices list a fixed list of sample volumes and devices
// (sample_volumes), never this computer's. Where each sample archive is
// attached is told on the volumes of the sample archives' folders, as normal
// use tells it, and no other volume of this computer is read; Archives and
// Devices show of that only the archives' roots, their sample folders, and
// the sample identity (Devices lists such a volume as SAMPLE-VOLUME on
// SAMPLE-DEVICE, with a sample health, attached_read). A read of a device's
// own serial behind its enclosure (decision 0047) goes to a sample device
// identity reader (sample_device_identity_helper), which answers with a fixed
// sample identity and never asks Windows for administrator permission.
//
// No picture shows the enclosure question of a FULL verify: the sample
// archives are folders on this computer's own volume, which identity matching
// finds as recorded (MATCH). The question needs an archive's carrier seen
// behind another enclosure, which only a made-up identity could give, and the
// window behind the dialog would then show the archive attached as recorded.
#include "gui/gui_app.h"

#include "common/utf8.h"

#include <algorithm>
#include <chrono>
#include <exception>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace va::gui {
namespace {

using Clock = std::chrono::steady_clock;

// The window's size in every picture, as the user guide shows it.
constexpr int picture_width = 1100;
constexpr int picture_height = 720;

// The new root that the relocation dialog's picture shows chosen: a folder on
// the first of the sample volumes (sample_volumes), SAMPLE DATA, never a
// folder of this computer.
constexpr const char* sample_new_root = "/media/SAMPLE-VOLUME-1/備份資料";

// The label typed into Register Archive for the sample folder that no archive
// holds (VERIARCHIVE_SCREENSHOT_NEW_FOLDER): a sample label (SAMPLE DATA),
// not the repository's next one, ARCH-0004 after the sample archives'
// ARCH-0001 to ARCH-0003, which the page proposes for an empty field: the
// picture then shows a label typed in, not the one proposed.
constexpr const char* sample_label = "ARCH-0009";

// After a view changes, GTK lays it out and draws it over the next frames; the
// picture is taken once a frame was drawn and this much time has passed, so
// text views and lists have filled in too.
constexpr auto settle_time = std::chrono::milliseconds(700);
constexpr auto draw_timeout = std::chrono::seconds(20);
constexpr auto job_timeout = std::chrono::minutes(10);

// Owners for GLib objects, so a failure halfway leaks nothing.
struct ObjectUnref {
    void operator()(gpointer object) const { g_object_unref(object); }
};
struct NodeUnref {
    void operator()(GskRenderNode* node) const { gsk_render_node_unref(node); }
};
struct BytesUnref {
    void operator()(GBytes* bytes) const { g_bytes_unref(bytes); }
};

std::string environment(const char* name)
{
    const char* value = g_getenv(name);
    if (!value || !*value) {
        throw std::runtime_error(std::string(name) + " is not set");
    }
    return value;
}

// Clicks a button as the user would: its "clicked" handler runs now.
void click(GtkWidget* button)
{
    g_signal_emit_by_name(button, "clicked");
}

// The archive chosen in a combo row (AdwComboRow) whose item i is uuids[i].
std::optional<Uuid> chosen(GtkWidget* row, const std::vector<Uuid>& uuids)
{
    const auto selected = adw_combo_row_get_selected(ADW_COMBO_ROW(row));
    return selected < uuids.size() ? std::optional(uuids[selected]) : std::nullopt;
}

// The first popover inside `widget`, at any depth.
GtkWidget* popover_in(GtkWidget* widget)
{
    for (GtkWidget* child = gtk_widget_get_first_child(widget); child; child = gtk_widget_get_next_sibling(child)) {
        if (GTK_IS_POPOVER(child)) {
            return child;
        }
        if (GtkWidget* inner = popover_in(child)) {
            return inner;
        }
    }
    return nullptr;
}

// Opens the list of a drop-down or a combo row (AdwComboRow), as a click on
// it does, and returns the popover that shows the list.
GtkWidget* open_list(GtkWidget* drop_down)
{
    if (!gtk_widget_activate(drop_down)) {
        throw std::runtime_error("the drop-down did not open");
    }
    if (GtkWidget* popover = popover_in(drop_down)) {
        return popover;
    }
    throw std::runtime_error("the drop-down has no list to show");
}

// Draws what `widget` drew in its last frame into `snapshot`, with its top
// left corner at `offset`.
void append_widget(GtkSnapshot* snapshot, GtkWidget* widget, graphene_point_t offset)
{
    std::unique_ptr<GdkPaintable, ObjectUnref> paintable(gtk_widget_paintable_new(widget));
    gtk_snapshot_save(snapshot);
    gtk_snapshot_translate(snapshot, &offset);
    gdk_paintable_snapshot(paintable.get(), GDK_SNAPSHOT(snapshot), gtk_widget_get_width(widget),
        gtk_widget_get_height(widget));
    gtk_snapshot_restore(snapshot);
}

// Where the popover lies in the window's coordinates. A popover has a surface
// of its own, placed relative to the window's surface; each widget sits inside
// its surface at the offset its "surface transform" gives (shadows, borders).
graphene_point_t popover_offset(GtkWindow* window, GtkWidget* popover)
{
    double window_x = 0.0;
    double window_y = 0.0;
    double popover_x = 0.0;
    double popover_y = 0.0;
    gtk_native_get_surface_transform(GTK_NATIVE(window), &window_x, &window_y);
    gtk_native_get_surface_transform(GTK_NATIVE(popover), &popover_x, &popover_y);
    GdkSurface* surface = gtk_native_get_surface(GTK_NATIVE(popover));
    return {static_cast<float>(gdk_popup_get_position_x(GDK_POPUP(surface)) + popover_x - window_x),
        static_cast<float>(gdk_popup_get_position_y(GDK_POPUP(surface)) + popover_y - window_y)};
}

// Saves the window, as it was last drawn, as a PNG; with `popover`, the open
// drop-down list is drawn over it where it appears on the screen.
void save_picture(GtkWindow* window, GtkWidget* popover, const std::filesystem::path& file)
{
    GtkWidget* widget = GTK_WIDGET(window);
    GtkSnapshot* snapshot = gtk_snapshot_new();
    append_widget(snapshot, widget, {0.0f, 0.0f});
    if (popover) {
        append_widget(snapshot, popover, popover_offset(window, popover));
    }
    std::unique_ptr<GskRenderNode, NodeUnref> node(gtk_snapshot_free_to_node(snapshot));
    if (!node) {
        throw std::runtime_error("the window has drawn nothing yet");
    }
    graphene_rect_t viewport;
    graphene_rect_init(&viewport, 0.0f, 0.0f, static_cast<float>(gtk_widget_get_width(widget)),
        static_cast<float>(gtk_widget_get_height(widget)));
    std::unique_ptr<GdkTexture, ObjectUnref> texture(
        gsk_renderer_render_texture(gtk_native_get_renderer(GTK_NATIVE(window)), node.get(), &viewport));
    std::unique_ptr<GBytes, BytesUnref> png(gdk_texture_save_to_png_bytes(texture.get()));
    gsize size = 0;
    const auto* bytes = static_cast<const char*>(g_bytes_get_data(png.get(), &size));
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out.write(bytes, static_cast<std::streamsize>(size));
    out.close();
    if (!out) {
        throw std::runtime_error("cannot write " + path_to_utf8(file));
    }
}

class Tour {
public:
    Tour(const Tour&) = delete;
    Tour& operator=(const Tour&) = delete;
    ~Tour() { close_popover(); }

    explicit Tour(GuiState& state) : state_(state)
    {
        if (!state_.session) {
            const auto why = state_.session_error ? state_.session_error(app::Text()) : std::string();
            throw std::runtime_error("no repository is open: " + why);
        }
        // No picture may show the identity of the volumes the archives lie on.
        if (state_.identity_shown != IdentityShown::Sample) {
            throw std::runtime_error("the views show the recorded media identity instead of the sample one");
        }
        reference_ = environment("VERIARCHIVE_SCREENSHOT_REFERENCE");
        copy_ = environment("VERIARCHIVE_SCREENSHOT_COPY");
        earlier_ = environment("VERIARCHIVE_SCREENSHOT_EARLIER");
        folder_ = environment("VERIARCHIVE_SCREENSHOT_FOLDER");
        new_folder_ = environment("VERIARCHIVE_SCREENSHOT_NEW_FOLDER");
        std::error_code error;
        std::filesystem::create_directories(*state_.screenshot_folder, error);
        if (error) {
            throw std::runtime_error("cannot create " + path_to_utf8(*state_.screenshot_folder) + ": " +
                                     error.message());
        }

        stops_ = {
            {"01-settings.png", [this] { show("settings"); }, {}},
            // Archives with archive A selected, its details beside the list,
            // once the attached devices were read: each row starts with where
            // its archive is attached, the sample archives' folders.
            {"02-archive-list.png",
                [this] {
                    show("archives");
                    select(reference_);
                },
                {}, [this] { return devices_read(); },
                [this] {
                    const auto* read = attached_now(state_);
                    const auto* attachment = read ? attachment_of(*read, reference_) : nullptr;
                    if (!attachment ||
                        (attachment->result != MatchResult::Match && attachment->result != MatchResult::MatchWeak)) {
                        throw std::runtime_error("archive " + reference_ + " is not attached at its folder");
                    }
                }},
            // Devices with the screenshot mode's fixed sample list, never
            // this computer's devices, and the sample archives on the volume
            // of their folders, shown as the sample identity.
            {"10-devices.png", [this] { show("devices"); }, {}, [this] { return devices_read(); },
                [this] {
                    const auto* read = attached_now(state_);
                    if (!read || !read->sample) {
                        throw std::runtime_error("Devices does not show the sample devices");
                    }
                    const auto& shown = state_.devices_page.shown;
                    if (!shown || shown->empty()) {
                        throw std::runtime_error("Devices shows no device");
                    }
                    const auto holding = std::ranges::find_if(*shown, [](const DeviceCard& card) {
                        return std::ranges::any_of(
                            card.volumes, [](const VolumeLine& volume) { return !volume.archives.empty(); });
                    });
                    if (holding == shown->end()) {
                        throw std::runtime_error("Devices shows no archive on a volume");
                    }
                    // Its card scrolled into view, where its mount point sorts
                    // after those of the sample list (a drive letter after
                    // /media/SAMPLE-*), as little as shows it whole.
                    GtkWidget* card = gtk_widget_get_first_child(state_.devices_page.cards);
                    for (auto index = holding - shown->begin(); card && index > 0; --index) {
                        card = gtk_widget_get_next_sibling(card);
                    }
                    if (GtkWidget* viewport = card ? gtk_widget_get_ancestor(card, GTK_TYPE_VIEWPORT) : nullptr) {
                        gtk_viewport_scroll_to(GTK_VIEWPORT(viewport), card, nullptr);
                    }
                    // Decision 0047: the sample device behind its sample
                    // enclosure offers Read the device's own serial...
                    if (std::ranges::none_of(*shown, [](const DeviceCard& listed) {
                            return listed.own && listed.own->offered;
                        })) {
                        throw std::runtime_error("Devices offers no read of a device's own serial");
                    }
                }},
            // Then Read the device's own serial... of that sample device (no
            // picture), as the operator clicks it: the screenshot mode's
            // sample device identity reader answers at once with its sample
            // identity, asking Windows nothing (SAMPLE DATA), and Devices
            // shows the serial read.
            {"",
                [this] {
                    show("devices");
                    const auto& shown = *state_.devices_page.shown;
                    const auto card = std::ranges::find_if(
                        shown, [](const DeviceCard& listed) { return listed.own && listed.own->offered; });
                    read_device_serial(state_, card->own->device_ref);
                    if (!state_.device_reading) {
                        throw std::runtime_error("the device's own serial is not being read");
                    }
                },
                {},
                [this] {
                    if (state_.device_reading) {
                        return false;
                    }
                    const auto& shown = state_.devices_page.shown;
                    if (!shown || std::ranges::none_of(*shown, [](const DeviceCard& listed) {
                            return listed.own && listed.own->value.contains(sample_own_serial);
                        })) {
                        throw std::runtime_error("Devices does not show the device's own serial read");
                    }
                    return true;
                }},
            {"03-register.png",
                [this] {
                    // Register Archive as the sidebar's + opens it, the
                    // sample folder that no archive holds typed in on its
                    // first page, then Next, which reads it off the window's
                    // thread.
                    show("archives");
                    select(reference_);
                    click(state_.shell.add);
                    auto& wizard = state_.register_wizard;
                    if (!wizard.dialog || !wizard.folder || !wizard.source_next) {
                        throw std::runtime_error("Register Archive did not open");
                    }
                    gtk_editable_set_text(GTK_EDITABLE(wizard.folder), new_folder_.c_str());
                    click(wizard.source_next);
                },
                {},
                // Its Options page for that folder, the sample label typed
                // in: ready for Next.
                [this] {
                    const auto& wizard = state_.register_wizard;
                    if (!wizard.dialog) {
                        throw std::runtime_error("Register Archive closed");
                    }
                    if (wizard.reading_source) {
                        return false;
                    }
                    if (!wizard.options_page) {
                        const char* problem =
                            wizard.problem_words ? gtk_label_get_text(GTK_LABEL(wizard.problem_words)) : "";
                        throw std::runtime_error(std::string("Register Archive did not offer its options: ") + problem);
                    }
                    return true;
                },
                [this] {
                    auto& wizard = state_.register_wizard;
                    // No archive holds the folder: a new archive, its label
                    // to be given, and neither a new revision nor a resume.
                    if (!gtk_widget_get_visible(wizard.label_group) || gtk_widget_get_visible(wizard.new_revision) ||
                        gtk_widget_get_visible(wizard.another_archive) || gtk_widget_get_visible(wizard.resume)) {
                        throw std::runtime_error("Register Archive does not take " + new_folder_ +
                                                 " for a folder that no archive holds");
                    }
                    gtk_editable_set_text(GTK_EDITABLE(wizard.label), sample_label);
                    if (!wizard.options_next || !gtk_widget_get_sensitive(wizard.options_next)) {
                        throw std::runtime_error(std::string("Register Archive does not offer Next with the label ") +
                                                 sample_label);
                    }
                    // As the reader sees it once the label is typed: the
                    // focus on Next, the label not shown selected.
                    gtk_widget_grab_focus(wizard.options_next);
                }},
            // Then Next and Start registration (no picture): the folder
            // registered as a job, and listed in Archives once it ended.
            {"",
                [this] {
                    auto& wizard = state_.register_wizard;
                    click(wizard.options_next);
                    if (!wizard.confirm_page || !wizard.start) {
                        throw std::runtime_error("Register Archive did not show its Confirm page");
                    }
                    click(wizard.start);
                    if (!state_.jobs.active() || state_.register_wizard.dialog) {
                        throw std::runtime_error("the registration did not start");
                    }
                    registration_ = state_.jobs.shown().back().id;
                },
                [this] {
                    const auto* job = state_.jobs.find(registration_);
                    expect(job ? job->report : Report{}, std::string("Registered ") + sample_label + " (",
                        "registration");
                    const auto& statuses = state_.archives_page.statuses;
                    if (std::ranges::none_of(statuses, [&](const ArchiveStatus& status) {
                            return status.archive.label == sample_label && status.archive.archive_uuid != reference_ &&
                                   status.archive.archive_uuid != copy_ && status.archive.archive_uuid != earlier_;
                        })) {
                        throw std::runtime_error(std::string("Archives does not list the archive registered as ") +
                                                 sample_label);
                    }
                }},
            {"04-verify.png",
                [this] {
                    // A FULL verification of archive A by its folder, as
                    // "Verify a folder or volume..." in its details starts
                    // one, rather than by its uuid: by uuid the verification
                    // looks through every attached volume, the folder needs
                    // only its own.
                    close_dialogs(state_);
                    show("archives");
                    select(reference_);
                    verify_root(state_, reference_, path_from_utf8(folder_));
                    const auto job = state_.verification_jobs.find(reference_);
                    if (!state_.jobs.active() || job == state_.verification_jobs.end()) {
                        throw std::runtime_error("the verification did not start");
                    }
                    job_ = job->second;
                },
                [this] {
                    const auto* job = state_.jobs.find(job_);
                    expect(job ? job->report : Report{}, "FULL VERIFY ", "verification");
                    // Then the details of archive C, which an earlier version
                    // registered without chunk hashes, with the menu of FULL
                    // verify open, as the operator's own archives registered by
                    // V1 show it: "FULL verify, and add chunk hashes" is offered
                    // (not started). The toast says the result of A's.
                    select(earlier_);
                    auto& page = state_.archives_page;
                    if (!page.adding || !gtk_widget_get_sensitive(page.adding) || !page.verify_menu) {
                        throw std::runtime_error("\"FULL verify, and add chunk hashes\" is not offered for archive " +
                                                 earlier_);
                    }
                    open_popover(page.verify_menu);
                    gtk_popover_popup(GTK_POPOVER(popover_));
                }},
            // The comparison dialog with A and its copy B chosen, as Compare...
            // on B's row under Copies in A's details opens it; not started.
            {"06-compare.png",
                [this] {
                    show("archives");
                    select(reference_);
                    compare_with(state_, reference_, copy_);
                    const auto& dialog = state_.compare_dialog;
                    if (!dialog.dialog) {
                        throw std::runtime_error("the comparison dialog did not open");
                    }
                    if (chosen(dialog.reference, dialog.references) != reference_ ||
                        chosen(dialog.copy, dialog.copies) != copy_) {
                        throw std::runtime_error("the comparison dialog does not have A and B chosen");
                    }
                },
                {}},
            // Then Compare (no picture): the report in the job's details.
            {"",
                [this] {
                    click(state_.compare_dialog.start);
                    if (!state_.jobs.active() || state_.compare_dialog.dialog) {
                        throw std::runtime_error("the comparison did not start");
                    }
                    comparison_ = state_.jobs.shown().back().id;
                },
                [this] {
                    const auto* job = state_.jobs.find(comparison_);
                    const auto report = job ? job->report : Report{};
                    expect(report, "ARCHIVES ", "comparison");
                    // Each archive's line says when its compared revision was
                    // recorded (decision 0041).
                    const auto english = report(app::Text());
                    for (const std::string archive : {"\nA (reference): ", "\nB (copy): "}) {
                        const auto start = english.find(archive);
                        const auto line = start == std::string::npos
                                              ? std::string()
                                              : english.substr(start + 1, english.find('\n', start + 1) - start - 1);
                        if (line.find(" revision 1, recorded ") == std::string::npos) {
                            throw std::runtime_error(
                                "the comparison does not say when each compared revision was recorded: " + line);
                        }
                    }
                }},
            // History of every archive after the verification of A and the
            // comparison, with the chain checked.
            {"05-history.png",
                [this] {
                    state_.history_filter.reset();
                    show("history");
                    refresh_history(state_);
                    click(state_.history_page.check);
                    const auto& check = state_.history_check;
                    if (!check || !*check || (*check)->first_broken_run) {
                        throw std::runtime_error("the check of the history chain did not find it intact");
                    }
                    // The run that the tour's verification of A recorded: a
                    // new sample repository holds no other.
                    const auto recorded = state_.job_runs.find(job_);
                    if (recorded == state_.job_runs.end() ||
                        std::ranges::none_of(state_.history_page.runs, [&](const VerifyRunRecord& run) {
                            return run.run_id == recorded->second.run_id && run.archive_uuid == reference_;
                        })) {
                        throw std::runtime_error("History does not list the verification of archive " + reference_);
                    }
                },
                {}},
            // The result page of A's last FULL verification, the tour's,
            // opened with View result in its details.
            {"09-result.png",
                [this] {
                    show("archives");
                    select(reference_);
                    if (!state_.archives_page.view_result) {
                        throw std::runtime_error("the details of archive " + reference_ + " offer no View result");
                    }
                    click(state_.archives_page.view_result);
                    auto* navigation = ADW_NAVIGATION_VIEW(state_.archives_page.navigation);
                    if (adw_navigation_view_get_visible_page(navigation) ==
                        ADW_NAVIGATION_PAGE(state_.archives_page.root)) {
                        throw std::runtime_error("View result opened no result page");
                    }
                },
                {}},
            // The relocation dialog of archive B, as Relocate to a new
            // carrier... in its details opens it, once the attached volumes
            // were read (the fixed sample list), a sample folder on a sample
            // volume typed in as the new root; not started.
            {"11-relocate.png",
                [this] {
                    show("archives");
                    select(copy_);
                    open_relocation(state_, copy_);
                    const auto& dialog = state_.relocate_dialog;
                    if (!dialog.dialog || !dialog.folder) {
                        throw std::runtime_error("the relocation dialog did not open for archive " + copy_);
                    }
                    gtk_editable_set_text(GTK_EDITABLE(dialog.folder), sample_new_root);
                },
                {},
                [this] {
                    const auto& dialog = state_.relocate_dialog;
                    if (!dialog.dialog) {
                        throw std::runtime_error("the relocation dialog closed");
                    }
                    // Its volumes read: the radios of the sample volumes.
                    return !dialog.radios.empty();
                },
                [this] {
                    const auto& dialog = state_.relocate_dialog;
                    if (!dialog.start || !gtk_widget_get_sensitive(dialog.start)) {
                        throw std::runtime_error("the relocation dialog does not offer Start relocation");
                    }
                    // As the reader sees it once the root is chosen: the
                    // focus on Start relocation (the root typed is not shown
                    // selected), the dialog from its top, which says first
                    // what the operator does before a relocation.
                    gtk_widget_grab_focus(dialog.start);
                    gtk_editable_select_region(GTK_EDITABLE(dialog.folder), -1, -1);
                }},
            // The jobs the tour started, the registration, the verification
            // and the comparison, ended (decision 0045); taken before the
            // language list opens.
            {"08-jobs.png",
                [this] {
                    close_dialogs(state_);
                    show("jobs");
                },
                {}},
            {"07-language.png",
                [this] {
                    show("settings");
                    open_popover(open_list(state_.settings_page.language));
                },
                {}},
        };
        // The pictures show the window at the size the user guide expects.
        gtk_window_unmaximize(state_.window);
        gtk_window_set_default_size(state_.window, picture_width, picture_height);
    }

    // Called every 100 ms by the main loop; false once the tour has ended.
    bool tick()
    {
        if (!state_.window) {
            // Closed by hand: the application is shutting down.
            g_printerr("VeriArchive screenshots: the window was closed before the tour ended\n");
            state_.exit_status = 1;
            return false;
        }
        try {
            return advance();
        } catch (const std::exception& error) {
            const auto& file = stops_[next_].file;
            g_printerr("VeriArchive screenshots: %s: %s\n", file.empty() ? "(between pictures)" : file.c_str(),
                error.what());
            state_.exit_status = 1;
            g_application_quit(G_APPLICATION(state_.application));
            return false;
        }
    }

private:
    enum class Phase { Act, Ready, Job, Settle };

    // One picture: `act` brings the view into shape, then the picture is
    // taken. When `act` starts an operation, `after_job` is set: the tour
    // waits for the operation, and after_job checks that it ended as the
    // picture needs. When `act` starts a read of a dialog, `ready` is set:
    // the tour waits until it says the dialog shows what the picture needs,
    // then `after_ready` finishes the view. A stop without a file takes no
    // picture.
    struct Stop {
        std::string file;
        std::function<void()> act;
        std::function<void()> after_job;
        std::function<bool()> ready = {};
        std::function<void()> after_ready = {};
    };

    bool advance()
    {
        const auto now = Clock::now();
        auto& stop = stops_[next_];
        switch (phase_) {
        case Phase::Act:
            stop.act();
            since_ = now;
            if (stop.ready) {
                phase_ = Phase::Ready;
            } else if (stop.after_job) {
                phase_ = Phase::Job;
            } else {
                begin_settling(now);
            }
            return true;
        case Phase::Ready:
            if (!stop.ready()) {
                if (now - since_ > draw_timeout) {
                    throw std::runtime_error("the dialog did not read what it shows in time");
                }
                return true;
            }
            if (stop.after_ready) {
                stop.after_ready();
            }
            since_ = now;
            if (stop.after_job) {
                phase_ = Phase::Job;
            } else {
                begin_settling(now);
            }
            return true;
        case Phase::Job:
            if (state_.jobs.active()) {
                if (now - since_ > job_timeout) {
                    throw std::runtime_error("the operation did not finish in time");
                }
                return true;
            }
            stop.after_job();
            begin_settling(now);
            return true;
        case Phase::Settle:
            if (now - since_ < settle_time || frame() == frame_before_ ||
                (popover_ && !gtk_widget_get_mapped(popover_))) {
                if (now - since_ > draw_timeout) {
                    throw std::runtime_error("the window did not draw the view");
                }
                return true;
            }
            if (!stop.file.empty()) {
                take(stop.file);
            }
            // The next picture shows neither the popover of this one nor
            // its toast.
            close_popover();
            dismiss_toast(state_);
            phase_ = Phase::Act;
            if (++next_ < stops_.size()) {
                return true;
            }
            g_application_quit(G_APPLICATION(state_.application));
            return false;
        }
        return false;
    }

    void begin_settling(Clock::time_point now)
    {
        since_ = now;
        frame_before_ = frame();
        gtk_widget_queue_draw(GTK_WIDGET(state_.window));  // so a frame is drawn even if nothing changed
        phase_ = Phase::Settle;
    }

    // The number of the window's latest frame (0 before its first).
    gint64 frame() const
    {
        GdkFrameClock* clock = gtk_widget_get_frame_clock(GTK_WIDGET(state_.window));
        return clock ? gdk_frame_clock_get_frame_counter(clock) : 0;
    }

    void show(const char* view)
    {
        show_page(state_, view);
    }

    // Whether a read of the attached devices has come back, none being in
    // flight: what Archives and Devices show of them is drawn.
    bool devices_read() const
    {
        if (state_.devices.busy) {
            return false;
        }
        if (!attached_now(state_)) {
            if (state_.devices.error) {
                throw std::runtime_error("the attached devices could not be read: " +
                                         state_.devices.error(app::Text()));
            }
            return false;
        }
        return true;
    }

    // Selects `archive` in Archives; it must be listed.
    void select(const Uuid& archive)
    {
        select_archive(state_, archive);
        if (state_.archive_selected != archive) {
            throw std::runtime_error("archive " + archive + " is not in Archives");
        }
    }

    // The popover drawn over the next picture, held until it is closed: a
    // read of the repository may make the view it belongs to again.
    void open_popover(GtkWidget* popover)
    {
        close_popover();
        popover_ = GTK_WIDGET(g_object_ref(popover));
    }

    void close_popover()
    {
        if (popover_) {
            gtk_popover_popdown(GTK_POPOVER(popover_));
            g_object_unref(popover_);
            popover_ = nullptr;
        }
    }

    // The report shown after the operation, worded in English, must begin
    // with `beginning`; anything else (an error) would make a wrong picture.
    static void expect(const Report& report, const std::string& beginning, const char* what)
    {
        const auto english = report ? report(app::Text()) : std::string();
        if (!english.starts_with(beginning)) {
            throw std::runtime_error(std::string("the ") + what + " did not reach a result: " +
                                     english.substr(0, english.find('\n')));
        }
    }

    void take(const std::string& file)
    {
        GtkWidget* window = GTK_WIDGET(state_.window);
        const int width = gtk_widget_get_width(window);
        const int height = gtk_widget_get_height(window);
        if (width != picture_width || height != picture_height) {
            // The screen may be too small for the window; the picture is
            // still useful, only not the usual size.
            g_printerr("VeriArchive screenshots: %s is %dx%d, not %dx%d\n", file.c_str(), width, height,
                picture_width, picture_height);
        }
        save_picture(state_.window, popover_, *state_.screenshot_folder / file);
    }

    GuiState& state_;
    Uuid reference_;
    Uuid copy_;
    Uuid earlier_;
    std::string folder_;
    std::string new_folder_;
    std::vector<Stop> stops_;
    std::size_t next_ = 0;
    Phase phase_ = Phase::Act;
    Clock::time_point since_{};
    gint64 frame_before_ = 0;
    std::uint64_t registration_ = 0; // the registration after 03-register
    std::uint64_t job_ = 0;          // the verification of 04-verify
    std::uint64_t comparison_ = 0;   // the comparison after 06-compare
    GtkWidget* popover_ = nullptr;   // the popover drawn over the picture (a reference of its own)
};

} // namespace

void start_screenshot_tour(GuiState& state)
{
    Tour* tour = nullptr;
    try {
        tour = new Tour(state);
    } catch (const std::exception& error) {
        g_printerr("VeriArchive screenshots: %s\n", error.what());
        state.exit_status = 1;
        g_application_quit(G_APPLICATION(state.application));
        return;
    }
    g_timeout_add_full(G_PRIORITY_DEFAULT, 100,
        +[](gpointer data) -> gboolean { return static_cast<Tour*>(data)->tick() ? G_SOURCE_CONTINUE : G_SOURCE_REMOVE; },
        tour, +[](gpointer data) { delete static_cast<Tour*>(data); });
}

} // namespace va::gui
