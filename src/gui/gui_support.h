#pragma once
// Shared GUI pieces (GTK4 C API, decision 0016): small widget helpers, Job,
// which runs a long operation on a worker thread while progress and completion
// are delivered on the GTK main thread (the only thread allowed to touch
// widgets), and the guard that keeps an exception from ending the program.
#include "app/text.h"
#include "core/interfaces.h"

#include <gtk/gtk.h>

#include <atomic>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace va::gui {

// A message or report that can be worded in any interface language. A view
// keeps the one it shows, so a language change can show it again in the new
// language (decision 0017).
using Report = std::function<std::string(const app::Text&)>;

// A fixed English message (or engine sentence) as a Report.
Report fixed_message(std::string english);

// "Unexpected error: ..." for an exception's message.
Report unexpected_error(std::string what);

std::string format_bytes(std::uint64_t bytes);

GtkWidget* make_label(const std::string& text, bool selectable = false);
// A short caption beside other widgets in a row ("Archive:"). It stays on one
// line: a wrapping label there is given its narrowest width and would show as
// a column of single words or characters.
GtkWidget* make_row_label(const std::string& text);
GtkWidget* make_heading(const std::string& text);
GtkWidget* make_text_view();
void set_text(GtkWidget* text_view, const std::string& text);
void set_margins(GtkWidget* widget, int margin);
void clear_grid(GtkWidget* grid);

// Path typed in (or chosen into) an entry.
std::filesystem::path entry_path(GtkWidget* entry);

// The archive chosen in a drop-down whose item `offset + i` is uuids[i], or
// nothing when another item (such as "The folder or volume above") is chosen.
std::optional<Uuid> chosen_archive(GtkWidget* drop_down, const std::vector<Uuid>& uuids, guint offset);

// Chooses `archive` again, if the drop-down still lists it.
void choose_archive(GtkWidget* drop_down, const std::vector<Uuid>& uuids, guint offset,
    const std::optional<Uuid>& archive);

// Opens a folder chooser titled `title`; the chosen folder is written into `entry`.
void choose_folder(GtkWindow* parent, GtkWidget* entry, const std::string& title);

// GTK calls signal handlers from C code, which a C++ exception cannot pass
// through: the program would end at once (std::terminate). Every handler
// therefore runs its work through `guarded`. An exception is recorded in the
// failure log and handed to `show` as "Unexpected error: ...", for the view the
// handler belongs to; the program keeps running.
void guarded(const std::function<void()>& work, const std::function<void(Report)>& show) noexcept;

// The failure log, veriarchive-gui.log in the user config folder (where
// config.json lives unless the program is portable). install_failure_log()
// makes the program record why it ends when it ends abnormally (an uncaught
// exception, a fatal GLib message); call it once, first thing in main.
void install_failure_log();
void log_failure(std::string_view what) noexcept;

// What the items of a progress update are: files read and hashed (register,
// verify), or manifest entries compared, with no bytes read.
enum class Counting { Files, Entries };

// One long operation at a time. `work` runs on a worker thread and returns the
// report; `done` then runs on the main thread. Progress goes to `bar`, worded in
// the language of `text`.
class Job {
public:
    using Work = std::function<Report(CancelToken&, IProgressSink&)>;
    using Done = std::function<void(const Report& report)>;

    ~Job();
    bool running() const { return running_.load(); }
    void start(const app::Text& text, GtkWidget* progress_bar, Work work, Done done,
        Counting counting = Counting::Files);
    void cancel() { cancel_.cancel(); }
    void wait();
    // Cancels the operation and waits for it; its `done` is then never called,
    // because the window it would report into is gone. For the application's
    // shutdown.
    void shut_down();

private:
    class BarProgress;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> shut_down_{false};
    CancelToken cancel_;
};

} // namespace va::gui
