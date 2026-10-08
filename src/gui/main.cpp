// VeriArchive GUI entry point. It runs unprivileged and never elevates itself
// (R-08, R-12). VERIARCHIVE_GUI_SMOKE_TEST=1 builds every view without showing
// the window and exits, so a build can be checked without a desktop.
#include "gui/gui_app.h"

#include <cstdlib>

int main(int argc, char* argv[])
{
    va::gui::GuiState state;
    state.host = va::app::host_info(VERIARCHIVE_VERSION);
    static bool smoke_test = std::getenv("VERIARCHIVE_GUI_SMOKE_TEST") != nullptr;

    state.application = gtk_application_new("io.veriarchive.Gui", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(state.application, "activate",
        G_CALLBACK(+[](GtkApplication*, gpointer data) {
            va::gui::activate(*static_cast<va::gui::GuiState*>(data), smoke_test);
        }),
        &state);
    // Closing the window cancels a running operation and waits for it, so an
    // interrupted run ends as INCOMPLETE with its checkpoints saved.
    g_signal_connect(state.application, "shutdown",
        G_CALLBACK(+[](GApplication*, gpointer data) {
            auto& state = *static_cast<va::gui::GuiState*>(data);
            state.job.cancel();
            state.job.wait();
        }),
        &state);
    const int status = g_application_run(G_APPLICATION(state.application), argc, argv);
    g_object_unref(state.application);
    return status;
}
