// The session (repository) shared by every page, the notice while its open
// upgrades the database, the start, and the language change that rebuilds the
// window (window.cpp builds it).
#include "gui/gui_app.h"

#include <exception>
#include <expected>
#include <memory>
#include <optional>
#include <string_view>

// Last: on Windows libintl.h redefines printf and its relatives as macros,
// which should not reach the other headers.
#include <libintl.h>

namespace va::gui {
namespace {

// Why no repository is open, followed by what holds all the same: the
// warnings, as running elevated may be why (R-12, decision 0037); what
// opening it did, said once (the upgrade of its database, and where the copy
// from before it is); and where each copy of its database kept before an
// upgrade is in the RepositoryRoot that the open resolved, read again whenever
// it is shown (decision 0032). Worded in the language shown, as
// app::session_failure_in_window words it.
Report session_failure(app::SessionFailure failure)
{
    return [failure = std::move(failure)](const app::Text& text) {
        return app::session_failure_in_window(text, failure);
    };
}

} // namespace

void open_repository_session(GuiState& state, bool initialise)
{
    // An exception from the engines (a std::filesystem error, say) is one more
    // reason the repository could not be opened, shown in Settings.
    try {
        auto session = app::open_session({initialise, std::nullopt, std::nullopt, {}, false, std::nullopt});
        if (session) {
            state.session = std::move(*session);
            state.session_error = nullptr;
        } else {
            // The warnings too, which hold where no repository could be
            // opened: running elevated may be why (R-12, decision 0037).
            state.session.reset();
            state.session_error = session_failure(std::move(session.error()));
        }
    } catch (const std::exception& error) {
        log_failure(std::string("exception while opening the repository: ") + error.what());
        state.session.reset();
        state.session_error = unexpected_error(error.what());
    }
    // The check of the chain was of the repository open before, and the
    // filter may name an archive of it.
    state.history_check.reset();
    state.history_filter.reset();
    refresh_shell(state);
    refresh_settings(state);
    refresh_archives(state);
    refresh_history(state);
    // The attached devices of the last read belong to the repository open
    // before (attached_now): the archives of this one are matched anew, at
    // once where Archives or Devices is shown.
    refresh_devices(state);
    if (state.devices.timer != 0) {
        read_devices(state, DevicesReadWhy::Refresh);
    }
}

namespace {

// What the user chose in the window, carried over when a language change
// replaces it. (What the views show is kept in GuiState as Reports; the Jobs
// page and the header's popover show the jobs as they are. Register Archive
// and the comparison dialog cover the window, Settings' language row too, so
// none is open when the language changes.) The window's position cannot be
// kept: GTK 4 has no way to set it.
struct Kept {
    int width = 0;
    int height = 0;
    bool maximized = false;
    std::string page;                    // the page shown (a child of state.stack)
    bool page_shown = false;             // a narrow window shows the page, not the sidebar
    bool details_shown = false;          // ... and of Archives the details of an archive, not the list
    // The runs whose result pages are open over Archives and over History.
    // (History's filter and its check of the chain are kept in GuiState.)
    std::vector<std::string> archives_results;
    std::vector<std::string> history_results;
};

Kept keep(const GuiState& state)
{
    Kept kept;
    // GTK keeps the default size up to date as the user resizes, and it is the
    // size a maximized window returns to.
    gtk_window_get_default_size(state.window, &kept.width, &kept.height);
    kept.maximized = gtk_window_is_maximized(state.window) != FALSE;
    if (const char* page = gtk_stack_get_visible_child_name(GTK_STACK(state.stack))) {
        kept.page = page;
    }
    kept.page_shown =
        adw_navigation_split_view_get_show_content(ADW_NAVIGATION_SPLIT_VIEW(state.shell.split)) != FALSE;
    kept.details_shown =
        adw_navigation_split_view_get_show_content(ADW_NAVIGATION_SPLIT_VIEW(state.archives_page.split)) != FALSE;

    kept.archives_results = results_open(state, ResultsOver::Archives);
    kept.history_results = results_open(state, ResultsOver::History);
    return kept;
}

// Puts what `keep` read into the new window, after the views were refreshed.
void restore(GuiState& state, const Kept& kept)
{
    if (kept.width > 0 && kept.height > 0) {
        gtk_window_set_default_size(state.window, kept.width, kept.height);
    }
    if (kept.maximized) {
        gtk_window_maximize(state.window);
    }

    open_results_again(state, ResultsOver::Archives, kept.archives_results);
    open_results_again(state, ResultsOver::History, kept.history_results);
}

} // namespace

namespace {

// Whether the open of the repository that open_repository_session(state,
// initialise) makes will upgrade its database (decision 0032), keeping a copy
// of it first, or wait for another process that does: known at once, without
// the write lock. When it cannot be told, the open goes as any open does.
bool upgrade_ahead(bool initialise) noexcept
{
    try {
        return app::schema_upgrade_ahead({initialise, std::nullopt, std::nullopt, {}, false, std::nullopt}).has_value();
    } catch (const std::exception& error) {
        log_failure(std::string("exception while looking for an upgrade of the repository database: ") +
                    error.what());
        return false;
    }
}

// The notice that the window shows in place of its pages while the open
// upgrades the repository's database (decision 0032).
struct UpgradeNotice {
    GtkWidget* heading = nullptr;  // AdwStatusPage, its title the heading
    GtkWidget* body = nullptr;
    GtkWidget* bar = nullptr;
};

// What the timer that pulses the notice's progress bar holds: the bar, with a
// reference of its own, until the open is done.
struct Pulse {
    GuiState* state;
    GtkWidget* bar;
};

} // namespace

bool open_with_upgrade_notice(GuiState& state, bool initialise)
{
    // The sample repository of the screenshots is new; and a job running
    // already (none runs while no repository is open) keeps the open as it is.
    if (state.screenshot_folder || state.job.running() || state.jobs.active() || !upgrade_ahead(initialise)) {
        return false;
    }
    const auto& text = state.text;
    auto* window = ADW_APPLICATION_WINDOW(state.window);
    GtkWidget* views = adw_application_window_get_content(window);
    g_object_ref(views);  // kept while the notice takes their place
    // A status page under a header bar of its own, which keeps the window's
    // title and its buttons.
    UpgradeNotice notice;
    notice.heading = adw_status_page_new();
    adw_status_page_set_title(ADW_STATUS_PAGE(notice.heading), text("Upgrading the repository database...").c_str());
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 18);
    notice.body = make_label(text("VeriArchive first keeps a copy of the database from before the upgrade, beside it. "
                                  "For a large repository this takes a while; the views open when it is done."));
    gtk_label_set_justify(GTK_LABEL(notice.body), GTK_JUSTIFY_CENTER);
    gtk_label_set_xalign(GTK_LABEL(notice.body), 0.5f);
    notice.bar = gtk_progress_bar_new();
    gtk_box_append(GTK_BOX(box), notice.body);
    gtk_box_append(GTK_BOX(box), notice.bar);
    GtkWidget* clamp = adw_clamp_new();
    adw_clamp_set_maximum_size(ADW_CLAMP(clamp), 560);
    adw_clamp_set_child(ADW_CLAMP(clamp), box);
    adw_status_page_set_child(ADW_STATUS_PAGE(notice.heading), clamp);
    GtkWidget* page = adw_toolbar_view_new();
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(page), adw_header_bar_new());
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(page), notice.heading);
    adw_application_window_set_content(window, page);
    gtk_window_present(state.window);

    // Why no repository is open, in place of the notice; it can be selected
    // and copied, as in Settings.
    const auto show_failure = [&state, notice](const Report& why) {
        adw_status_page_set_title(ADW_STATUS_PAGE(notice.heading), state.text("No repository is open.").c_str());
        adw_status_page_set_icon_name(ADW_STATUS_PAGE(notice.heading), "dialog-warning-symbolic");
        gtk_label_set_text(GTK_LABEL(notice.body), why ? why(state.text).c_str() : "");
        gtk_label_set_selectable(GTK_LABEL(notice.body), TRUE);
        gtk_label_set_justify(GTK_LABEL(notice.body), GTK_JUSTIFY_LEFT);
        gtk_label_set_xalign(GTK_LABEL(notice.body), 0.0f);
        gtk_widget_set_visible(notice.bar, FALSE);
    };
    auto opened = std::make_shared<std::optional<std::expected<app::Session, app::SessionFailure>>>();
    state.job.start(state.text, notice.bar,
        [opened, initialise](CancelToken&, IProgressSink&) -> Report {
            *opened = app::open_session({initialise, std::nullopt, std::nullopt, {}, false, std::nullopt});
            return nullptr;
        },
        [&state, opened, views, show_failure](const Report& report) {
            if (!state.window) {
                return;  // shutting down
            }
            bool shown = false;
            guarded(
                [&] {
                    // Nothing when the open threw: `report` says what.
                    auto& session = *opened;
                    if (!session || !*session) {
                        // What opening it did all the same follows why (where
                        // the copy is, when the open failed after the
                        // upgrade), then where each copy is, as in Settings.
                        state.session.reset();
                        state.session_error = session ? session_failure(std::move(session->error())) : report;
                        show_failure(state.session_error);
                        return;
                    }
                    state.session = std::move(**session);
                    state.session_error = nullptr;
                    adw_application_window_set_content(ADW_APPLICATION_WINDOW(state.window), views);
                    g_object_unref(views);
                    shown = true;
                    refresh_shell(state);
                    refresh_settings(state);
                    refresh_archives(state);
                    refresh_history(state);
                    // Settings says once where the copy of the database is.
                    if (!state.session->notes.empty()) {
                        show_page(state, "settings");
                    }
                },
                [&](const Report& error) {
                    if (!shown) {
                        show_failure(error);
                        return;
                    }
                    // As when the views cannot be filled at the start.
                    g_printerr("%s\n", error(state.text).c_str());
                    state.exit_status = 1;
                    g_application_quit(G_APPLICATION(state.application));
                });
        });
    g_timeout_add_full(G_PRIORITY_DEFAULT, 100,
        +[](gpointer data) -> gboolean {
            auto* pulse = static_cast<Pulse*>(data);
            if (!pulse->state->job.running()) {
                return G_SOURCE_REMOVE;
            }
            gtk_progress_bar_pulse(GTK_PROGRESS_BAR(pulse->bar));
            return G_SOURCE_CONTINUE;
        },
        new Pulse{&state, GTK_WIDGET(g_object_ref(notice.bar))},
        +[](gpointer data) {
            auto* pulse = static_cast<Pulse*>(data);
            g_object_unref(pulse->bar);
            delete pulse;
        });
    return true;
}

