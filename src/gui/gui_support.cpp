#include "gui/gui_support.h"

#include "app/keep_awake.h"
#include "app/plain_words.h"
#include "app/progress_estimate.h"
#include "app/typed_path.h"
#include "gui/ended_notification.h"
#include "gui/window_words.h"
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

GtkWidget* make_alert_label()
{
    GtkWidget* label = make_label("", true);
    // The theme's red for errors, which libadwaita gives the light and the
    // dark style each its own of: it must stand out from the progress in
    // either.
    gtk_widget_add_css_class(label, "error");
    gtk_widget_set_visible(label, FALSE);
    return label;
}

void show_alert(GtkWidget* label, const std::string& text)
{
    gchar* escaped = g_markup_escape_text(text.c_str(), -1);
    const std::string markup = std::string("<b>") + escaped + "</b>";
    g_free(escaped);
    gtk_label_set_markup(GTK_LABEL(label), markup.c_str());
    gtk_widget_set_visible(label, TRUE);
}

void hide_alert(GtkWidget* label)
{
    gtk_label_set_text(GTK_LABEL(label), "");
    gtk_widget_set_visible(label, FALSE);
}

void ask(GtkWindow* parent, const std::string& heading, const std::string& body, std::vector<Answer> answers,
    std::function<void(const std::string& id)> answered)
{
    if (!parent || answers.empty()) {
        return;
    }
    AdwDialog* dialog =
        adw_alert_dialog_new(heading.empty() ? nullptr : heading.c_str(), body.empty() ? nullptr : body.c_str());
    auto* alert = ADW_ALERT_DIALOG(dialog);
    for (const auto& answer : answers) {
        adw_alert_dialog_add_response(alert, answer.id.c_str(), answer.label.c_str());
        if (answer.destructive) {
            adw_alert_dialog_set_response_appearance(alert, answer.id.c_str(), ADW_RESPONSE_DESTRUCTIVE);
        }
    }
    // The answer that changes nothing is chosen by Enter, Escape and by
    // closing the dialog.
    adw_alert_dialog_set_default_response(alert, answers.front().id.c_str());
    adw_alert_dialog_set_close_response(alert, answers.front().id.c_str());
    using Answered = std::function<void(const std::string&)>;
    g_signal_connect_data(dialog, "response",
        G_CALLBACK(+[](AdwAlertDialog*, const char* response, gpointer data) {
            const auto& answered = *static_cast<Answered*>(data);
            if (answered) {
                answered(response ? response : "");
            }
        }),
        new Answered(std::move(answered)), +[](gpointer data, GClosure*) { delete static_cast<Answered*>(data); },
        GConnectFlags{});
    adw_dialog_present(dialog, GTK_WIDGET(parent));
}

void tell(GtkWindow* parent, const app::Text& text, const std::string& message)
{
    const auto words = alert_words(message);
    ask(parent, words.heading, words.body, {{"close", text("Close")}}, nullptr);
}

