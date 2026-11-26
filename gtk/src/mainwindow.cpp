// SPDX-License-Identifier: LGPL-2.1-or-later
#include "mainwindow.hpp"

#include "dialogs.hpp"
#include "engine.hpp"
#include "gtkutil.hpp"
#include "settings.hpp"
#include "util.hpp"

namespace szl {

namespace {

// A menu item that shows its keyboard shortcut (also shortcuts that only work in the file list).
void item(GMenu *m, const char *label, const char *action, const char *accel = nullptr)
{
    GMenuItem *i = g_menu_item_new(label, action);
    if (accel) g_menu_item_set_attribute(i, "accel", "s", accel);
    g_menu_append_item(m, i);
    g_object_unref(i);
}

}  // namespace

// ---------- construction ----------

MainWindow::MainWindow(GtkApplication *app)
{
    win_ = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(win_), "7zipl");
    gtk_window_set_icon_name(GTK_WINDOW(win_), SZL_APP_ID);
    gtk_window_set_default_size(GTK_WINDOW(win_), settings().width, settings().height);
    g_object_set_data(G_OBJECT(win_), "szl-main", this);

    for (int i = 0; i < 2; i++) build_side(i);
    build_actions();

    GtkWidget *box = vbox(0);
    GMenuModel *menu = build_menu();
    GtkWidget *menubar = gtk_popover_menu_bar_new_from_model(menu);
    g_object_unref(menu);
    gtk_box_append(GTK_BOX(box), menubar);
    toolbar_ = build_toolbar();
    gtk_box_append(GTK_BOX(box), toolbar_);

    paned_ = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_paned_set_start_child(GTK_PANED(paned_), panels_[0].box);
    gtk_paned_set_resize_start_child(GTK_PANED(paned_), TRUE);
    gtk_paned_set_shrink_start_child(GTK_PANED(paned_), FALSE);
    gtk_paned_set_end_child(GTK_PANED(paned_), panels_[1].box);
    gtk_paned_set_resize_end_child(GTK_PANED(paned_), TRUE);
    gtk_paned_set_shrink_end_child(GTK_PANED(paned_), FALSE);
    gtk_widget_set_vexpand(paned_, TRUE);
    gtk_box_append(GTK_BOX(box), paned_);