void activate(GuiState& state, bool smoke_test)
{
    build_window(state);
    // An open that upgrades the repository's database shows the window at
    // once, with a notice (decision 0032); the smoke test shows nothing.
    if (!smoke_test && open_with_upgrade_notice(state, false)) {
        return;
    }
    open_repository_session(state, false);
    // Settings says why no repository is open, or, once, that its database
    // was upgraded and where the copy from before the upgrade is (decision
    // 0032).
    if (!state.session || !state.session->notes.empty()) {
        show_page(state, "settings");
    }

    if (smoke_test) {
        // Like the Settings drop-down, rebuild once this handler has returned:
        // in the other language and back, then quit. Nothing is shown and the
        // config file is not written.
        g_idle_add(
            +[](gpointer data) -> gboolean {
                auto& state = *static_cast<GuiState*>(data);
                guarded(
                    [&] {
                        if (!state.window) {
                            return;  // shutting down
                        }
                        // The result pages' read speed chart, which the
                        // empty repository gives no run to draw.
                        check_sample_chart(state);
                        const auto start = state.text.language();
                        const auto other = start == app::Language::English ? app::Language::TraditionalChinese
                                                                           : app::Language::English;
                        rebuild_window(state, other, false);
                        rebuild_window(state, start, false);
                    },
                    [&](const Report& report) {
                        g_printerr("%s\n", report(state.text).c_str());
                        state.exit_status = 1;
                    });
                g_application_quit(G_APPLICATION(state.application));
                return G_SOURCE_REMOVE;
            },
            &state);
        return;
    }
    gtk_window_present(state.window);
    if (state.screenshot_folder) {
        start_screenshot_tour(state);
    }
}

