// The window (decision 0046): an AdwApplicationWindow whose
// AdwNavigationSplitView puts a sidebar of the pages beside the page shown,
// each under a header bar of its own: above the sidebar VeriArchive, the
// folder of the open repository and Register Archive (an icon, so that the
// title beside it is never cut), which opens its dialog (register_wizard.cpp);
// above the page its title and the jobs
// button. A narrow window (AdwBreakpoint) shows one of the two at a time, and
// Archives its list or the details of an archive. Toasts come over the whole
// window. Closing it while jobs run asks first.
#include "gui/gui_app.h"

#include "gui/window_words.h"

#include <cstring>
#include <memory>
#include <string_view>

namespace va::gui {
namespace {

// A page of the window, a row of the sidebar: the name of its child in
// state.stack, its icon, its title (English, worded where shown). Register
// Archive and the comparison are dialogs: the button above the sidebar opens
// the one, an archive's details the other.
struct Page {
    const char* name;
    const char* icon;
    const char* title;
};

constexpr Page pages[] = {
    {"archives", "package-x-generic-symbolic", "Archives"},
    {"devices", "drive-harddisk-symbolic", "Devices"},
    {"jobs", jobs_icon, "Jobs"},
    {"history", "document-open-recent-symbolic", "History"},
    {"settings", "emblem-system-symbolic", "Settings"},
};

const Page* page_named(std::string_view name)
{
    for (const auto& page : pages) {
        if (name == page.name) {
            return &page;
        }
    }
    return nullptr;
}

// The window's own style (decision 0046), in the theme's named colours, so it
// follows the light and the dark style: the count of jobs that run or wait,
// and the colour of how far they are on the jobs button's ring; in Archives
// the status dot of an archive, the pills of results and marks, the banner of
// an archive's result and the lines of its device alerts and notices, each in
// the colour of its tone (tone_class), as is a notice of an archive not
// attached as recorded (va-warning); the list's pane on the page's own
// background, and the items of the details' menus; on a result page the
// colours of its read speed chart and of the swatches of its legend, which
// the chart takes its colours from, and its metrics; the count of unfinished
// operations that stopped, in the colour of warnings, the tiles of the
// overview of Archives as cards, the one chosen outlined in the accent
// colour, and the line under the head of an archive's details once they
// scroll under it (decision 0049); in Devices the model of
// each device, at the head of its card.
constexpr const char* style = R"css(
.va-ring {
    color: @accent_color;
}
.va-dot {
    min-width: 10px;
    min-height: 10px;
    border-radius: 999px;
    background-color: alpha(@window_fg_color, 0.35);
}
.va-dot.va-success {
    background-color: @success_color;
}
.va-dot.va-warning {
    background-color: @warning_color;
}
.va-dot.va-error {
    background-color: @error_color;
}
.va-pill {
    border-radius: 999px;
    padding: 1px 9px;
    font-size: smaller;
    font-weight: bold;
    background-color: alpha(@window_fg_color, 0.08);
    color: alpha(@window_fg_color, 0.7);
}
.va-pill.va-success {
    background-color: alpha(@success_color, 0.16);
    color: @success_color;
}
.va-pill.va-warning {
    background-color: alpha(@warning_color, 0.18);
    color: @warning_color;
}
.va-pill.va-error {
    background-color: alpha(@error_color, 0.14);
    color: @error_color;
}
.va-banner {
    border-radius: 12px;
    padding: 14px 16px 14px 18px;
    background-color: alpha(@window_fg_color, 0.05);
}
.va-banner.va-success {
    background-color: alpha(@success_color, 0.12);
}
.va-banner.va-warning {
    background-color: alpha(@warning_color, 0.14);
}
.va-banner.va-error {
    background-color: alpha(@error_color, 0.11);
}
.va-banner-icon {
    min-width: 40px;
    min-height: 40px;
    border-radius: 999px;
    background-color: alpha(@window_fg_color, 0.45);
    color: @window_bg_color;
}
.va-banner-icon.va-success {
    background-color: @success_bg_color;
    color: @success_fg_color;
}
.va-banner-icon.va-warning {
    background-color: @warning_bg_color;
    color: @warning_fg_color;
}
.va-banner-icon.va-error {
    background-color: @error_bg_color;
    color: @error_fg_color;
}
.va-line {
    border-radius: 8px;
    padding: 10px 14px;
}
.va-line.va-error {
    background-color: alpha(@error_color, 0.11);
}
.va-line.va-neutral {
    background-color: alpha(@window_fg_color, 0.06);
}
.va-line.va-warning {
    background-color: alpha(@warning_color, 0.14);
}
.va-line.va-success {
    background-color: alpha(@success_color, 0.12);
}
.va-chart {
    color: alpha(@window_fg_color, 0.7);
}
.va-chart-now {
    color: @accent_color;
}
.va-chart-before {
    color: alpha(@window_fg_color, 0.6);
}
.va-chart-slower {
    color: @error_color;
}
.va-swatch {
    min-width: 16px;
    min-height: 3px;
    border-radius: 2px;
    background-color: currentColor;
}
.va-swatch.va-chart-before {
    min-height: 0;
    border-top: 2px dashed currentColor;
    background-color: transparent;
}
.va-swatch.va-chart-slower {
    min-height: 10px;
    background-color: alpha(@error_color, 0.25);
}
.va-metric {
    padding: 12px 16px;
}
navigation-split-view.va-archives > .sidebar-pane {
    background-color: transparent;
    color: inherit;
    box-shadow: none;
}
.va-menu-item {
    padding: 6px 10px;
}
row.va-device-head label.title {
    font-weight: bold;
}
.va-count {
    background-color: @accent_bg_color;
    color: @accent_fg_color;
    border-radius: 999px;
    padding: 0 7px;
    min-width: 8px;
    font-size: smaller;
    font-weight: bold;
}
.va-count.va-paused {
    background-color: @warning_bg_color;
    color: @warning_fg_color;
}
.va-stopped {
    color: @warning_color;
    font-weight: bold;
}
.va-tile {
    padding: 8px 12px;
    border-radius: 10px;
    background-color: @card_bg_color;
    color: @card_fg_color;
    box-shadow: 0 0 0 1px alpha(@window_fg_color, 0.07), 0 1px 2px alpha(black, 0.06);
}
.va-tile:checked {
    background-color: alpha(@accent_bg_color, 0.12);
    color: @window_fg_color;
    box-shadow: inset 0 0 0 2px @accent_color;
}
.va-head-band.va-stuck {
    box-shadow: 0 1px alpha(@window_fg_color, 0.12);
}
)css";

void load_style()
{
    static bool loaded = false;
    GdkDisplay* display = gdk_display_get_default();
    if (loaded || !display) {
        return;
    }
    GtkCssProvider* provider = gtk_css_provider_new();
    gtk_css_provider_load_from_string(provider, style);
    gtk_style_context_add_provider_for_display(display, GTK_STYLE_PROVIDER(provider),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);
    loaded = true;
}

const char* page_of(GtkListBoxRow* row)
{
    return row ? static_cast<const char*>(g_object_get_data(G_OBJECT(row), "va-page")) : nullptr;
}

// The sidebar selects the row of the page shown.
void select_row(GuiState& state, const char* page)
{
    auto* sidebar = GTK_LIST_BOX(state.shell.sidebar);
    for (int index = 0;; ++index) {
        GtkListBoxRow* row = gtk_list_box_get_row_at_index(sidebar, index);
        if (!row) {
            break;
        }
        const char* name = page_of(row);
        if (page && name && std::strcmp(page, name) == 0) {
            if (gtk_list_box_get_selected_row(sidebar) != row) {
                gtk_list_box_select_row(sidebar, row);
            }
            return;
        }
    }
    gtk_list_box_unselect_all(sidebar);
}

GtkWidget* sidebar_row(GuiState& state, const Page& page)
{
    GtkWidget* row = gtk_list_box_row_new();
    g_object_set_data(G_OBJECT(row), "va-page", const_cast<char*>(page.name));
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_box_append(GTK_BOX(box), gtk_image_new_from_icon_name(page.icon));
    GtkWidget* label = gtk_label_new(state.text(page.title).c_str());
    gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand(label, TRUE);
    gtk_box_append(GTK_BOX(box), label);
    if (std::string_view(page.name) == "jobs") {
        // The jobs that run or wait, as on the header's button.
        state.shell.jobs_badge = gtk_label_new("0");
        gtk_widget_add_css_class(state.shell.jobs_badge, "va-count");
        gtk_widget_add_css_class(state.shell.jobs_badge, "numeric");
        gtk_widget_set_valign(state.shell.jobs_badge, GTK_ALIGN_CENTER);
        gtk_widget_set_visible(state.shell.jobs_badge, FALSE);
        gtk_box_append(GTK_BOX(box), state.shell.jobs_badge);
        // ... and the unfinished operations that stopped, which Jobs lists
        // (decision 0049), in the colour of warnings.
        state.shell.stopped_badge = gtk_label_new("0");
        gtk_widget_add_css_class(state.shell.stopped_badge, "va-count");
        gtk_widget_add_css_class(state.shell.stopped_badge, "va-paused");
        gtk_widget_add_css_class(state.shell.stopped_badge, "numeric");
        gtk_widget_set_valign(state.shell.stopped_badge, GTK_ALIGN_CENTER);
        gtk_widget_set_visible(state.shell.stopped_badge, FALSE);
        gtk_box_append(GTK_BOX(box), state.shell.stopped_badge);
    }
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
    return row;
}

GtkWidget* build_sidebar(GuiState& state)
{
    const auto& text = state.text;
    GtkWidget* sidebar = gtk_list_box_new();
    state.shell.sidebar = sidebar;
    gtk_widget_add_css_class(sidebar, "navigation-sidebar");
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(sidebar), GTK_SELECTION_SINGLE);
    for (const auto& page : pages) {
        gtk_list_box_append(GTK_LIST_BOX(sidebar), sidebar_row(state, page));
    }
    // A click or Enter on a row shows its page, also its own page again in
    // a narrow window, where the page takes the sidebar's place.
    g_signal_connect(sidebar, "row-activated",
        G_CALLBACK(+[](GtkListBox*, GtkListBoxRow* row, gpointer data) {
            auto& state = *static_cast<GuiState*>(data);
            guarded([&] {
                if (const char* page = page_of(row)) {
                    show_page(state, page);
                }
            },
                [](const Report&) {});
        }),
        &state);
    g_signal_connect(sidebar, "row-selected",
        G_CALLBACK(+[](GtkListBox*, GtkListBoxRow* row, gpointer data) {
            auto& state = *static_cast<GuiState*>(data);
            guarded([&] {
                const char* page = page_of(row);
                const char* shown = gtk_stack_get_visible_child_name(GTK_STACK(state.stack));
                if (page && (!shown || std::strcmp(page, shown) != 0)) {
                    gtk_stack_set_visible_child_name(GTK_STACK(state.stack), page);
                }
            },
                [](const Report&) {});
        }),
        &state);