std::filesystem::path entry_root(GtkWidget* entry)
{
    return app::typed_root(gtk_editable_get_text(GTK_EDITABLE(entry)));
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

// ---- Progress, notifications -----------------------------------------------

std::optional<BarState> ProgressWording::update(const Progress& progress)
{
    const auto now = std::chrono::steady_clock::now();
    estimate_.update(progress, now);
    const bool complete = progress.items_total > 0 && progress.items_done == progress.items_total;
    if (!complete && now - last_ < std::chrono::milliseconds(200)) {
        return std::nullopt;
    }
    last_ = now;
    BarState bar;
    if (progress.items_total == 0) {
        bar.pulse = true;
        bar.text = text_.format("Scanning: {}", app::count_of(text_, progress.items_done, app::Counted::Entry));
    } else if (counting_ == Counting::Entries) {
        bar.fraction = static_cast<double>(progress.items_done) / static_cast<double>(progress.items_total);
        // Two calls: the compiler checks each literal template against its values.
        bar.text = progress.items_total == 1
                       ? text_.format("Comparing: {} / {} entry", progress.items_done, progress.items_total)
                       : text_.format("Comparing: {} / {} entries", progress.items_done, progress.items_total);
    } else {
        bar.fraction = estimate_.fraction().value_or(0);
        bar.text = text_.format("Hashing: {}", app::describe_progress(text_, estimate_));
    }
    return bar;
}

void notify_if_away(const std::string& report, const std::string& id)
{
    GApplication* application = g_application_get_default();
    if (!application || !GTK_IS_APPLICATION(application)) {
        return;
    }
    for (GList* window = gtk_application_get_windows(GTK_APPLICATION(application)); window;
         window = window->next) {
        if (gtk_window_is_active(GTK_WINDOW(window->data))) {
            return;
        }
    }
    // GLib's GNotification: shown on Windows from the notification area, on
    // Linux by the desktop's notification service. A later one with the same
    // id replaces it.
    GNotification* notification = g_notification_new("VeriArchive");
    g_notification_set_body(notification, ended_notification_body(report).c_str());
    g_application_send_notification(application, id.c_str(), notification);
    g_object_unref(notification);
}

// ---- Job ------------------------------------------------------------------

// The bar of the job's progress, updated at most five times a second
// (ProgressWording), on the main thread.
class Job::BarProgress final : public IProgressSink {
public:
    BarProgress(const app::Text& text, GtkWidget* bar, Counting counting) : bar_(bar), wording_(text, counting) {}

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
        // The text is worded here, on the worker thread: the estimate lives
        // here. Each update holds its own reference to the bar: it may arrive
        // after the window was closed. (The job's reference keeps the bar
        // alive until then, so taking one here on the worker thread is safe.)
        auto state = wording_.update(progress);
        if (!state) {
            return;
        }
        auto update = std::make_unique<Update>(GTK_WIDGET(g_object_ref(bar_)), std::move(*state));
        g_idle_add(
            +[](gpointer data) -> gboolean {
                std::unique_ptr<Update> update(static_cast<Update*>(data));
                auto* bar = GTK_PROGRESS_BAR(update->bar);
                if (update->state.pulse) {
                    gtk_progress_bar_pulse(bar);
                } else {
                    gtk_progress_bar_set_fraction(bar, update->state.fraction);
                }
                gtk_progress_bar_set_text(bar, update->state.text.c_str());
                return G_SOURCE_REMOVE;
            },
            update.release());
    }

    struct Update {
        Update(GtkWidget* widget, BarState shown) : bar(widget), state(std::move(shown)) {}
        ~Update() { g_object_unref(bar); }
        Update(const Update&) = delete;
        Update& operator=(const Update&) = delete;
        GtkWidget* bar;  // a reference of its own, given back on the main thread
        BarState state;
    };
    GtkWidget* bar_;
    ProgressWording wording_;
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

void Job::shut_down()
{
    shut_down_.store(true);
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
        const std::atomic<bool>* shut_down;
        app::Text text;
        bool notifying;
    };
    // The job keeps the bar alive until it has finished, even if the window closes.
    g_object_ref(progress_bar);
    running_.store(true);
    try {
        thread_ = std::thread([this, text, progress_bar, counting, work = std::move(work), done = std::move(done),
                                  notifying = notifying_]() mutable {
            // The computer stays awake until the operation ends (decision 0043).
            const auto awake = app::keep_awake(text);
            BarProgress progress(text, progress_bar, counting);
            Report report;
            try {
                report = work(cancel_, progress);
            } catch (const std::exception& error) {
                log_failure(std::string("exception in an operation: ") + error.what());
                report = unexpected_error(error.what());
            }
            auto* finished = new Finished{
                std::move(done), std::move(report), progress_bar, &running_, &shut_down_, text, notifying};
            g_idle_add(
                +[](gpointer data) -> gboolean {
                    std::unique_ptr<Finished> finished(static_cast<Finished*>(data));
                    finished->running->store(false);
                    if (!finished->shut_down->load()) {
                        try {
                            finished->done(finished->report);
                            if (finished->notifying) {
                                notify_if_away(finished->report(finished->text));
                            }
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