    status_bar_ = hbox(6);
    gtk_widget_add_css_class(status_bar_, "szl-statusbar");
    status_ = label("");
    gtk_label_set_ellipsize(GTK_LABEL(status_), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand(status_, TRUE);
    gtk_box_append(GTK_BOX(status_bar_), status_);
    gtk_box_append(GTK_BOX(box), status_bar_);
    gtk_window_set_child(GTK_WINDOW(win_), box);

    g_signal_connect_swapped(win_, "close-request", CB([](MainWindow *w) -> gboolean {
                                 Settings &s = settings();
                                 gtk_window_get_default_size(w->window(), &s.width, &s.height);
                                 s.two_panels = w->two_;
                                 if (!w->panels_[0].panel->in_archive() && !w->panels_[0].panel->fs_dir().empty())
                                     s.last_folder = w->panels_[0].panel->fs_dir();
                                 if (!w->panels_[1].panel->in_archive() && !w->panels_[1].panel->fs_dir().empty())
                                     s.last_folder2 = w->panels_[1].panel->fs_dir();
                                 s.save();
                                 return FALSE;
                             }),
                             this);
    set_two_panels(settings().two_panels);
    set_active(0);
    apply_view_settings();
}

MainWindow::~MainWindow()
{
    for (GFileMonitor *m : edit_monitors_) {
        g_file_monitor_cancel(m);
        g_object_unref(m);
    }
}

void MainWindow::build_side(int i)
{
    Side &s = panels_[i];
    s.panel = std::make_unique<Panel>();
    Panel *p = s.panel.get();
    s.box = vbox(0);
    gtk_widget_add_css_class(s.box, "szl-panel");
    GtkWidget *addr = hbox(4);
    gtk_widget_add_css_class(addr, "szl-address");
    s.up = gtk_button_new_from_icon_name("go-up-symbolic");
    gtk_widget_set_tooltip_text(s.up, "Up One Level (Backspace)");
    gtk_widget_add_css_class(s.up, "flat");
    g_object_set_data(G_OBJECT(s.up), "side", GINT_TO_POINTER(i));
    g_signal_connect(s.up, "clicked", CB([](GtkWidget *b, MainWindow *w) {
                         const int i = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "side"));
                         w->set_active(i);
                         w->panels_[i].panel->go_up();
                         w->panels_[i].panel->focus();
                     }),
                     this);
    gtk_box_append(GTK_BOX(addr), s.up);
    s.address = gtk_entry_new();
    gtk_widget_set_hexpand(s.address, TRUE);
    g_object_set_data(G_OBJECT(s.address), "side", GINT_TO_POINTER(i));
    g_signal_connect(s.address, "activate", CB([](GtkEntry *e, MainWindow *w) {
                         const int i = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(e), "side"));
                         w->set_active(i);
                         w->panels_[i].panel->navigate_to(w->window(), gtk_editable_get_text(GTK_EDITABLE(e)));
                         w->panels_[i].panel->focus();
                     }),
                     this);
    GtkEventController *fc = gtk_event_controller_focus_new();
    g_object_set_data(G_OBJECT(fc), "side", GINT_TO_POINTER(i));
    g_signal_connect(fc, "enter", CB([](GtkEventController *c, MainWindow *w) {
                         w->set_active(GPOINTER_TO_INT(g_object_get_data(G_OBJECT(c), "side")));
                     }),
                     this);
    gtk_widget_add_controller(s.address, fc);
    gtk_box_append(GTK_BOX(addr), s.address);
    gtk_box_append(GTK_BOX(s.box), addr);
    gtk_box_append(GTK_BOX(s.box), p->widget());

    p->on_location_changed = [this, i] {
        update_address(i);
        // Folders History (Alt+F12)
        std::string where = panels_[i].panel->address();
        while (where.size() > 1 && where.back() == '/') where.pop_back();
        settings().push_history(settings().folder_history, where);
    };
    p->on_selection_changed = [this, i] {
        if (i == active_) update_status();
    };
    p->on_focus = [this, i] { set_active(i); };
    p->on_open_file = [this](const Row &r) { launch_file(window(), r.full); };
    p->on_drop_files = [this, i](const std::vector<std::string> &files) {
        set_active(i);
        if (panels_[i].panel->in_archive())
            add_files_to_archive(*panels_[i].panel, files);
        else
            copy_files_to(files, panels_[i].panel->fs_dir(), false);
    };
}

void MainWindow::set_active(int i)
{
    if (i != 0 && !two_) i = 0;
    active_ = i;
    for (int k = 0; k < 2; k++) {
        panels_[k].panel->set_active(k == i || !two_);
        if (k == i && two_)
            gtk_widget_add_css_class(panels_[k].box, "active-panel");
        else
            gtk_widget_remove_css_class(panels_[k].box, "active-panel");
    }
    update_address(i);
}

void MainWindow::set_two_panels(bool on)
{
    two_ = on;
    gtk_widget_set_visible(panels_[1].box, on);
    if (on && !panels_[1].panel->in_archive() && panels_[1].panel->fs_dir().empty()) {
        const std::string d = settings().last_folder2;
        panels_[1].panel->navigate_fs(!d.empty() && g_file_test(d.c_str(), G_FILE_TEST_IS_DIR) ? d : panel().fs_dir());
    }
    if (on) {
        const int w = gtk_widget_get_width(paned_);
        gtk_paned_set_position(GTK_PANED(paned_), w > 0 ? w / 2 : settings().width / 2);
    }
    if (!on) set_active(0);
    GAction *a = g_action_map_lookup_action(G_ACTION_MAP(win_), "two-panels");
    if (a) g_simple_action_set_state(G_SIMPLE_ACTION(a), g_variant_new_boolean(on));
    settings().two_panels = on;
}

void MainWindow::switch_panel()
{
    if (!two_) return;
    set_active(1 - active_);
    panel().focus();
}

void MainWindow::present()
{
    gtk_window_present(GTK_WINDOW(win_));
    panel().focus();
}

void MainWindow::open_path(const std::string &path)
{
    if (path.empty()) {
        const std::string last = settings().last_folder;
        panels_[0].panel->navigate_fs(!last.empty() && g_file_test(last.c_str(), G_FILE_TEST_IS_DIR) ? last : home_dir());
    } else {
        panels_[0].panel->navigate_to(window(), path);
        if (panels_[0].panel->fs_dir().empty()) panels_[0].panel->navigate_fs(home_dir());
    }
}

