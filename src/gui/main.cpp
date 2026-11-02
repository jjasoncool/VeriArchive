// VeriArchive GUI entry point. It runs unprivileged and never elevates itself
// (R-08, R-12). VERIARCHIVE_GUI_SMOKE_TEST=1 builds every view without showing
// the window, rebuilds it in the other language and back (as a language change
// does), and exits, so a build can be checked without a desktop.
// VERIARCHIVE_GUI_SCREENSHOTS=<folder> takes the documentation screenshots into
// that folder and exits (tools/screenshots); the views then show a sample media
// identity (displayed_identity).
#include "gui/gui_app.h"

#include "common/utf8.h"

#include <cstdlib>
#include <exception>
#include <string>

int main(int argc, char* argv[])
{
    // First, so that even a failure while starting is recorded.
    va::gui::install_failure_log();

    va::gui::GuiState state;
    state.host = va::app::host_info(VERIARCHIVE_VERSION);
    try {
        state.text = va::app::Text(va::app::interface_language(std::nullopt, std::nullopt));
    } catch (const std::exception& error) {
        // Start in English; Settings will show what is wrong with the config.
        va::gui::log_failure(std::string("exception while choosing the language: ") + error.what());
    }
    va::gui::use_gtk_language(state.text.language());
    static bool smoke_test = std::getenv("VERIARCHIVE_GUI_SMOKE_TEST") != nullptr;
    // g_getenv gives UTF-8 on Windows whatever the code page, so a folder with
    // any name arrives intact (on Linux the bytes are the path).
    if (const char* folder = g_getenv("VERIARCHIVE_GUI_SCREENSHOTS"); folder && *folder) {
        state.screenshot_folder = va::path_from_utf8(folder);
        // The pictures are published, so from the first frame on the views
        // show the sample identity, never that of the volumes the sample
        // archives lie on. The smoke test takes precedence and shows nothing.
        if (!smoke_test) {
            state.identity_shown = va::gui::IdentityShown::Sample;
        }
    }

    // Windows has no D-Bus session bus. Without an address GLib would try to
    // start one (a background gdbus.exe) or warn that it cannot; an unusable
    // address makes it skip D-Bus quietly. Linux desktops keep their bus.
    if (VERIARCHIVE_WITHOUT_SESSION_BUS && !g_getenv("DBUS_SESSION_BUS_ADDRESS")) {
        g_setenv("DBUS_SESSION_BUS_ADDRESS", "disabled:", TRUE);
    }
    // Nobody watches the screenshot mode or the smoke test from another
    // window: their operations end without a notification (decision 0043).
    state.job.set_notifying(!smoke_test && !state.screenshot_folder);
    state.application = gtk_application_new("io.veriarchive.Gui", G_APPLICATION_NON_UNIQUE);
    g_signal_connect(state.application, "activate",
        G_CALLBACK(+[](GtkApplication*, gpointer data) {
            auto& state = *static_cast<va::gui::GuiState*>(data);
            va::gui::guarded([&] { va::gui::activate(state, smoke_test); },
                [&](const va::gui::Report& report) {
                    // Without a window there is nothing to continue with; the
                    // reason is in the failure log (and on stderr).
                    g_printerr("%s\n", report(state.text).c_str());
                    state.exit_status = 1;
                    g_application_quit(G_APPLICATION(state.application));
                });
        }),
        &state);
    // Closing the window cancels a running operation and waits for it, so an
    // interrupted run ends as INCOMPLETE with its checkpoints saved.
    g_signal_connect(state.application, "shutdown",
        G_CALLBACK(+[](GApplication*, gpointer data) {
            auto& state = *static_cast<va::gui::GuiState*>(data);
            va::gui::guarded([&] { state.job.shut_down(); }, [](const va::gui::Report&) {});
            // The window is gone. GLib still runs the callbacks already queued
            // (a language change, say); they check this and do nothing.
            state.window = nullptr;
        }),
        &state);
    const int status = g_application_run(G_APPLICATION(state.application), argc, argv);
    g_object_unref(state.application);
    return status != 0 ? status : state.exit_status;
}