    GtkWidget* scroller = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), sidebar);

    GtkWidget* header = adw_header_bar_new();
    state.shell.title = adw_window_title_new("VeriArchive", "");
    adw_header_bar_set_title_widget(ADW_HEADER_BAR(header), state.shell.title);
    // An icon, named to a tooltip and a screen reader: the sidebar's title
    // beside it is never cut; Archives has Register Archive in words.
    GtkWidget* add = gtk_button_new_from_icon_name("list-add-symbolic");
    state.shell.add = add;
    gtk_widget_add_css_class(add, "flat");
    gtk_widget_set_tooltip_text(add, text("Register Archive").c_str());
    gtk_accessible_update_property(
        GTK_ACCESSIBLE(add), GTK_ACCESSIBLE_PROPERTY_LABEL, text("Register Archive").c_str(), -1);
    g_signal_connect(add, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) {
        auto& state = *static_cast<GuiState*>(data);
        guarded([&] { open_register_wizard(state); }, [&](const Report& report) { show_message(state, report); });
    }), &state);
    adw_header_bar_pack_start(ADW_HEADER_BAR(header), add);

    GtkWidget* view = adw_toolbar_view_new();
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(view), header);
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(view), scroller);
    return GTK_WIDGET(adw_navigation_page_new(view, "VeriArchive"));
}