void use_gtk_language(app::Language language)
{
    // GTK translates its own texts with gettext, which takes LANGUAGE ahead of
    // the system language. The Windows package ships GTK's zh_TW catalogs.
    // (Changing the environment is unsafe while another thread reads it; a
    // language change is refused while an operation runs, and waits for a
    // read of the attached devices in flight, change_language.)
    g_setenv("LANGUAGE", language == app::Language::TraditionalChinese ? "zh_TW" : "en", TRUE);
    // gettext keeps the translations it has already found and does not notice
    // a new LANGUAGE; setting the text domain again makes it look them up
    // afresh. Without this, GTK's own menus stay Chinese after a switch to English.
    textdomain(textdomain(nullptr));
}

void change_language(GuiState& state, app::Language language)
{
    // No window: the application is shutting down (see main). The language in
    // use chosen again: a change that waited for a read is not made.
    if (!state.window || language == state.text.language()) {
        state.devices.language_after.reset();
        return;
    }
    if (state.job.running() || state.jobs.active()) {
        // A job started after the choice was made (the drop-down is locked
        // while one waits or works). It reports into this window, so the
        // window stays and the drop-down shows the language in use again.
        show_language_in_use(state);
        return;
    }
    if (state.devices.busy) {
        // A read of the attached devices is in flight: on Linux it asks
        // udisks, which reads the environment, and use_gtk_language writes
        // it, which is unsafe while another thread reads it. The change comes
        // once the read is back (devices_poll.cpp), without holding the
        // window up meanwhile, as a device that does not answer would.
        state.devices.language_after = language;
        return;
    }
    state.devices.language_after.reset();
    state.settings_page.notice = nullptr;
    if (auto saved = app::save_language(language, std::nullopt); !saved) {
        state.settings_page.notice = fixed_message(saved.error());
    }
    rebuild_window(state, language, true);
}

