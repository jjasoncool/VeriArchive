#pragma once
// Shared GUI pieces (GTK4 C API, decision 0016): small widget helpers and Job,
// which runs a long operation on a worker thread while progress and completion
// are delivered on the GTK main thread (the only thread allowed to touch widgets).
#include "core/interfaces.h"

#include <gtk/gtk.h>

#include <atomic>
#include <filesystem>
#include <functional>
#include <string>
#include <thread>

namespace va::gui {

std::string format_bytes(std::uint64_t bytes);

GtkWidget* make_label(const std::string& text, bool selectable = false);
GtkWidget* make_heading(const std::string& text);
GtkWidget* make_text_view();
void set_text(GtkWidget* text_view, const std::string& text);
void set_margins(GtkWidget* widget, int margin);
void clear_grid(GtkWidget* grid);

// Path typed in (or chosen into) an entry.
std::filesystem::path entry_path(GtkWidget* entry);

// Opens a folder chooser; the chosen folder is written into `entry`.
void choose_folder(GtkWindow* parent, GtkWidget* entry);

// One long operation at a time. `work` runs on a worker thread and returns the
// report text; `done` then runs on the main thread. Progress goes to `bar`.
class Job {
public:
    using Work = std::function<std::string(CancelToken&, IProgressSink&)>;
    using Done = std::function<void(const std::string& report)>;

    ~Job();
    bool running() const { return running_.load(); }
    void start(GtkWidget* progress_bar, Work work, Done done);
    void cancel() { cancel_.cancel(); }
    void wait();

private:
    class BarProgress;
    std::thread thread_;
    std::atomic<bool> running_{false};
    CancelToken cancel_;
};

} // namespace va::gui