void MainWindow::update_address(int i)
{
    Panel &p = *panels_[i].panel;
    GtkWidget *focus = gtk_root_get_focus(GTK_ROOT(win_));
    if (!focus || (focus != panels_[i].address && !gtk_widget_is_ancestor(focus, panels_[i].address)))
        gtk_editable_set_text(GTK_EDITABLE(panels_[i].address), p.address().c_str());
    if (i != active_) return;
    std::string title = p.in_archive() ? p.arc().virtual_path : p.fs_dir();
    if (p.in_archive() && !p.prefix().empty()) title += "/" + p.prefix();
    title = path_basename(title) + " - 7zipl";
    gtk_window_set_title(GTK_WINDOW(win_), title.c_str());
    const bool upd = !p.in_archive() || p.arc().can_update;
    for (const char *a : {"delete", "rename", "new-folder", "move-to"}) {
        GAction *ac = g_action_map_lookup_action(G_ACTION_MAP(win_), a);
        if (ac) g_simple_action_set_enabled(G_SIMPLE_ACTION(ac), upd);
    }
    for (const char *a : {"new-file", "link", "split", "combine", "delete-permanently"}) {
        GAction *ac = g_action_map_lookup_action(G_ACTION_MAP(win_), a);
        if (ac) g_simple_action_set_enabled(G_SIMPLE_ACTION(ac), !p.in_archive());
    }
    GAction *fl = g_action_map_lookup_action(G_ACTION_MAP(win_), "flat-view");
    if (fl) g_simple_action_set_state(G_SIMPLE_ACTION(fl), g_variant_new_boolean(p.flat()));
    update_status();
}

void MainWindow::update_status() { gtk_label_set_text(GTK_LABEL(status_), panel().status_text().c_str()); }

void MainWindow::apply_view_settings()
{
    const Settings &s = settings();
    gtk_widget_set_visible(toolbar_, s.show_toolbar);
    gtk_widget_set_visible(status_bar_, s.show_statusbar);
    for (GtkWidget *c = gtk_widget_get_first_child(toolbar_); c; c = gtk_widget_get_next_sibling(c)) {
        GtkWidget *l = GTK_WIDGET(g_object_get_data(G_OBJECT(c), "tool-label"));
        if (l) gtk_widget_set_visible(l, s.toolbar_text);
        GtkWidget *img = GTK_WIDGET(g_object_get_data(G_OBJECT(c), "tool-icon"));
        if (img) gtk_image_set_icon_size(GTK_IMAGE(img), s.large_buttons ? GTK_ICON_SIZE_LARGE : GTK_ICON_SIZE_NORMAL);
    }
    set_memory_limit(s.mem_limit ? (uint64_t)s.mem_limit_gb << 30 : 0);
    for (auto &side : panels_) {
        side.panel->set_show_hidden(s.show_hidden);
        side.panel->apply_view_settings();
    }
}

void MainWindow::after_change()
{
    panel().refresh(window());
    if (two_) other_panel().refresh(window());
}

// ---------- actions, menu and toolbar ----------