void rebuild_window(GuiState& state, app::Language language, bool show)
{
    // Read the old window before build_window points `state` at the new widgets.
    const Kept kept = keep(state);
    state.text = app::Text(language);
    use_gtk_language(language);

    // The new window is built before the old one closes, so the application
    // always has a window and keeps running. A job's dialog of details
    // belongs to the old one; the new window shows it again. Register
    // Archive and the comparison dialog go with it.
    const auto details = close_job_details(state);
    close_dialogs(state);
    GtkWindow* old_window = state.window;
    build_window(state);
    const auto replace_old_window = [&] {
        // The page shown before; Settings, where the language was changed,
        // when none was.
        gtk_stack_set_visible_child_name(GTK_STACK(state.stack), kept.page.empty() ? "settings" : kept.page.c_str());
        adw_navigation_split_view_set_show_content(ADW_NAVIGATION_SPLIT_VIEW(state.shell.split), kept.page_shown);
        adw_navigation_split_view_set_show_content(
            ADW_NAVIGATION_SPLIT_VIEW(state.archives_page.split), kept.details_shown);
        if (show) {
            gtk_window_present(state.window);
        }
        gtk_window_destroy(old_window);
    };
    try {
        refresh_shell(state);
        refresh_settings(state);
        refresh_archives(state);
        refresh_history(state);
        // The details of an archive, made anew, show its job as it is.
        restore(state, kept);
        lock_language(state);
    } catch (...) {
        // `state` already points at the new window, so it replaces the old
        // one even when filling it failed; the caller reports the error.
        replace_old_window();
        throw;
    }
    replace_old_window();
    if (show) {
        for (const auto job : details) {
            open_job_details(state, job);
        }
    }
}

} // namespace va::gui