// Whichever way the page shown changed (the sidebar, a button, the code that
// brings a view into view): the sidebar selects its row and the header names
// it. A narrow window shows it in place of the sidebar only through
// show_page: the arrow keys select the sidebar's rows one after another, and
// each shows its page behind the sidebar, which stays, so the next row can
// be reached; a click or Enter (row-activated) shows the page.
void page_changed(GuiState& state)
{
    const char* shown = gtk_stack_get_visible_child_name(GTK_STACK(state.stack));
    select_row(state, shown);
    const auto* page = shown ? page_named(shown) : nullptr;
    adw_navigation_page_set_title(
        ADW_NAVIGATION_PAGE(state.shell.content), page ? state.text(page->title).c_str() : "VeriArchive");
}

// Closing the window while jobs wait or work asks first (decision 0045):
// closing stops them, each resumable. Confirmed, the window closes and the
// application's shutdown stops them and waits for each to record where it
// stopped. Without jobs, the window closes.
gboolean ask_before_closing(GtkWindow* closing, gpointer data)
{
    auto& state = *static_cast<GuiState*>(data);
    const auto active = state.jobs.active_count();
    if (active == 0) {
        return FALSE;
    }
    if (state.shell.asking_to_close) {
        return TRUE;  // asked already
    }
    const auto& text = state.text;
    const auto question =
        active == 1 ? text("1 job is still running.") : text.format("{} jobs are still running.", active);
    // The window asked about, kept until the answer: the question goes with it.
    std::shared_ptr<GtkWindow> window(GTK_WINDOW(g_object_ref(closing)), [](GtkWindow* held) { g_object_unref(held); });
    state.shell.asking_to_close = true;
    ask(closing, question,
        text("Closing the window stops them. Each can be resumed later, without reading again what it read."),
        {{"keep", text("Keep the window open")}, {"stop", text("Stop them and close"), true}},
        [&state, window](const std::string& answer) {
            state.shell.asking_to_close = false;
            if (answer == "stop") {
                gtk_window_destroy(window.get());
            }
        });
    return TRUE;
}

} // namespace