void MainWindow::build_actions()
{
    struct A {
        const char *name;
        void (*fn)(MainWindow *);
    };
    static const A acts[] = {
        {"open", [](MainWindow *w) { w->cmd_open(0); }},
        {"open-inside", [](MainWindow *w) { w->cmd_open(1); }},
        {"open-inside-all", [](MainWindow *w) { w->cmd_open_inside_type("*"); }},
        {"open-inside-parser", [](MainWindow *w) { w->cmd_open_inside_type("#"); }},
        {"open-outside", [](MainWindow *w) { w->cmd_open(2); }},
        {"view", [](MainWindow *w) { w->cmd_view(false); }},
        {"edit", [](MainWindow *w) { w->cmd_view(true); }},
        {"rename", [](MainWindow *w) { w->cmd_rename(); }},
        {"copy-to", [](MainWindow *w) { w->cmd_copy_move(false); }},
        {"move-to", [](MainWindow *w) { w->cmd_copy_move(true); }},
        {"delete", [](MainWindow *w) { w->cmd_delete(false); }},
        {"delete-permanently", [](MainWindow *w) { w->cmd_delete(true); }},
        {"split", [](MainWindow *w) { w->cmd_split(); }},
        {"combine", [](MainWindow *w) { w->cmd_combine(); }},
        {"properties", [](MainWindow *w) { w->cmd_properties(); }},
        {"diff", [](MainWindow *w) { w->cmd_diff(); }},
        {"new-folder", [](MainWindow *w) { w->cmd_new_folder(); }},
        {"new-file", [](MainWindow *w) { w->cmd_new_file(); }},
        {"link", [](MainWindow *w) { w->cmd_link(); }},
        {"add", [](MainWindow *w) { w->cmd_add(); }},
        {"extract", [](MainWindow *w) { w->cmd_extract(); }},
        {"test", [](MainWindow *w) { w->cmd_test(); }},
        {"select-all", [](MainWindow *w) { w->panel().select_all(); }},
        {"deselect-all", [](MainWindow *w) { w->panel().select_none(); }},
        {"invert-selection", [](MainWindow *w) { w->panel().invert_selection(); }},
        {"select-mask", [](MainWindow *w) { w->cmd_select_mask(true); }},
        {"deselect-mask", [](MainWindow *w) { w->cmd_select_mask(false); }},
        {"select-type", [](MainWindow *w) { w->panel().select_by_type(true); }},
        {"deselect-type", [](MainWindow *w) { w->panel().select_by_type(false); }},
        {"refresh", [](MainWindow *w) { w->panel().refresh(w->window()); }},
        {"up", [](MainWindow *w) { w->panel().go_up(); }},
        {"root", [](MainWindow *w) { w->panel().go_root(); }},
        {"folders-history", [](MainWindow *w) { w->cmd_folders_history(); }},
        {"switch-panel", [](MainWindow *w) { w->switch_panel(); }},
        {"options", [](MainWindow *w) { w->cmd_options(0); }},
        {"benchmark", [](MainWindow *w) { w->cmd_benchmark(); }},
        {"delete-temp", [](MainWindow *w) { w->cmd_delete_temp(); }},
        {"help", [](MainWindow *w) { w->cmd_help(); }},
        {"about", [](MainWindow *w) { about_dialog(w->window()); }},
        {"close", [](MainWindow *w) { gtk_window_close(w->window()); }},
        {"focus-address", [](MainWindow *w) {
             GtkWidget *a = w->panels_[w->active_].address;
             gtk_widget_grab_focus(a);
             gtk_editable_select_region(GTK_EDITABLE(a), 0, -1);
         }},
    };
    for (const A &a : acts) {
        GSimpleAction *sa = g_simple_action_new(a.name, nullptr);
        g_object_set_data(G_OBJECT(sa), "fn", (gpointer)a.fn);
        g_signal_connect(sa, "activate", CB([](GSimpleAction *sa, GVariant *, MainWindow *w) {
                             auto fn = (void (*)(MainWindow *))g_object_get_data(G_OBJECT(sa), "fn");
                             fn(w);
                         }),
                         this);
        g_action_map_add_action(G_ACTION_MAP(win_), G_ACTION(sa));
        g_object_unref(sa);
    }

    // View > Large Icons / Small Icons / List / Details
    GSimpleAction *vm = g_simple_action_new_stateful("view-mode", G_VARIANT_TYPE_INT32, g_variant_new_int32(settings().view_mode));
    g_signal_connect(vm, "change-state", CB([](GSimpleAction *a, GVariant *v, MainWindow *w) {
                         g_simple_action_set_state(a, v);
                         settings().view_mode = g_variant_get_int32(v);
                         for (auto &side : w->panels_) side.panel->set_view_mode(settings().view_mode);
                         w->panel().focus();
                     }),
                     this);
    g_action_map_add_action(G_ACTION_MAP(win_), G_ACTION(vm));
    g_object_unref(vm);

    // View > Arrange by: sort-by(column), -1 = unsorted
    GSimpleAction *sort = g_simple_action_new("sort-by", G_VARIANT_TYPE_INT32);
    g_signal_connect(sort, "activate", CB([](GSimpleAction *, GVariant *v, MainWindow *w) {
                         w->panel().sort_by(g_variant_get_int32(v));
                     }),
                     this);
    g_action_map_add_action(G_ACTION_MAP(win_), G_ACTION(sort));
    g_object_unref(sort);

    GSimpleAction *flat = g_simple_action_new_stateful("flat-view", nullptr, g_variant_new_boolean(FALSE));
    g_signal_connect(flat, "activate", CB([](GSimpleAction *a, GVariant *, MainWindow *w) {
                         const bool v = !w->panel().flat();
                         w->panel().set_flat(v);
                         g_simple_action_set_state(a, g_variant_new_boolean(v));
                     }),
                     this);
    g_action_map_add_action(G_ACTION_MAP(win_), G_ACTION(flat));
    g_object_unref(flat);

    GSimpleAction *two = g_simple_action_new_stateful("two-panels", nullptr, g_variant_new_boolean(FALSE));
    g_signal_connect(two, "activate", CB([](GSimpleAction *, GVariant *, MainWindow *w) {
                         w->set_two_panels(!w->two_);
                         w->panel().focus();
                     }),
                     this);
    g_action_map_add_action(G_ACTION_MAP(win_), G_ACTION(two));
    g_object_unref(two);

    GSimpleAction *opage = g_simple_action_new("options-page", G_VARIANT_TYPE_INT32);
    g_signal_connect(opage, "activate", CB([](GSimpleAction *, GVariant *v, MainWindow *w) { w->cmd_options(g_variant_get_int32(v)); }), this);
    g_action_map_add_action(G_ACTION_MAP(win_), G_ACTION(opage));
    g_object_unref(opage);

    GSimpleAction *hash = g_simple_action_new("hash", G_VARIANT_TYPE_STRING);
    g_signal_connect(hash, "activate", CB([](GSimpleAction *, GVariant *v, MainWindow *w) {
                         w->cmd_hash(g_variant_get_string(v, nullptr));
                     }),
                     this);
    g_action_map_add_action(G_ACTION_MAP(win_), G_ACTION(hash));
    g_object_unref(hash);

    for (const char *n : {"add-favorite", "goto-favorite"}) {
        GSimpleAction *fa = g_simple_action_new(n, G_VARIANT_TYPE_INT32);
        const bool add = n[0] == 'a';
        g_signal_connect(fa, "activate",
                         add ? CB([](GSimpleAction *, GVariant *v, MainWindow *w) { w->cmd_add_favorite(g_variant_get_int32(v)); })
                             : CB([](GSimpleAction *, GVariant *v, MainWindow *w) { w->cmd_goto_favorite(g_variant_get_int32(v)); }),
                         this);
        g_action_map_add_action(G_ACTION_MAP(win_), G_ACTION(fa));
        g_object_unref(fa);
    }

    // View toggles that are settings
    struct T {
        const char *name;
        bool Settings::*field;
    };
    static const T toggles[] = {
        {"show-toolbar", &Settings::show_toolbar}, {"toolbar-text", &Settings::toolbar_text},
        {"large-buttons", &Settings::large_buttons}, {"show-statusbar", &Settings::show_statusbar},
        {"show-hidden", &Settings::show_hidden}, {"auto-refresh", &Settings::auto_refresh},
    };
    for (const T &t : toggles) {
        GSimpleAction *sa = g_simple_action_new_stateful(t.name, nullptr, g_variant_new_boolean(settings().*(t.field)));
        g_object_set_data(G_OBJECT(sa), "field", (gpointer)&t);
        g_signal_connect(sa, "activate", CB([](GSimpleAction *sa, GVariant *, MainWindow *w) {
                             const T *t = (const T *)g_object_get_data(G_OBJECT(sa), "field");
                             bool &v = settings().*(t->field);
                             v = !v;
                             g_simple_action_set_state(sa, g_variant_new_boolean(v));
                             settings().save();
                             w->apply_view_settings();
                             if (t->field == &Settings::show_hidden || t->field == &Settings::auto_refresh) w->after_change();
                         }),
                         this);
        g_action_map_add_action(G_ACTION_MAP(win_), G_ACTION(sa));
        g_object_unref(sa);
    }

    GtkApplication *app = gtk_window_get_application(GTK_WINDOW(win_));
    // Shortcuts that also work while typing in an address bar.
    struct K {
        const char *action;
        std::vector<const char *> keys;
    };
    const K keys[] = {
        {"win.properties", {"<Alt>Return"}},
        {"win.new-file", {"<Control>n"}},
        {"win.refresh", {"<Control>r"}},
        {"win.root", {"<Control>backslash"}},
        {"win.show-hidden", {"<Control>h"}},
        {"win.focus-address", {"<Control>l", "<Alt>d"}},
        {"win.two-panels", {"F9"}},
        {"win.folders-history", {"<Alt>F12"}},
        {"win.help", {"F1"}},
        {"win.close", {"<Alt>F4", "<Control>q", "<Control>w"}},
        {"win.view-mode(0)", {"<Control>1"}},
        {"win.view-mode(1)", {"<Control>2"}},
        {"win.view-mode(2)", {"<Control>3"}},
        {"win.view-mode(3)", {"<Control>4"}},
        {"win.sort-by(0)", {"<Control>F3"}},
        {"win.sort-by(16)", {"<Control>F4"}},
        {"win.sort-by(3)", {"<Control>F5"}},
        {"win.sort-by(1)", {"<Control>F6"}},
        {"win.sort-by(-1)", {"<Control>F7"}},
    };
    for (const K &k : keys) {
        std::vector<const char *> v = k.keys;
        v.push_back(nullptr);
        gtk_application_set_accels_for_action(app, k.action, v.data());
    }
    for (int i = 0; i < 10; i++) {
        const std::string go = "win.goto-favorite(" + std::to_string(i) + ")";
        const std::string add = "win.add-favorite(" + std::to_string(i) + ")";
        const std::string k1 = "<Alt>" + std::to_string(i), k2 = "<Alt><Shift>" + std::to_string(i);
        const char *v1[] = {k1.c_str(), nullptr};
        const char *v2[] = {k2.c_str(), nullptr};
        gtk_application_set_accels_for_action(app, go.c_str(), v1);
        gtk_application_set_accels_for_action(app, add.c_str(), v2);
    }
    // Single keys work in the file lists only, so typing in an address bar is safe.
    static const char *const list_keys[][2] = {
        {"<Shift>Return", "win.open-outside"}, {"<Control>Page_Down", "win.open-inside"},
        {"F3", "win.view"}, {"F4", "win.edit"},
        {"F2", "win.rename"}, {"F5", "win.copy-to"}, {"F6", "win.move-to"}, {"F7", "win.new-folder"},
        {"Delete", "win.delete"}, {"KP_Delete", "win.delete"}, {"<Shift>Delete", "win.delete-permanently"},
        {"<Control>a", "win.select-all"}, {"<Shift>KP_Add", "win.select-all"},
        {"<Shift>KP_Subtract", "win.deselect-all"}, {"KP_Multiply", "win.invert-selection"},
        {"KP_Add", "win.select-mask"}, {"KP_Subtract", "win.deselect-mask"},
        {"<Alt>KP_Add", "win.select-type"}, {"<Alt>KP_Subtract", "win.deselect-type"},
        {"Tab", "win.switch-panel"}, {"backslash", "win.root"},
    };
    for (auto &side : panels_)
        for (GtkWidget *v : side.panel->views()) {
            GtkEventController *sc = gtk_shortcut_controller_new();
            for (const auto &k : list_keys)
                gtk_shortcut_controller_add_shortcut(GTK_SHORTCUT_CONTROLLER(sc),
                                                     gtk_shortcut_new(gtk_shortcut_trigger_parse_string(k[0]),
                                                                      gtk_named_action_new(k[1])));
            gtk_widget_add_controller(v, sc);
        }
}

