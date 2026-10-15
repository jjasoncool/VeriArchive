#include "gui/gui_support.h"

#include "app/progress_estimate.h"
#include "common/utf8.h"
#include "platform/platform_services.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <format>
#include <fstream>
#include <memory>
#include <string_view>
#include <system_error>

namespace va::gui {

Report fixed_message(std::string english)
{
    return [english = std::move(english)](const app::Text& text) { return text(english); };
}

Report unexpected_error(std::string what)
{
    // The message goes through text() too: some exceptions carry a sentence the
    // catalog knows, such as "cannot initialise SHA-256".
    return [what = std::move(what)](const app::Text& text) {
        return text.format("Unexpected error: {}", text(what));
    };
}

std::string format_bytes(std::uint64_t bytes)
{
    const char* units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    double value = static_cast<double>(bytes);
    int unit = 0;
    while (value >= 1024.0 && unit < 4) {
        value /= 1024.0;
        ++unit;
    }
    return std::format("{:.1f} {}", value, units[unit]);
}

GtkWidget* make_label(const std::string& text, bool selectable)
{
    GtkWidget* label = gtk_label_new(text.c_str());
    gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
    gtk_label_set_wrap(GTK_LABEL(label), TRUE);
    gtk_label_set_selectable(GTK_LABEL(label), selectable);
    return label;
}

GtkWidget* make_row_label(const std::string& text)
{
    GtkWidget* label = gtk_label_new(text.c_str());
    gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
    return label;
}

GtkWidget* make_heading(const std::string& text)
{
    GtkWidget* label = gtk_label_new(nullptr);
    gchar* escaped = g_markup_escape_text(text.c_str(), -1);
    const std::string markup = std::string("<b><big>") + escaped + "</big></b>";
    g_free(escaped);
    gtk_label_set_markup(GTK_LABEL(label), markup.c_str());
    gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
    return label;
}

GtkWidget* make_text_view()
{
    GtkWidget* view = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(view), FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(view), TRUE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(view), GTK_WRAP_WORD_CHAR);
    return view;
}

void set_text(GtkWidget* text_view, const std::string& text)
{
    gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(text_view)), text.c_str(), -1);
}

void set_margins(GtkWidget* widget, int margin)
{
    gtk_widget_set_margin_start(widget, margin);
    gtk_widget_set_margin_end(widget, margin);
    gtk_widget_set_margin_top(widget, margin);
    gtk_widget_set_margin_bottom(widget, margin);
}

void clear_grid(GtkWidget* grid)
{
    while (GtkWidget* child = gtk_widget_get_first_child(grid)) {
        gtk_grid_remove(GTK_GRID(grid), child);
    }
}

std::filesystem::path entry_path(GtkWidget* entry)
{
    return path_from_utf8(gtk_editable_get_text(GTK_EDITABLE(entry)));
}

std::optional<Uuid> chosen_archive(GtkWidget* drop_down, const std::vector<Uuid>& uuids, guint offset)
{
    const auto selected = gtk_drop_down_get_selected(GTK_DROP_DOWN(drop_down));
    if (selected == GTK_INVALID_LIST_POSITION || selected < offset || selected - offset >= uuids.size()) {
        return std::nullopt;
    }
    return uuids[selected - offset];
}

void choose_archive(GtkWidget* drop_down, const std::vector<Uuid>& uuids, guint offset,
    const std::optional<Uuid>& archive)
{
    if (!archive) {
        return;
    }
    if (const auto found = std::ranges::find(uuids, *archive); found != uuids.end()) {
        gtk_drop_down_set_selected(GTK_DROP_DOWN(drop_down), offset + static_cast<guint>(found - uuids.begin()));
    }
}

void choose_folder(GtkWindow* parent, GtkWidget* entry, const std::string& title)
{
    GtkFileDialog* dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, title.c_str());
    // The dialog holds its own reference to the entry, so the answer can be
    // written even if the window closed meanwhile (the entry is then unused).
    gtk_file_dialog_select_folder(dialog, parent, nullptr,
        +[](GObject* source, GAsyncResult* result, gpointer data) {
            GFile* folder = gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(source), result, nullptr);
            if (folder) {
                if (gchar* path = g_file_get_path(folder)) {
                    gtk_editable_set_text(GTK_EDITABLE(data), path);
                    g_free(path);
                }
                g_object_unref(folder);
            }
            g_object_unref(data);
            g_object_unref(source);
        },
        g_object_ref(entry));
}

// ---- Failure log ----------------------------------------------------------