void build_window(GuiState& state)
{
    load_style();
    const auto& text = state.text;
    if (state.shell.toast) {
        g_object_remove_weak_pointer(G_OBJECT(state.shell.toast), reinterpret_cast<gpointer*>(&state.shell.toast));
    }
    state.shell = {};
    // Made first, so that the pages can say what changes in a narrow window
    // as they are built (set_while_narrow).
    state.shell.narrow = adw_breakpoint_new(adw_breakpoint_condition_parse("max-width: 860sp"));
    GtkWidget* window = adw_application_window_new(state.application);
    state.window = GTK_WINDOW(window);
    gtk_window_set_title(state.window, "VeriArchive");
    gtk_window_set_default_size(state.window, 1100, 720);
    // The narrowest the window goes; below the breakpoint the sidebar and
    // the page take turns.
    gtk_widget_set_size_request(window, 360, 480);

    GtkWidget* stack = gtk_stack_new();
    state.stack = stack;
    gtk_widget_set_hexpand(stack, TRUE);
    gtk_widget_set_vexpand(stack, TRUE);
    const auto add_page = [&](GtkWidget* child, const char* name) {
        gtk_stack_add_titled(GTK_STACK(stack), child, name, text(page_named(name)->title).c_str());
    };
    add_page(build_archives_page(state), "archives");
    add_page(build_devices_page(state), "devices");
    add_page(build_jobs_page(state), "jobs");
    add_page(build_history_page(state), "history");
    add_page(build_settings_page(state), "settings");

    GtkWidget* sidebar = build_sidebar(state);

    GtkWidget* header = adw_header_bar_new();
    adw_header_bar_pack_end(ADW_HEADER_BAR(header), build_jobs_button(state));
    GtkWidget* view = adw_toolbar_view_new();
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(view), header);
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(view), stack);
    state.shell.content = GTK_WIDGET(adw_navigation_page_new(view, text("Archives").c_str()));

    state.shell.split = adw_navigation_split_view_new();
    auto* split = ADW_NAVIGATION_SPLIT_VIEW(state.shell.split);
    adw_navigation_split_view_set_min_sidebar_width(split, 180);
    adw_navigation_split_view_set_max_sidebar_width(split, 240);
    adw_navigation_split_view_set_sidebar(split, ADW_NAVIGATION_PAGE(sidebar));
    adw_navigation_split_view_set_content(split, ADW_NAVIGATION_PAGE(state.shell.content));

    state.shell.toasts = adw_toast_overlay_new();
    adw_toast_overlay_set_child(ADW_TOAST_OVERLAY(state.shell.toasts), state.shell.split);
    adw_application_window_set_content(ADW_APPLICATION_WINDOW(window), state.shell.toasts);

    // A narrow window shows the sidebar or the page, with a button back to
    // the sidebar above the page; the jobs button shows its ring and count
    // without its word, and its popover fits in the window.
    AdwBreakpoint* narrow = state.shell.narrow;
    set_while_narrow(state, state.shell.split, "collapsed", true);
    set_while_narrow(state, state.jobs_page.word, "visible", false);
    GValue width = G_VALUE_INIT;
    g_value_init(&width, G_TYPE_INT);
    g_value_set_int(&width, jobs_popover_narrow_width);
    adw_breakpoint_add_setter(narrow, G_OBJECT(state.jobs_page.popover_box), "width-request", &width);
    g_value_unset(&width);
    adw_application_window_add_breakpoint(ADW_APPLICATION_WINDOW(window), narrow);

    select_row(state, gtk_stack_get_visible_child_name(GTK_STACK(stack)));
    g_signal_connect(stack, "notify::visible-child",
        G_CALLBACK(+[](GObject*, GParamSpec*, gpointer data) {
            auto& state = *static_cast<GuiState*>(data);
            guarded([&] { page_changed(state); }, [](const Report&) {});
        }),
        &state);
    // Switching back to Archives shows the latest results, Devices the
    // archives' labels as Archives read them last (its devices are read as it
    // comes on screen), and History the latest runs.
    g_signal_connect(stack, "notify::visible-child",
        G_CALLBACK(+[](GObject* source, GParamSpec*, gpointer data) {
            auto& state = *static_cast<GuiState*>(data);
            guarded(
                [&] {
                    const char* name = gtk_stack_get_visible_child_name(GTK_STACK(source));
                    if (name && std::string_view(name) == "archives") {
                        refresh_archives(state);
                    }
                },
                [&](const Report& report) { show_archives_status(state, report); });
            guarded(
                [&] {
                    const char* name = gtk_stack_get_visible_child_name(GTK_STACK(source));
                    if (name && std::string_view(name) == "devices") {
                        refresh_devices(state);
                    }
                },
                [&](const Report& report) { show_message(state, report); });
            guarded(
                [&] {
                    const char* name = gtk_stack_get_visible_child_name(GTK_STACK(source));
                    if (name && std::string_view(name) == "history") {
                        refresh_history(state);
                    }
                },
                // Said in the page, never the last read shown as if it were this one.
                [&](const Report& report) { guarded([&] { show_history_error(state, report); }, [](const Report&) {}); });
        }),
        &state);
    // Switching to Settings reads again which copies of the database kept
    // before an upgrade are in RepositoryRoot (decision 0032): one removed
    // since is no longer named.
    g_signal_connect(stack, "notify::visible-child",
        G_CALLBACK(+[](GObject* source, GParamSpec*, gpointer data) {
            auto& state = *static_cast<GuiState*>(data);
            guarded(
                [&] {
                    const char* name = gtk_stack_get_visible_child_name(GTK_STACK(source));
                    if (name && std::string_view(name) == "settings") {
                        refresh_settings(state);
                    }
                },
                [&](const Report& report) {
                    state.settings_page.notice = report;
                    guarded([&] { refresh_settings(state); }, [](const Report&) {});
                });
        }),
        &state);

    g_signal_connect(window, "close-request", G_CALLBACK(ask_before_closing), &state);
}

