// The window (decision 0046): an AdwApplicationWindow whose
// AdwNavigationSplitView puts a sidebar of the pages beside the page shown,
// each under a header bar of its own: above the sidebar VeriArchive, the
// folder of the open repository and Register Archive; above the page its title
// and the jobs button. A narrow window (AdwBreakpoint) shows one of the two at
// a time. Toasts come over the whole window. Closing it while jobs run asks
// first.
#include "gui/gui_app.h"

#include "gui/window_words.h"

#include <cstring>
#include <memory>
#include <string_view>

namespace va::gui {
namespace {

// A page of the window: the name of its child in state.stack, its icon, its
// title (English, worded where shown), and whether the sidebar lists it, and
// below a line. Register Archive has no row: the button above the sidebar
// opens it.
struct Page {
    const char* name;
    const char* icon;
    const char* title;
    bool listed;
    bool separated;
};

constexpr Page pages[] = {
    {"archives", "package-x-generic-symbolic", "Archives", true, false},
    {"jobs", jobs_icon, "Jobs", true, false},
    {"history", "document-open-recent-symbolic", "History", true, false},
    {"settings", "emblem-system-symbolic", "Settings", true, false},
    // Until the later steps of decision 0046 take them into Archives.
    {"verify", "object-select-symbolic", "Verify Archive", true, true},
    {"compare", "view-dual-symbolic", "Compare Archives", true, true},
    {"register", "list-add-symbolic", "Register Archive", false, false},
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
// and the colour of how far they are on the jobs button's ring; Register
// Archive above the sidebar with less space around its words, so the title
// beside it fits in either language.
constexpr const char* style = R"css(
.va-ring {
    color: @accent_color;
}
.va-register {
    padding-left: 6px;
    padding-right: 8px;
}
.va-register > buttoncontent > box {
    border-spacing: 4px;
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

// The sidebar selects the row of the page shown; none for a page it does not
// list (Register Archive).
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
    g_object_set_data(G_OBJECT(row), "va-separated", GINT_TO_POINTER(page.separated ? 1 : 0));
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
        if (page.listed) {
            gtk_list_box_append(GTK_LIST_BOX(sidebar), sidebar_row(state, page));
        }
    }
    // A line above the first row of the pages that later steps take into
    // others.
    gtk_list_box_set_header_func(GTK_LIST_BOX(sidebar),
        +[](GtkListBoxRow* row, GtkListBoxRow* before, gpointer) {
            const auto separated = [](GtkListBoxRow* listed) {
                return g_object_get_data(G_OBJECT(listed), "va-separated") != nullptr;
            };
            if (before && separated(row) && !separated(before)) {
                if (!gtk_list_box_row_get_header(row)) {
                    gtk_list_box_row_set_header(row, gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
                }
            } else if (gtk_list_box_row_get_header(row)) {
                gtk_list_box_row_set_header(row, nullptr);
            }
        },
        nullptr, nullptr);
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
    GtkWidget* add = gtk_button_new();
    GtkWidget* face = adw_button_content_new();
    adw_button_content_set_icon_name(ADW_BUTTON_CONTENT(face), "list-add-symbolic");
    adw_button_content_set_label(ADW_BUTTON_CONTENT(face), text("Register Archive").c_str());
    gtk_button_set_child(GTK_BUTTON(add), face);
    gtk_widget_add_css_class(add, "flat");
    gtk_widget_add_css_class(add, "va-register");
    g_signal_connect(add, "clicked", G_CALLBACK(+[](GtkButton*, gpointer data) {
        auto& state = *static_cast<GuiState*>(data);
        guarded([&] { show_page(state, "register"); }, [](const Report&) {});
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

// Sets `property` of `object` to `value` while the window is narrow.
void set_while_narrow(AdwBreakpoint* narrow, GtkWidget* object, const char* property, const GValue& value)
{
    if (object) {
        adw_breakpoint_add_setter(narrow, G_OBJECT(object), property, &value);
    }
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
    state.shell = {};
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
    // The views from before decision 0046 do not scroll as a whole, only
    // their table or report does. A window with breakpoints is as small as its
    // size request, whatever its content asks for, so in a smaller one they
    // scroll rather than lose their buttons. A view taller than its least
    // still fills the page: its table or report takes the rest.
    const auto scrolling = [](GtkWidget* view) {
        GtkWidget* scroller = gtk_scrolled_window_new();
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
        gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), view);
        return scroller;
    };
    add_page(scrolling(build_archive_list_view(state)), "archives");
    add_page(build_jobs_page(state), "jobs");
    add_page(scrolling(build_register_view(state)), "register");
    add_page(scrolling(build_verify_view(state)), "verify");
    add_page(scrolling(build_compare_view(state)), "compare");
    add_page(scrolling(build_history_view(state)), "history");
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
    AdwBreakpoint* narrow = adw_breakpoint_new(adw_breakpoint_condition_parse("max-width: 860sp"));
    GValue yes = G_VALUE_INIT;
    g_value_init(&yes, G_TYPE_BOOLEAN);
    g_value_set_boolean(&yes, TRUE);
    set_while_narrow(narrow, state.shell.split, "collapsed", yes);
    g_value_unset(&yes);
    GValue no = G_VALUE_INIT;
    g_value_init(&no, G_TYPE_BOOLEAN);
    g_value_set_boolean(&no, FALSE);
    set_while_narrow(narrow, state.jobs_page.word, "visible", no);
    g_value_unset(&no);
    GValue width = G_VALUE_INIT;
    g_value_init(&width, G_TYPE_INT);
    g_value_set_int(&width, jobs_popover_narrow_width);
    set_while_narrow(narrow, state.jobs_page.popover_box, "width-request", width);
    g_value_unset(&width);
    adw_application_window_add_breakpoint(ADW_APPLICATION_WINDOW(window), narrow);

    select_row(state, gtk_stack_get_visible_child_name(GTK_STACK(stack)));
    g_signal_connect(stack, "notify::visible-child",
        G_CALLBACK(+[](GObject*, GParamSpec*, gpointer data) {
            auto& state = *static_cast<GuiState*>(data);
            guarded([&] { page_changed(state); }, [](const Report&) {});
        }),
        &state);
    // Switching back to the list shows the latest results.
    g_signal_connect(stack, "notify::visible-child",
        G_CALLBACK(+[](GObject* source, GParamSpec*, gpointer data) {
            auto& state = *static_cast<GuiState*>(data);
            guarded(
                [&] {
                    const char* name = gtk_stack_get_visible_child_name(GTK_STACK(source));
                    if (name && std::string_view(name) == "archives") {
                        refresh_archive_list(state);
                    }
                },
                [&](const Report& report) {
                    gtk_label_set_text(GTK_LABEL(state.archive_list.status), report(state.text).c_str());
                });
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
    gtk_stack_set_visible_child_name(GTK_STACK(state.stack), page);
    // The page shown already: a narrow window shows it all the same.
    if (state.shell.split) {
        adw_navigation_split_view_set_show_content(ADW_NAVIGATION_SPLIT_VIEW(state.shell.split), TRUE);
    }
}

void refresh_shell(GuiState& state)
{
    if (!state.shell.title) {
        return;
    }
    const auto folder = state.session ? repository_folder(state.session->context.repo.root) : std::string{};
    adw_window_title_set_subtitle(ADW_WINDOW_TITLE(state.shell.title), folder.c_str());
}

void show_toast(GuiState& state, const std::string& text, std::optional<ToastAction> action)
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
    adw_toast_overlay_add_toast(ADW_TOAST_OVERLAY(state.shell.toasts), toast);
}

} // namespace va::gui