namespace {

// Set once by install_failure_log(), before any other thread starts; only read
// afterwards.
std::filesystem::path failure_log_file;
std::terminate_handler previous_terminate = nullptr;
GLogLevelFlags glib_always_fatal = G_LOG_FLAG_RECURSION;

// Called by the C++ runtime when an exception is not caught (or a noexcept
// function throws): the program ends. It records why, so an unexpected close
// leaves evidence, then lets the standard handler print and abort as usual.
[[noreturn]] void on_terminate() noexcept
{
    std::string reason = "terminate called without an active exception";
    if (const auto exception = std::current_exception()) {
        try {
            std::rethrow_exception(exception);
        } catch (const std::exception& error) {
            reason = std::string("uncaught exception: ") + error.what();
        } catch (...) {
            reason = "uncaught exception of an unknown type";
        }
    }
    log_failure("the program ended: " + reason);
    if (previous_terminate) {
        previous_terminate();
    }
    std::abort();
}

// GLib's structured messages (g_error, g_critical, ... in GTK) arrive here.
// A fatal one is recorded before GLib ends the program; every message is then
// written as GLib would write it.
GLogWriterOutput write_glib_log(GLogLevelFlags level, const GLogField* fields, gsize count, gpointer data)
{
    // Messages from the older g_log() calls carry this field; they were
    // recorded already by handle_old_glib_log.
    bool old_api = false;
    for (gsize index = 0; index < count; ++index) {
        old_api = old_api || std::string_view(fields[index].key) == "GLIB_OLD_LOG_API";
    }
    if (!old_api && (level & (G_LOG_FLAG_FATAL | glib_always_fatal)) != 0) {
        // "(program:pid): Domain-LEVEL **: time: message", after a line break.
        gchar* line = g_log_writer_format_fields(level, fields, count, FALSE);
        try {
            std::string_view formatted(line);
            formatted.remove_prefix(std::min(formatted.find_first_not_of('\n'), formatted.size()));
            log_failure(std::string("fatal GLib message: ") + std::string(formatted));
        } catch (const std::exception&) {
            // Nothing more can be done while GLib ends the program.
        }
        g_free(line);
    }
    return g_log_writer_default(level, fields, count, data);
}

// The older g_log() calls arrive here first (GLib's "assertion failed"
// criticals among them); G_LOG_FLAG_FATAL says GLib ends the program next.
void handle_old_glib_log(const gchar* domain, GLogLevelFlags level, const gchar* message, gpointer data)
{
    if ((level & G_LOG_FLAG_FATAL) != 0) {
        try {
            const std::string_view from = domain ? domain : "";
            log_failure(std::format("fatal GLib message: {}{}{}", from, from.empty() ? "" : ": ",
                message ? message : ""));
        } catch (const std::exception&) {
            // As above.
        }
    }
    g_log_default_handler(domain, level, message, data);
}

} // namespace

void install_failure_log()
{
    if (const auto directory = platform::user_config_dir()) {
        failure_log_file = *directory / "veriarchive-gui.log";
    }
    previous_terminate = std::set_terminate(on_terminate);
    // The levels GLib treats as fatal (G_DEBUG=fatal-criticals adds criticals).
    // GLib can only report them by setting them, so the old value is put back.
    glib_always_fatal = g_log_set_always_fatal(static_cast<GLogLevelFlags>(G_LOG_FATAL_MASK));
    g_log_set_always_fatal(glib_always_fatal);
    // GLib allows one writer per program; this is the only call.
    g_log_set_writer_func(write_glib_log, nullptr, nullptr);
    g_log_set_default_handler(handle_old_glib_log, nullptr);
}

void log_failure(std::string_view what) noexcept
{
    if (failure_log_file.empty()) {
        return;
    }
    try {
        std::error_code error;
        std::filesystem::create_directories(failure_log_file.parent_path(), error);
        const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
        // The whole line is written at once, so lines from two threads do not mix.
        const auto line = std::format("{:%Y-%m-%dT%H:%M:%SZ} {}\n", now, what);
        std::ofstream log(failure_log_file, std::ios::binary | std::ios::app);
        log.write(line.data(), static_cast<std::streamsize>(line.size()));
    } catch (const std::exception&) {
        // Writing the log must never be what ends the program.
    }
}

void guarded(const std::function<void()>& work, const std::function<void(Report)>& show) noexcept
{
    try {
        work();
    } catch (const std::exception& error) {
        log_failure(std::string("exception in a GUI handler: ") + error.what());
        try {
            show(unexpected_error(error.what()));
        } catch (const std::exception& second) {
            log_failure(std::string("exception while showing it: ") + second.what());
        }
    }
}

// ---- Job ------------------------------------------------------------------

// Throttled progress: at most five updates a second are posted to the main loop.
// The update that completes the count always is, so a finished operation's bar
// ends full rather than where the last update before it happened to leave it.
// Hashing fills the bar by bytes and shows the speed and the time left
// (app::ProgressEstimate), worded as on the command line.
class Job::BarProgress final : public IProgressSink {
public:
    BarProgress(const app::Text& text, GtkWidget* bar, Counting counting)
        : text_(text), bar_(bar), counting_(counting)
    {
    }

