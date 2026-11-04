// Documentation screenshots (tools/screenshots/README.md). With
// VERIARCHIVE_GUI_SCREENSHOTS=<folder> set, the GUI tours its views once the
// window is shown, as a reader of the user guide would: it fills in forms,
// starts a verification and a comparison through the buttons' own handlers,
// saves each view as a PNG in the folder, and quits.
//
// The archives and the folder come from the environment:
//   VERIARCHIVE_SCREENSHOT_REFERENCE  uuid of archive A: selected in Archives, verified by its
//                                     folder, compared, its runs in History, the result page of
//                                     its FULL verification shown
//   VERIARCHIVE_SCREENSHOT_COPY       uuid of archive B: compared with A
//   VERIARCHIVE_SCREENSHOT_EARLIER    uuid of archive C, registered without chunk hashes as an
//                                     earlier version registered archives: its details shown in
//                                     Archives with the menu of FULL verify open, which offers
//                                     "FULL verify, and add chunk hashes"
//   VERIARCHIVE_SCREENSHOT_FOLDER     archive A's folder: typed into Register, verified
//
// GTK renders the pictures itself from what the window drew, so they show only
// the window, never other windows on the desktop, and need no screen access.
//
// The pictures are published, and the sample archives are folders on a real
// volume, whose label, device model and capacity Archives would show. So
// while this mode runs, the views show a fixed sample identity instead
// (displayed_identity; main turns it on together with the mode).
#include "gui/gui_app.h"

#include "common/utf8.h"

#include <algorithm>
#include <chrono>
#include <exception>
#include <fstream>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace va::gui {
namespace {

using Clock = std::chrono::steady_clock;

// The window's size in every picture, as the user guide shows it.
constexpr int picture_width = 1100;
constexpr int picture_height = 720;

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

std::string text_of(GtkWidget* text_view)
{
    GtkTextBuffer* buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(text_view));
    GtkTextIter start;
    GtkTextIter end;
    gtk_text_buffer_get_bounds(buffer, &start, &end);
    gchar* text = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
    std::string result = text;
    g_free(text);
    return result;
}

// Clicks a button as the user would: its "clicked" handler runs now.
void click(GtkWidget* button)
{
    g_signal_emit_by_name(button, "clicked");
}

// Chooses `archive` in a drop-down whose item `offset + i` is uuids[i].
void choose(GtkWidget* drop_down, const std::vector<Uuid>& uuids, guint offset, const Uuid& archive)
{
    choose_archive(drop_down, uuids, offset, archive);
    if (chosen_archive(drop_down, uuids, offset) != archive) {
        throw std::runtime_error("archive " + archive + " is not in the repository's archive list");
    }
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
        std::error_code error;
        std::filesystem::create_directories(*state_.screenshot_folder, error);
        if (error) {
            throw std::runtime_error("cannot create " + path_to_utf8(*state_.screenshot_folder) + ": " +
                                     error.message());
        }

        stops_ = {
            {"01-settings.png", [this] { show("settings"); }, {}},
            // Archives with archive A selected, its details beside the list.
            {"02-archive-list.png",
                [this] {
                    show("archives");
                    select(reference_);
                },
                {}},
            {"03-register.png",
                [this] {
                    // How archive A was registered: its folder and label, not started.
                    gtk_editable_set_text(GTK_EDITABLE(state_.register_view.folder), folder_.c_str());
                    gtk_editable_set_text(GTK_EDITABLE(state_.register_view.label), label_of(reference_).c_str());
                    show("register");
                },
                {}},
            {"04-verify.png",
                [this] {
                    // A FULL verification of archive A by its folder, as
                    // "Verify a folder or volume..." in its details starts
                    // one, rather than by its uuid: by uuid the verification
                    // looks through every attached volume, the folder needs
                    // only its own.
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
            {"06-compare.png",
                [this] {
                    show("compare");
                    choose(state_.compare_view.reference, state_.compare_view.archive_uuids, 0, reference_);
                    choose(state_.compare_view.copy, state_.compare_view.archive_uuids, 0, copy_);
                    start(state_.compare_view.start, state_.compare_view.report, "comparison");
                },
                [this] {
                    expect(state_.compare_view.shown_report, "ARCHIVES ", "comparison");
                    // Each archive's line says when its compared revision was
                    // recorded (decision 0041).
                    const auto english = state_.compare_view.shown_report(app::Text());
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
            // The jobs the tour started, the verification and the comparison,
            // ended (decision 0045); taken before the language list opens.
            {"08-jobs.png", [this] { show("jobs"); }, {}},
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
            g_printerr("VeriArchive screenshots: %s: %s\n", stops_[next_].file.c_str(), error.what());
            state_.exit_status = 1;
            g_application_quit(G_APPLICATION(state_.application));
            return false;
        }
    }

private:
    enum class Phase { Act, Job, Settle };

    // One picture: `act` brings the view into shape, then the picture is
    // taken. When `act` starts an operation, `after_job` is set: the tour
    // waits for the operation, and after_job checks that it ended as the
    // picture needs.
    struct Stop {
        std::string file;
        std::function<void()> act;
        std::function<void()> after_job;
    };

    bool advance()
    {
        const auto now = Clock::now();
        auto& stop = stops_[next_];
        switch (phase_) {
        case Phase::Act:
            stop.act();
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
            take(stop.file);
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

    // Starts an operation with its button; it must then be running.
    void start(GtkWidget* button, GtkWidget* report, const char* what)
    {
        click(button);
        if (!state_.jobs.active()) {
            throw std::runtime_error(std::string("the ") + what + " did not start: " + text_of(report));
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

    std::string label_of(const Uuid& archive) const
    {
        auto store = ArchiveStore::open(state_.session->context.repo);
        if (!store) {
            throw std::runtime_error("cannot open the repository: " + store.error().message);
        }
        const auto record = store->find_archive(archive);
        if (!record || !*record) {
            throw std::runtime_error("archive " + archive + " is not in the repository");
        }
        return (*record)->label;
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
    std::vector<Stop> stops_;
    std::size_t next_ = 0;
    Phase phase_ = Phase::Act;
    Clock::time_point since_{};
    gint64 frame_before_ = 0;
    std::uint64_t job_ = 0;          // the verification of 04-verify
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