GMenuModel *MainWindow::build_menu()
{
    GMenu *bar = g_menu_new();
    std::vector<GMenu *> owned;
    auto menu = [&]() {
        GMenu *m = g_menu_new();
        owned.push_back(m);
        return m;
    };

    GMenu *file = menu();
    GMenu *s1 = menu();
    item(s1, "_Open", "win.open", "Return");
    item(s1, "Open _Inside", "win.open-inside", "<Control>Page_Down");
    item(s1, "Open Inside *", "win.open-inside-all");
    item(s1, "Open Inside #", "win.open-inside-parser");
    item(s1, "Open O_utside", "win.open-outside", "<Shift>Return");
    item(s1, "_View", "win.view", "F3");
    item(s1, "_Edit", "win.edit", "F4");
    g_menu_append_section(file, nullptr, G_MENU_MODEL(s1));
    GMenu *s2 = menu();
    item(s2, "Re_name", "win.rename", "F2");
    item(s2, "_Copy To...", "win.copy-to", "F5");
    item(s2, "_Move To...", "win.move-to", "F6");
    item(s2, "_Delete", "win.delete", "Delete");
    g_menu_append_section(file, nullptr, G_MENU_MODEL(s2));
    GMenu *s3 = menu();
    item(s3, "_Split file...", "win.split");
    item(s3, "Com_bine files...", "win.combine");
    g_menu_append_section(file, nullptr, G_MENU_MODEL(s3));
    GMenu *s4 = menu();
    item(s4, "P_roperties", "win.properties", "<Alt>Return");
    GMenu *crc = menu();
    for (const auto &m : hash_methods()) {
        if (m == "XXH64a" || m == "CKSUM") continue;
        const std::string a = "win.hash('" + m + "')";
        g_menu_append(crc, m.c_str(), a.c_str());
    }
    g_menu_append(crc, "*", "win.hash('*')");
    g_menu_append_submenu(s4, "CRC", G_MENU_MODEL(crc));
    item(s4, "Di_ff", "win.diff");
    g_menu_append_section(file, nullptr, G_MENU_MODEL(s4));
    GMenu *s5 = menu();
    item(s5, "Create Folder", "win.new-folder", "F7");
    item(s5, "Create File", "win.new-file", "<Control>n");
    g_menu_append_section(file, nullptr, G_MENU_MODEL(s5));
    GMenu *s6 = menu();
    item(s6, "_Link...", "win.link");
    g_menu_append_section(file, nullptr, G_MENU_MODEL(s6));
    GMenu *s7 = menu();
    item(s7, "E_xit", "win.close", "<Alt>F4");
    g_menu_append_section(file, nullptr, G_MENU_MODEL(s7));
    g_menu_append_submenu(bar, "_File", G_MENU_MODEL(file));

    GMenu *edit = menu();
    GMenu *e1 = menu();
    item(e1, "Select _All", "win.select-all", "<Control>a");
    item(e1, "Deselect All", "win.deselect-all", "<Shift>KP_Subtract");
    item(e1, "_Invert Selection", "win.invert-selection", "KP_Multiply");
    item(e1, "Select...", "win.select-mask", "KP_Add");
    item(e1, "Deselect...", "win.deselect-mask", "KP_Subtract");
    g_menu_append_section(edit, nullptr, G_MENU_MODEL(e1));
    GMenu *e2 = menu();
    item(e2, "Select by Type", "win.select-type", "<Alt>KP_Add");
    item(e2, "Deselect by Type", "win.deselect-type", "<Alt>KP_Subtract");
    g_menu_append_section(edit, nullptr, G_MENU_MODEL(e2));
    g_menu_append_submenu(bar, "_Edit", G_MENU_MODEL(edit));

    GMenu *view = menu();
    GMenu *v0 = menu();
    item(v0, "Lar_ge Icons", "win.view-mode(0)", "<Control>1");
    item(v0, "S_mall Icons", "win.view-mode(1)", "<Control>2");
    item(v0, "_List", "win.view-mode(2)", "<Control>3");
    item(v0, "_Details", "win.view-mode(3)", "<Control>4");
    g_menu_append_section(view, nullptr, G_MENU_MODEL(v0));
    GMenu *v1 = menu();
    item(v1, "Name", "win.sort-by(0)", "<Control>F3");
    item(v1, "Type", "win.sort-by(16)", "<Control>F4");
    item(v1, "Date", "win.sort-by(3)", "<Control>F5");
    item(v1, "Size", "win.sort-by(1)", "<Control>F6");
    item(v1, "Unsorted", "win.sort-by(-1)", "<Control>F7");
    g_menu_append_section(view, nullptr, G_MENU_MODEL(v1));
    GMenu *v2 = menu();
    item(v2, "Flat View", "win.flat-view");
    item(v2, "_2 Panels", "win.two-panels", "F9");
    g_menu_append_section(view, nullptr, G_MENU_MODEL(v2));
    GMenu *tb = menu();
    item(tb, "Toolbar", "win.show-toolbar");
    item(tb, "Large Buttons", "win.large-buttons");
    item(tb, "Show Buttons Text", "win.toolbar-text");
    GMenu *v3 = menu();
    g_menu_append_submenu(v3, "Toolbars", G_MENU_MODEL(tb));
    item(v3, "Status Bar", "win.show-statusbar");
    item(v3, "Hidden Files", "win.show-hidden", "<Control>h");
    g_menu_append_section(view, nullptr, G_MENU_MODEL(v3));
    GMenu *v4 = menu();
    item(v4, "Open Root Folder", "win.root", "backslash");
    item(v4, "Up One Level", "win.up", "BackSpace");
    item(v4, "Folders History...", "win.folders-history", "<Alt>F12");
    item(v4, "_Refresh", "win.refresh", "<Control>r");
    item(v4, "Auto Refresh", "win.auto-refresh");
    g_menu_append_section(view, nullptr, G_MENU_MODEL(v4));
    g_menu_append_submenu(bar, "_View", G_MENU_MODEL(view));

    favorites_menu_ = menu();
    g_object_ref(favorites_menu_);
    g_menu_append_submenu(bar, "F_avorites", G_MENU_MODEL(favorites_menu_));
    rebuild_favorites_menu();

    GMenu *tools = menu();
    GMenu *t1 = menu();
    item(t1, "_Options...", "win.options");
    g_menu_append_section(tools, nullptr, G_MENU_MODEL(t1));
    GMenu *t2 = menu();
    item(t2, "_Benchmark", "win.benchmark");
    g_menu_append_section(tools, nullptr, G_MENU_MODEL(t2));
    GMenu *t3 = menu();
    item(t3, "Delete Temporary Files...", "win.delete-temp");
    g_menu_append_section(tools, nullptr, G_MENU_MODEL(t3));
    g_menu_append_submenu(bar, "_Tools", G_MENU_MODEL(tools));

    GMenu *help = menu();
    item(help, "_Contents...", "win.help", "F1");
    item(help, "_About 7zipl...", "win.about");
    g_menu_append_submenu(bar, "_Help", G_MENU_MODEL(help));

    for (GMenu *m : owned) g_object_unref(m);
    return G_MENU_MODEL(bar);
}

