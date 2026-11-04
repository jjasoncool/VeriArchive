#pragma once
// Shared GUI pieces (GTK4 C API, decision 0016): small widget helpers, Job,
// which runs a long operation on a worker thread while progress and completion
// are delivered on the GTK main thread (the only thread allowed to touch
// widgets), and the guard that keeps an exception from ending the program.
#include "app/progress_estimate.h"
#include "app/text.h"
#include "core/interfaces.h"
#include "gui/job_shown.h"
#include "gui/report.h"

#include <adwaita.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace va::gui {

// What VeriArchive detects and what this version does not (R-02): every
// verify report carries it (decision 0017), and Settings says it.
inline constexpr const char* guarantee =
    "VeriArchive detects accidental corruption and unintended change; tamper evidence (signed manifests) "
    "is not part of this version.";

std::string format_bytes(std::uint64_t bytes);

GtkWidget* make_label(const std::string& text, bool selectable = false);
// A short caption beside other widgets in a row ("Archive:"). It stays on one
// line: a wrapping label there is given its narrowest width and would show as
// a column of single words or characters.
GtkWidget* make_row_label(const std::string& text);
GtkWidget* make_text_view();
void set_text(GtkWidget* text_view, const std::string& text);
void set_margins(GtkWidget* widget, int margin);
void clear_grid(GtkWidget* grid);

// A device alert above a job's progress (decision 0044): a label in bold, in
// the theme's colour for errors, hidden until an alert is shown in it, and
// hidden again when the job it shows has none. On the main thread.
GtkWidget* make_alert_label();
void show_alert(GtkWidget* label, const std::string& text);
void hide_alert(GtkWidget* label);

// One answer to a question of the window (ask).
struct Answer {
    std::string id;
    std::string label;
    bool destructive = false;  // shown in the theme's colour for what cannot be undone
};

// Asks `heading`, with `body` under it, in an AdwAlertDialog over `parent`
// (decision 0046). answers[0] is the answer that changes nothing (Cancel,
// Keep the window open): it is the default, and the answer when the dialog
// is closed otherwise (Escape). `answered` gets the id of the answer, on the
// main thread; it is never called when the window closes first. An empty
// heading or body is left out.
void ask(GtkWindow* parent, const std::string& heading, const std::string& body, std::vector<Answer> answers,
    std::function<void(const std::string& id)> answered);

// Says `message` in an AdwAlertDialog over `parent` whose one answer is
// Close: a short first line as the heading and the lines after it under it,
// or, where the first line is long, the whole message under no heading
// (alert_words).
void tell(GtkWindow* parent, const app::Text& text, const std::string& message);

// The folder or volume root to register or verify, typed in (or chosen into)
// an entry, made into a root exactly as the command line makes one
// (app::typed_root): '/' and '\', "." and "..", the separators it ends with
// and drive letters made with subst give the same root as there.
std::filesystem::path entry_root(GtkWidget* entry);

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

// Words an operation's progress for a bar (BarState), at most five times a second, and
// always for the report that completes the count, so a finished operation's
// bar ends full rather than where the update before left it. Every report
// feeds the estimate, also those not shown.
class ProgressWording {
public:
    ProgressWording(const app::Text& text, Counting counting) : text_(text), counting_(counting) {}
    std::optional<BarState> update(const Progress& progress);

private:
    app::Text text_;
    Counting counting_;
    // The engines report regularly (hashing five times a second), so a long
    // silence is a pause, such as the computer sleeping.
    app::ProgressEstimate estimate_{app::ProgressEstimate::Reports::regular};
    std::chrono::steady_clock::time_point last_{};
};

// Says in a notification of the system that an operation of the window
// ended, unless a window of the program is the active one, where the
// operator reads its report (decision 0043): its body the report's first two
// lines and its device alerts (ended_notification_body), under `id`: a later
// notification of the same id replaces it. On the main thread.
void notify_if_away(const std::string& report, const std::string& id = "operation-ended");

// One long operation at a time. `work` runs on a worker thread and returns the
// report; `done` then runs on the main thread. Progress goes to `bar`, worded in
// the language of `text`. While `work` runs, the computer stays awake
// (app::keep_awake, decision 0043). Once `done` has run, an operation that
// ended while no window of the program is the active one is told in a
// notification of the system, its body the report's first two lines and its
// device alerts (ended_notification_body), unless notifications are off.
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
    // Off for the screenshot mode and the smoke test, which nobody watches
    // from another window. On the main thread, before start.
    void set_notifying(bool notifying) { notifying_ = notifying; }

private:
    class BarProgress;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> shut_down_{false};
    CancelToken cancel_;
    bool notifying_ = true;
};

} // namespace va::gui