void show_page(GuiState& state, const char* page)
{
    if (!state.stack) {
        return;
    }
    // The page itself, not a result page opened over it.
    const std::string_view name = page ? page : "";
    GtkWidget* navigation = name == "archives"  ? state.archives_page.navigation
                            : name == "history" ? state.history_page.navigation
                                                : nullptr;
    GtkWidget* root = name == "archives" ? state.archives_page.root : state.history_page.root;
    if (navigation && root &&
        adw_navigation_view_get_visible_page(ADW_NAVIGATION_VIEW(navigation)) != ADW_NAVIGATION_PAGE(root)) {
        adw_navigation_view_pop_to_page(ADW_NAVIGATION_VIEW(navigation), ADW_NAVIGATION_PAGE(root));
    }
    gtk_stack_set_visible_child_name(GTK_STACK(state.stack), page);
    // The page shown already: a narrow window shows it all the same.
    if (state.shell.split) {
        adw_navigation_split_view_set_show_content(ADW_NAVIGATION_SPLIT_VIEW(state.shell.split), TRUE);
    }
}

void set_while_narrow(GuiState& state, GtkWidget* object, const char* property, bool value)
{
    if (!state.shell.narrow || !object) {
        return;
    }
    GValue shown = G_VALUE_INIT;
    g_value_init(&shown, G_TYPE_BOOLEAN);
    g_value_set_boolean(&shown, value ? TRUE : FALSE);
    adw_breakpoint_add_setter(state.shell.narrow, G_OBJECT(object), property, &shown);
    g_value_unset(&shown);
}