void MainWindow::rebuild_favorites_menu()
{
    g_menu_remove_all(favorites_menu_);
    GMenu *add = g_menu_new();
    for (int i = 0; i < 10; i++) {
        const std::string a = "win.add-favorite(" + std::to_string(i) + ")";
        const std::string accel = "<Alt><Shift>" + std::to_string(i);
        item(add, ("Bookmark " + std::to_string(i)).c_str(), a.c_str(), accel.c_str());
    }
    g_menu_append_submenu(favorites_menu_, "_Add folder to Favorites as", G_MENU_MODEL(add));
    g_object_unref(add);
    GMenu *list = g_menu_new();
    for (int i = 0; i < 10; i++) {
        const std::string &f = settings().favorites[(size_t)i];
        if (f.empty()) continue;
        const std::string a = "win.goto-favorite(" + std::to_string(i) + ")";
        const std::string accel = "<Alt>" + std::to_string(i);
        item(list, f.c_str(), a.c_str(), accel.c_str());
    }
    g_menu_append_section(favorites_menu_, nullptr, G_MENU_MODEL(list));
    g_object_unref(list);
}

GtkWidget *MainWindow::build_toolbar()
{
    GtkWidget *tb = hbox(2);
    gtk_widget_add_css_class(tb, "szl-toolbar");
    struct B {
        const char *label, *icon, *action, *tip;
    };
    static const B buttons[] = {
        {"Add", "list-add-symbolic", "win.add", "Add files to archive"},
        {"Extract", "folder-download-symbolic", "win.extract", "Extract files from archive"},
        {"Test", "object-select-symbolic", "win.test", "Test archive"},
        {nullptr, nullptr, nullptr, nullptr},
        {"Copy", "edit-copy-symbolic", "win.copy-to", "Copy To (F5)"},
        {"Move", "go-next-symbolic", "win.move-to", "Move To (F6)"},
        {"Delete", "edit-delete-symbolic", "win.delete", "Delete (Del)"},
        {"Info", "dialog-information-symbolic", "win.properties", "Properties (Alt+Enter)"},
    };
    for (const B &b : buttons) {
        if (!b.label) {
            gtk_box_append(GTK_BOX(tb), gtk_separator_new(GTK_ORIENTATION_VERTICAL));
            continue;
        }
        GtkWidget *btn = gtk_button_new();
        gtk_widget_add_css_class(btn, "flat");
        GtkWidget *v = vbox(1);
        GtkWidget *img = gtk_image_new_from_icon_name(b.icon);
        gtk_image_set_icon_size(GTK_IMAGE(img), GTK_ICON_SIZE_LARGE);
        gtk_box_append(GTK_BOX(v), img);
        GtkWidget *l = gtk_label_new(b.label);
        gtk_widget_add_css_class(l, "tool-label");
        gtk_box_append(GTK_BOX(v), l);
        gtk_button_set_child(GTK_BUTTON(btn), v);
        gtk_widget_set_tooltip_text(btn, b.tip);
        gtk_actionable_set_action_name(GTK_ACTIONABLE(btn), b.action);
        gtk_widget_set_focus_on_click(btn, FALSE);
        g_object_set_data(G_OBJECT(btn), "tool-label", l);
        g_object_set_data(G_OBJECT(btn), "tool-icon", img);
        gtk_box_append(GTK_BOX(tb), btn);
    }
    return tb;
}

}  // namespace szl