    void on_progress(const Progress& progress) override
    {
        // The engine reporting progress must not see an exception: it would
        // leave the hash pipeline's threads running. Showing progress is not
        // worth that; the reason goes to the failure log.
        try {
            show(progress);
        } catch (const std::exception& error) {
            log_failure(std::string("exception while showing progress: ") + error.what());
        }
    }

private:
    void show(const Progress& progress)
    {
        const auto now = std::chrono::steady_clock::now();
        // Every report feeds the estimate, also those not shown.
        estimate_.update(progress, now);
        const bool complete = progress.items_total > 0 && progress.items_done == progress.items_total;
        if (!complete && now - last_ < std::chrono::milliseconds(200)) {
            return;
        }
        last_ = now;
        // The text is worded here, on the worker thread: the estimate lives here.
        // Each update holds its own reference to the bar: it may arrive after
        // the window was closed. (The job's reference keeps the bar alive
        // until then, so taking one here on the worker thread is safe.)
        auto update = std::make_unique<Update>(GTK_WIDGET(g_object_ref(bar_)));
        if (progress.items_total == 0) {
            update->pulse = true;
            update->text = text_.format("Scanning: {} entries", progress.items_done);
        } else if (counting_ == Counting::Entries) {
            update->fraction = static_cast<double>(progress.items_done) / static_cast<double>(progress.items_total);
            update->text = text_.format("Comparing: {} / {} entries", progress.items_done, progress.items_total);
        } else {
            update->fraction = estimate_.fraction().value_or(0);
            update->text = text_.format("Hashing: {}", app::describe_progress(text_, estimate_));
        }
        g_idle_add(
            +[](gpointer data) -> gboolean {
                std::unique_ptr<Update> update(static_cast<Update*>(data));
                auto* bar = GTK_PROGRESS_BAR(update->bar);
                if (update->pulse) {
                    gtk_progress_bar_pulse(bar);
                } else {
                    gtk_progress_bar_set_fraction(bar, update->fraction);
                }
                gtk_progress_bar_set_text(bar, update->text.c_str());
                return G_SOURCE_REMOVE;
            },
            update.release());
    }

    struct Update {
        explicit Update(GtkWidget* widget) : bar(widget) {}
        ~Update() { g_object_unref(bar); }
        Update(const Update&) = delete;
        Update& operator=(const Update&) = delete;
        GtkWidget* bar;  // a reference of its own, given back on the main thread
        bool pulse = false;
        double fraction = 0;
        std::string text;
    };
    app::Text text_;
    GtkWidget* bar_;
    Counting counting_;
    // The engines report regularly (hashing five times a second), so a long
    // silence is a pause, such as the computer sleeping.
    app::ProgressEstimate estimate_{app::ProgressEstimate::Reports::regular};
    std::chrono::steady_clock::time_point last_{};
};

Job::~Job()
{
    cancel();
    wait();
}

void Job::wait()
{
    if (thread_.joinable()) {
        thread_.join();
    }
}

void Job::abandon()
{
    abandoned_.store(true);
    cancel();
    wait();
}

void Job::start(const app::Text& text, GtkWidget* progress_bar, Work work, Done done, Counting counting)
{
    wait();
    cancel_.reset();
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(progress_bar), 0.0);
    gtk_progress_bar_set_text(GTK_PROGRESS_BAR(progress_bar), text("Starting...").c_str());

    struct Finished {
        Done done;
        Report report;
        GtkWidget* bar;  // the job's reference, given back on the main thread
        std::atomic<bool>* running;
        const std::atomic<bool>* abandoned;
    };
    // The job keeps the bar alive until it has finished, even if the window closes.
    g_object_ref(progress_bar);
    running_.store(true);
    try {
        thread_ = std::thread([this, text, progress_bar, counting, work = std::move(work),
                                  done = std::move(done)]() mutable {
            BarProgress progress(text, progress_bar, counting);
            Report report;
            try {
                report = work(cancel_, progress);
            } catch (const std::exception& error) {
                log_failure(std::string("exception in an operation: ") + error.what());
                report = unexpected_error(error.what());
            }
            auto* finished = new Finished{std::move(done), std::move(report), progress_bar, &running_, &abandoned_};
            g_idle_add(
                +[](gpointer data) -> gboolean {
                    std::unique_ptr<Finished> finished(static_cast<Finished*>(data));
                    finished->running->store(false);
                    if (!finished->abandoned->load()) {
                        try {
                            finished->done(finished->report);
                        } catch (const std::exception& error) {
                            log_failure(std::string("exception after an operation: ") + error.what());
                        }
                    }
                    g_object_unref(finished->bar);
                    return G_SOURCE_REMOVE;
                },
                finished);
        });
    } catch (...) {
        // No thread could be started (std::system_error): nothing is running.
        running_.store(false);
        g_object_unref(progress_bar);
        throw;
    }
}

} // namespace va::gui