void refresh_shell(GuiState& state)
{
    if (!state.shell.title) {
        return;
    }
    const auto folder = state.session ? repository_folder(state.session->context.repo.root) : std::string{};
    adw_window_title_set_subtitle(ADW_WINDOW_TITLE(state.shell.title), folder.c_str());
}

namespace {

// Forgets the toast that `kept` points at, as a weak pointer, and dismisses
// it if it is still there.
void forget_toast(AdwToast*& kept, bool dismiss)
{
    if (AdwToast* toast = std::exchange(kept, nullptr)) {
        g_object_remove_weak_pointer(G_OBJECT(toast), reinterpret_cast<gpointer*>(&kept));
        if (dismiss) {
            adw_toast_dismiss(toast);
        }
    }
}

void keep_toast(AdwToast*& kept, AdwToast* toast)
{
    kept = toast;
    g_object_add_weak_pointer(G_OBJECT(toast), reinterpret_cast<gpointer*>(&kept));
}

} // namespace

void show_toast(GuiState& state, const std::string& text, std::optional<ToastAction> action, ToastKind kind)
{
    if (!state.shell.toasts) {
        return;
    }
    AdwToast* toast = adw_toast_new(text.c_str());
    adw_toast_set_use_markup(toast, FALSE);
    if (action) {
        struct Pressed {
            GuiState* state;
            std::function<void(GuiState&)> run;
        };
        adw_toast_set_button_label(toast, action->label.c_str());
        g_signal_connect_data(toast, "button-clicked",
            G_CALLBACK(+[](AdwToast*, gpointer data) {
                auto* pressed = static_cast<Pressed*>(data);
                guarded(
                    [&] {
                        if (pressed->state->window && pressed->run) {
                            pressed->run(*pressed->state);
                        }
                    },
                    [](const Report&) {});
            }),
            new Pressed{&state, std::move(action->run)},
            +[](gpointer data, GClosure*) { delete static_cast<Pressed*>(data); }, GConnectFlags{});
    }
    // That a job started gives way to whatever comes next: the end of a job
    // says more than that it started. Any other toast waits behind the new
    // one, which shows at once, and shows again after it.
    forget_toast(state.shell.started, true);
    adw_toast_set_priority(toast, ADW_TOAST_PRIORITY_HIGH);
    // Kept until it goes, for dismiss_toast; the overlay holds it.
    forget_toast(state.shell.toast, false);
    keep_toast(state.shell.toast, toast);
    if (kind == ToastKind::Started) {
        keep_toast(state.shell.started, toast);
    }
    adw_toast_overlay_add_toast(ADW_TOAST_OVERLAY(state.shell.toasts), toast);
}

void dismiss_toast(GuiState& state)
{
    forget_toast(state.shell.started, true);
    if (state.shell.toast) {
        adw_toast_dismiss(state.shell.toast);
    }
}

} // namespace va::gui
