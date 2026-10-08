#include "gui/gui_support.h"

#include "common/utf8.h"

#include <chrono>
#include <format>
#include <memory>

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

void choose_folder(GtkWindow* parent, GtkWidget* entry)
{
    GtkFileDialog* dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "Choose the archive folder");
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
            g_object_unref(source);
        },
        entry);
}

// Throttled progress: at most five updates a second are posted to the main loop.
class Job::BarProgress final : public IProgressSink {
public:
    explicit BarProgress(GtkWidget* bar) : bar_(bar) {}

    void on_progress(const Progress& progress) override
    {
        const auto now = std::chrono::steady_clock::now();
        if (now - last_ < std::chrono::milliseconds(200)) {
            return;
        }
        last_ = now;
        auto* update = new Update{bar_, progress};
        g_idle_add(
            +[](gpointer data) -> gboolean {
                std::unique_ptr<Update> update(static_cast<Update*>(data));
                const auto& p = update->progress;
                if (p.items_total == 0) {
                    gtk_progress_bar_pulse(GTK_PROGRESS_BAR(update->bar));
                    const auto text = "Scanning: " + std::to_string(p.items_done) + " entries";
                    gtk_progress_bar_set_text(GTK_PROGRESS_BAR(update->bar), text.c_str());
                } else {
                    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(update->bar),
                        static_cast<double>(p.items_done) / static_cast<double>(p.items_total));
                    const auto text = "Hashing: " + std::to_string(p.items_done) + " / " +
                                      std::to_string(p.items_total) + " files, " + format_bytes(p.bytes_done);
                    gtk_progress_bar_set_text(GTK_PROGRESS_BAR(update->bar), text.c_str());
                }
                return G_SOURCE_REMOVE;
            },
            update);
    }

private:
    struct Update {
        GtkWidget* bar;
        Progress progress;
    };
    GtkWidget* bar_;
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

void Job::start(GtkWidget* progress_bar, Work work, Done done)
{
    wait();
    cancel_.reset();
    running_.store(true);
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(progress_bar), 0.0);
    gtk_progress_bar_set_text(GTK_PROGRESS_BAR(progress_bar), "Starting...");

    struct Finished {
        Done done;
        std::string report;
        std::atomic<bool>* running;
    };
    thread_ = std::thread([this, progress_bar, work = std::move(work), done = std::move(done)]() mutable {
        BarProgress progress(progress_bar);
        std::string report;
        try {
            report = work(cancel_, progress);
        } catch (const std::exception& error) {
            report = std::string("Unexpected error: ") + error.what();
        }
        auto* finished = new Finished{std::move(done), std::move(report), &running_};
        g_idle_add(
            +[](gpointer data) -> gboolean {
                std::unique_ptr<Finished> finished(static_cast<Finished*>(data));
                finished->running->store(false);
                finished->done(finished->report);
                return G_SOURCE_REMOVE;
            },
            finished);
    });
}

} // namespace va::gui
