// Documentation screenshots (tools/screenshots/README.md). With
// VERIARCHIVE_GUI_SCREENSHOTS=<folder> set, the GUI tours its views once the
// window is shown, as a reader of the user guide would: it fills in forms,
// starts a verification and a comparison through the buttons' own handlers,
// saves each view as a PNG in the folder, and quits.
//
// The archives and the folder come from the environment:
//   VERIARCHIVE_SCREENSHOT_REFERENCE  uuid of archive A: verified, its history shown, compared
//   VERIARCHIVE_SCREENSHOT_COPY       uuid of archive B: compared with A
//   VERIARCHIVE_SCREENSHOT_EARLIER    uuid of archive C, registered without chunk hashes as an
//                                     earlier version registered archives: chosen in Verify
//                                     Archive with "Full verify, and add chunk hashes" ticked
//   VERIARCHIVE_SCREENSHOT_FOLDER     archive A's folder: typed into Register and Verify
//
// GTK renders the pictures itself from what the window drew, so they show only
// the window, never other windows on the desktop, and need no screen access.
//
// The pictures are published, and the sample archives are folders on a real
// volume, whose label, device model and capacity the Archive List would show.
// So while this mode runs, the views show a fixed sample identity instead
// (displayed_identity; main turns it on together with the mode).
#include "gui/gui_app.h"

#include "common/utf8.h"

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

// Opens a drop-down's list, as a click on it does, and returns the popover
// that shows the list.
GtkWidget* open_list(GtkWidget* drop_down)
{
    if (!gtk_widget_activate(drop_down)) {
        throw std::runtime_error("the drop-down did not open");
    }
    for (GtkWidget* child = gtk_widget_get_first_child(drop_down); child; child = gtk_widget_get_next_sibling(child)) {
        if (GTK_IS_POPOVER(child)) {
            return child;
        }
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
            {"02-archive-list.png", [this] { show("archives"); }, {}},
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
                    // The folder rather than the archive from the list: by uuid
                    // the verification looks through every attached volume,
                    // the folder needs only its own.
                    show("verify");
                    gtk_editable_set_text(GTK_EDITABLE(state_.verify_view.folder), folder_.c_str());
                    gtk_drop_down_set_selected(GTK_DROP_DOWN(state_.verify_view.archive), 0);
                    start(state_.verify_view.start, state_.verify_view.report, "verification");
                },
                [this] {
                    expect(state_.verify_view.shown_report, "FULL VERIFY ", "verification");
                    // Then archive C chosen in the list, which an earlier
                    // version registered without chunk hashes, and "Full
                    // verify, and add chunk hashes" ticked, as the operator's
                    // own archives registered by V1 show it (not started).
                    choose(state_.verify_view.archive, state_.verify_view.archive_uuids, 1, earlier_);
                    if (!gtk_widget_get_sensitive(state_.verify_view.add_chunk_hashes)) {
                        throw std::runtime_error("\"Full verify, and add chunk hashes\" is not offered for archive " +
                                                 earlier_);
                    }
                    gtk_check_button_set_active(GTK_CHECK_BUTTON(state_.verify_view.add_chunk_hashes), TRUE);
                }},
            {"05-history.png",
                [this] {
                    show("history");
                    choose(state_.history_view.archive, state_.history_view.archive_uuids, 0, reference_);
                    click(state_.history_view.show);
                    click(state_.history_view.check);
                },
                {}},
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
            {"07-language.png",
                [this] {
                    show("settings");
                    popover_ = open_list(state_.settings_view.language);
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
            if (state_.job.running()) {
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
            phase_ = Phase::Act;
            if (++next_ < stops_.size()) {
                return true;
            }
            if (popover_) {
                gtk_popover_popdown(GTK_POPOVER(popover_));
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
        gtk_stack_set_visible_child_name(GTK_STACK(state_.stack), view);
    }

    // Starts an operation with its button; it must then be running.
    void start(GtkWidget* button, GtkWidget* report, const char* what)
    {
        click(button);
        if (!state_.job.running()) {
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
    GtkWidget* popover_ = nullptr;  // the open language list, for the last picture
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
