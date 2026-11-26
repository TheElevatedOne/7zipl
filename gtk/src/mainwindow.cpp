// SPDX-License-Identifier: LGPL-2.1-or-later
#include "mainwindow.hpp"

#include "compressdialog.hpp"
#include "dialogs.hpp"
#include "extractdialog.hpp"
#include "gtkutil.hpp"
#include "job.hpp"
#include "settings.hpp"
#include "util.hpp"

#include <glib/gstdio.h>

#include <set>

namespace szl {

namespace {

// ---------- copying and moving files on disk (F5/F6 in a folder) ----------

struct FsOp {
    Ui &ui;
    Result &res;
    bool move;
    int all = 0;  // 1 overwrite all, 2 skip all
    uint64_t done = 0;
};

uint64_t tree_size(const std::string &p, uint64_t &files)
{
    GStatBuf st;
    if (g_lstat(p.c_str(), &st) != 0) return 0;
    if (!S_ISDIR(st.st_mode)) {
        files++;
        return (uint64_t)st.st_size;
    }
    uint64_t s = 0;
    GDir *d = g_dir_open(p.c_str(), 0, nullptr);
    if (!d) return 0;
    const char *n;
    while ((n = g_dir_read_name(d))) s += tree_size(path_join(p, n), files);
    g_dir_close(d);
    return s;
}

std::string unique_name(const std::string &path)
{
    const std::string dir = path_dirname(path), base = path_basename(path);
    const auto dot = base.rfind('.');
    const std::string stem = dot == std::string::npos || dot == 0 ? base : base.substr(0, dot);
    const std::string ext = dot == std::string::npos || dot == 0 ? "" : base.substr(dot);
    for (int i = 1;; i++) {
        std::string p = path_join(dir, stem + "_" + std::to_string(i) + ext);
        if (!g_file_test(p.c_str(), G_FILE_TEST_EXISTS)) return p;
    }
}

bool copy_one(FsOp &op, const std::string &src, std::string dst)
{
    if (op.ui.cancelled()) return false;
    GStatBuf st;
    if (g_lstat(src.c_str(), &st) != 0) {
        op.res.errors++;
        op.res.messages.push_back("Cannot find " + src);
        return true;
    }
    if (src == dst || g_str_has_prefix(dst.c_str(), (src + "/").c_str())) {
        op.res.errors++;
        op.res.messages.push_back("Cannot copy a folder into itself: " + src);
        return true;
    }
    op.ui.set_current(op.move ? "Moving" : "Copying", src);
    const bool is_dir = S_ISDIR(st.st_mode);
    GStatBuf dst_st;
    const bool exists = g_lstat(dst.c_str(), &dst_st) == 0;
    if (exists && !(is_dir && S_ISDIR(dst_st.st_mode))) {
        int answer = op.all;
        if (!answer) {
            switch (op.ui.ask_overwrite(dst, (uint64_t)dst_st.st_size, dst_st.st_mtime, src, (uint64_t)st.st_size,
                                        st.st_mtime)) {
            case Ui::Overwrite::Yes: answer = 1; break;
            case Ui::Overwrite::YesAll: answer = op.all = 1; break;
            case Ui::Overwrite::No: answer = 2; break;
            case Ui::Overwrite::NoAll: answer = op.all = 2; break;
            case Ui::Overwrite::Rename: answer = 3; break;
            case Ui::Overwrite::Cancel: return false;
            }
        }
        if (answer == 2) return true;
        if (answer == 3) dst = unique_name(dst);
        else remove_tree(dst);
    }
    GFile *s = g_file_new_for_path(src.c_str());
    GFile *d = g_file_new_for_path(dst.c_str());
    GError *err = nullptr;
    bool ok = true;
    if (op.move && !g_file_test(dst.c_str(), G_FILE_TEST_EXISTS) &&
        g_file_move(s, d, (GFileCopyFlags)(G_FILE_COPY_NOFOLLOW_SYMLINKS | G_FILE_COPY_ALL_METADATA | G_FILE_COPY_NO_FALLBACK_FOR_MOVE),
                    nullptr, nullptr, nullptr, nullptr)) {
        uint64_t f = 0;
        op.done += 0 * tree_size(dst, f);
        op.ui.file_done();
    } else if (is_dir) {
        if (!g_file_test(dst.c_str(), G_FILE_TEST_IS_DIR) && g_mkdir(dst.c_str(), st.st_mode & 07777) != 0) {
            op.res.errors++;
            op.res.messages.push_back("Cannot create folder " + dst);
            ok = false;
        } else {
            GDir *dir = g_dir_open(src.c_str(), 0, nullptr);
            const char *n;
            std::vector<std::string> names;
            while (dir && (n = g_dir_read_name(dir))) names.emplace_back(n);
            if (dir) g_dir_close(dir);
            for (const auto &name : names)
                if (!copy_one(op, path_join(src, name), path_join(dst, name))) {
                    g_object_unref(s);
                    g_object_unref(d);
                    return false;
                }
            if (op.move) g_rmdir(src.c_str());
        }
    } else {
        struct P {
            FsOp *op;
            uint64_t base;
        } p{&op, op.done};
        if (g_file_copy(s, d, (GFileCopyFlags)(G_FILE_COPY_NOFOLLOW_SYMLINKS | G_FILE_COPY_ALL_METADATA | G_FILE_COPY_OVERWRITE),
                        nullptr,
                        [](goffset cur, goffset, gpointer pp) {
                            auto *p = static_cast<P *>(pp);
                            p->op->ui.set_completed(p->base + (uint64_t)cur);
                        },
                        &p, &err)) {
            op.done += (uint64_t)st.st_size;
            op.ui.set_completed(op.done);
            op.ui.file_done();
            if (op.move) g_unlink(src.c_str());
        } else {
            op.res.errors++;
            op.res.messages.push_back(std::string(err ? err->message : "Error") + " : " + src);
            if (err) g_error_free(err);
        }
    }
    g_object_unref(s);
    g_object_unref(d);
    return ok;
}

Result copy_files(Ui &ui, const std::vector<std::string> &files, const std::string &dest, bool move)
{
    Result r;
    uint64_t nfiles = 0, total = 0;
    for (const auto &f : files) total += tree_size(f, nfiles);
    ui.set_total(total);
    ui.set_num_files(nfiles);
    FsOp op{ui, r, move};
    for (const auto &f : files)
        if (!copy_one(op, f, path_join(dest, path_basename(f)))) {
            r.cancelled = ui.cancelled();
            break;
        }
    r.ok = r.errors == 0 && !r.cancelled;
    return r;
}

// Builds a folder whose layout is `prefix/` + links to `files`, so 7-Zip adds the files into that folder
// of the archive. Returns the top folder to pass to 7-Zip ("<tmp>/<first part of prefix>").
std::string stage_for_prefix(const std::string &prefix, const std::vector<std::string> &files,
                             const std::vector<std::string> &new_dirs)
{
    const std::string tmp = make_temp_dir("add");
    if (tmp.empty()) return {};
    const std::string inner = path_join(tmp, prefix);
    g_mkdir_with_parents(inner.c_str(), 0700);
    for (const auto &f : files) {
        const std::string link = path_join(inner, path_basename(f));
        if (symlink(f.c_str(), link.c_str()) != 0) return {};
    }
    for (const auto &d : new_dirs) g_mkdir_with_parents(path_join(inner, d).c_str(), 0755);
    return path_join(tmp, split(prefix, '/')[0]);
}

std::vector<std::string> paths_of(const std::vector<Row> &rows)
{
    std::vector<std::string> out;
    for (const auto &r : rows) out.push_back(r.full);
    return out;
}

// Arguments that keep an archive's password and header encryption when it is changed.
void add_password_args(std::vector<std::string> &a, const ArcLevel &arc)
{
    if (arc.password.empty()) return;
    a.push_back("-p" + arc.password);
    if (arc.format == "7z") a.push_back("-mhe=on");
}

// Adds a menu item that shows a keyboard shortcut (also for shortcuts that only work in the file list).
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

    panel_ = std::make_unique<Panel>();
    build_actions();

    GtkWidget *box = vbox(0);
    GMenuModel *menu = build_menu();
    GtkWidget *menubar = gtk_popover_menu_bar_new_from_model(menu);
    g_object_unref(menu);
    gtk_box_append(GTK_BOX(box), menubar);

    toolbar_ = build_toolbar();
    gtk_box_append(GTK_BOX(box), toolbar_);

    GtkWidget *addr = hbox(4);
    gtk_widget_add_css_class(addr, "szl-address");
    up_ = gtk_button_new_from_icon_name("go-up-symbolic");
    gtk_widget_set_tooltip_text(up_, "Up One Level (Backspace)");
    gtk_widget_add_css_class(up_, "flat");
    g_signal_connect_swapped(up_, "clicked", CB([](MainWindow *w) {
                                 w->panel().go_up();
                                 w->panel().focus();
                             }),
                             this);
    gtk_box_append(GTK_BOX(addr), up_);
    address_ = gtk_entry_new();
    gtk_widget_set_hexpand(address_, TRUE);
    g_signal_connect(address_, "activate", CB([](GtkEntry *e, MainWindow *w) {
                         w->panel().navigate_to(w->window(), gtk_editable_get_text(GTK_EDITABLE(e)));
                         w->panel().focus();
                     }),
                     this);
    gtk_box_append(GTK_BOX(addr), address_);
    gtk_box_append(GTK_BOX(box), addr);

    GtkWidget *pw = panel_->widget();
    gtk_widget_add_css_class(pw, "szl-panel");
    gtk_box_append(GTK_BOX(box), pw);

    status_bar_ = hbox(6);
    gtk_widget_add_css_class(status_bar_, "szl-statusbar");
    status_ = label("");
    gtk_label_set_ellipsize(GTK_LABEL(status_), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand(status_, TRUE);
    gtk_box_append(GTK_BOX(status_bar_), status_);
    gtk_box_append(GTK_BOX(box), status_bar_);
    gtk_window_set_child(GTK_WINDOW(win_), box);

    panel_->on_location_changed = [this] { update_address(); };
    panel_->on_selection_changed = [this] { update_status(); };
    panel_->on_open_file = [this](const Row &r) { launch_file(window(), r.full); };
    panel_->on_drop_files = [this](const std::vector<std::string> &files) {
        if (panel_->in_archive())
            add_files_to_archive(files);
        else
            copy_files_here(files);
    };

    g_signal_connect_swapped(win_, "close-request", CB([](MainWindow *w) -> gboolean {
                                 Settings &s = settings();
                                 gtk_window_get_default_size(w->window(), &s.width, &s.height);
                                 s.save();
                                 return FALSE;
                             }),
                             this);
    apply_view_settings();
}

MainWindow::~MainWindow() = default;

void MainWindow::present()
{
    gtk_window_present(GTK_WINDOW(win_));
    panel_->focus();
}

void MainWindow::open_path(const std::string &path)
{
    if (path.empty()) {
        const std::string last = settings().last_folder;
        panel_->navigate_fs(!last.empty() && g_file_test(last.c_str(), G_FILE_TEST_IS_DIR) ? last : home_dir());
        return;
    }
    panel_->navigate_to(window(), path);
}

void MainWindow::update_address()
{
    gtk_editable_set_text(GTK_EDITABLE(address_), panel_->address().c_str());
    std::string title = panel_->in_archive() ? panel_->arc().virtual_path : panel_->fs_dir();
    if (panel_->in_archive() && !panel_->prefix().empty()) title += "/" + panel_->prefix();
    title = path_basename(title) + " - 7zipl";
    gtk_window_set_title(GTK_WINDOW(win_), title.c_str());
    const bool upd = !panel_->in_archive() || panel_->arc().can_update;
    for (const char *a : {"delete", "rename", "new-folder", "move-to"}) {
        GAction *ac = g_action_map_lookup_action(G_ACTION_MAP(win_), a);
        if (ac) g_simple_action_set_enabled(G_SIMPLE_ACTION(ac), upd);
    }
    GAction *nf = g_action_map_lookup_action(G_ACTION_MAP(win_), "new-file");
    if (nf) g_simple_action_set_enabled(G_SIMPLE_ACTION(nf), !panel_->in_archive());
    update_status();
}

void MainWindow::update_status() { gtk_label_set_text(GTK_LABEL(status_), panel_->status_text().c_str()); }

void MainWindow::apply_view_settings()
{
    const Settings &s = settings();
    gtk_widget_set_visible(toolbar_, s.show_toolbar);
    gtk_widget_set_visible(status_bar_, s.show_statusbar);
    for (GtkWidget *c = gtk_widget_get_first_child(toolbar_); c; c = gtk_widget_get_next_sibling(c)) {
        GtkWidget *l = GTK_WIDGET(g_object_get_data(G_OBJECT(c), "tool-label"));
        if (l) gtk_widget_set_visible(l, s.toolbar_text);
    }
    panel_->apply_view_settings();
}

void MainWindow::after_change()
{
    panel_->refresh(window());
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
        {"open-outside", [](MainWindow *w) { w->cmd_open(2); }},
        {"rename", [](MainWindow *w) { w->cmd_rename(); }},
        {"copy-to", [](MainWindow *w) { w->cmd_copy_move(false); }},
        {"move-to", [](MainWindow *w) { w->cmd_copy_move(true); }},
        {"delete", [](MainWindow *w) { w->cmd_delete(false); }},
        {"delete-permanently", [](MainWindow *w) { w->cmd_delete(true); }},
        {"properties", [](MainWindow *w) { w->cmd_properties(); }},
        {"new-folder", [](MainWindow *w) { w->cmd_new_folder(); }},
        {"new-file", [](MainWindow *w) { w->cmd_new_file(); }},
        {"add", [](MainWindow *w) { w->cmd_add(); }},
        {"extract", [](MainWindow *w) { w->cmd_extract(); }},
        {"test", [](MainWindow *w) { w->cmd_test(); }},
        {"select-all", [](MainWindow *w) { w->panel().select_all(); }},
        {"deselect-all", [](MainWindow *w) { w->panel().select_none(); }},
        {"invert-selection", [](MainWindow *w) { w->panel().invert_selection(); }},
        {"select-mask", [](MainWindow *w) { w->cmd_select_mask(true); }},
        {"deselect-mask", [](MainWindow *w) { w->cmd_select_mask(false); }},
        {"refresh", [](MainWindow *w) { w->panel().refresh(w->window()); }},
        {"up", [](MainWindow *w) { w->panel().go_up(); }},
        {"root", [](MainWindow *w) { w->panel().go_root(); }},
        {"options", [](MainWindow *w) { w->cmd_options(); }},
        {"about", [](MainWindow *w) { about_dialog(w->window()); }},
        {"close", [](MainWindow *w) { gtk_window_close(w->window()); }},
        {"focus-address", [](MainWindow *w) {
             gtk_widget_grab_focus(w->address_);
             gtk_editable_select_region(GTK_EDITABLE(w->address_), 0, -1);
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

    // Sorting by a column: sort-by(i)
    GSimpleAction *sort = g_simple_action_new("sort-by", G_VARIANT_TYPE_INT32);
    g_signal_connect(sort, "activate", CB([](GSimpleAction *, GVariant *v, MainWindow *w) {
                         GListModel *cols = gtk_column_view_get_columns(GTK_COLUMN_VIEW(w->panel().list()));
                         auto *c = GTK_COLUMN_VIEW_COLUMN(g_list_model_get_item(cols, (guint)g_variant_get_int32(v)));
                         if (!c) return;
                         gtk_column_view_sort_by_column(GTK_COLUMN_VIEW(w->panel().list()), c, GTK_SORT_ASCENDING);
                         g_object_unref(c);
                     }),
                     this);
    g_action_map_add_action(G_ACTION_MAP(win_), G_ACTION(sort));
    g_object_unref(sort);

    // Checksums: hash(method)
    GSimpleAction *hash = g_simple_action_new("hash", G_VARIANT_TYPE_STRING);
    g_signal_connect(hash, "activate", CB([](GSimpleAction *, GVariant *v, MainWindow *w) {
                         w->cmd_hash(g_variant_get_string(v, nullptr));
                     }),
                     this);
    g_action_map_add_action(G_ACTION_MAP(win_), G_ACTION(hash));
    g_object_unref(hash);

    // Favorites: add-favorite(i), goto-favorite(i)
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

    // View toggles
    struct T {
        const char *name;
        bool Settings::*field;
    };
    static const T toggles[] = {
        {"show-toolbar", &Settings::show_toolbar}, {"toolbar-text", &Settings::toolbar_text},
        {"show-statusbar", &Settings::show_statusbar}, {"show-hidden", &Settings::show_hidden},
        {"show-grid", &Settings::show_grid}, {"single-click", &Settings::single_click},
        {"show-dotdot", &Settings::show_dotdot},
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
                             w->panel().set_show_hidden(settings().show_hidden);
                             w->apply_view_settings();
                             if (t->field == &Settings::show_hidden || t->field == &Settings::show_dotdot)
                                 w->panel().refresh(w->window());
                         }),
                         this);
        g_action_map_add_action(G_ACTION_MAP(win_), G_ACTION(sa));
        g_object_unref(sa);
    }

    GtkApplication *app = gtk_window_get_application(GTK_WINDOW(win_));
    struct K {
        const char *action;
        std::vector<const char *> keys;
    };
    // Shortcuts that also work while typing in the address bar.
    const K keys[] = {
        {"win.properties", {"<Alt>Return"}},
        {"win.new-file", {"<Control>n"}},
        {"win.refresh", {"<Control>r"}},
        {"win.root", {"<Control>backslash"}},
        {"win.show-hidden", {"<Control>h"}},
        {"win.focus-address", {"<Control>l", "<Alt>d"}},
        {"win.close", {"<Alt>F4", "<Control>q", "<Control>w"}},
    };
    for (const K &k : keys) {
        std::vector<const char *> v = k.keys;
        v.push_back(nullptr);
        gtk_application_set_accels_for_action(app, k.action, v.data());
    }
    // Single keys work in the file list only, so typing in the address bar is safe.
    static const char *const list_keys[][2] = {
        {"<Shift>Return", "win.open-outside"}, {"<Control>Page_Down", "win.open-inside"},
        {"F2", "win.rename"}, {"F5", "win.copy-to"}, {"F6", "win.move-to"}, {"F7", "win.new-folder"},
        {"Delete", "win.delete"}, {"KP_Delete", "win.delete"}, {"<Shift>Delete", "win.delete-permanently"},
        {"<Control>a", "win.select-all"}, {"<Shift>KP_Add", "win.select-all"},
        {"<Shift>KP_Subtract", "win.deselect-all"}, {"KP_Multiply", "win.invert-selection"},
        {"KP_Add", "win.select-mask"}, {"KP_Subtract", "win.deselect-mask"},
    };
    GtkEventController *sc = gtk_shortcut_controller_new();
    for (const auto &k : list_keys)
        gtk_shortcut_controller_add_shortcut(GTK_SHORTCUT_CONTROLLER(sc),
                                             gtk_shortcut_new(gtk_shortcut_trigger_parse_string(k[0]),
                                                              gtk_named_action_new(k[1])));
    gtk_widget_add_controller(panel_->list(), sc);
    const char *sort_keys[][2] = {{"win.sort-by(0)", "<Control>F3"}, {"win.sort-by(3)", "<Control>F5"},
                                  {"win.sort-by(1)", "<Control>F6"}};
    for (auto &sk : sort_keys) {
        const char *v[] = {sk[1], nullptr};
        gtk_application_set_accels_for_action(app, sk[0], v);
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
}

GMenuModel *MainWindow::build_menu()
{
    GMenu *bar = g_menu_new();

    GMenu *file = g_menu_new();
    GMenu *s1 = g_menu_new();
    item(s1, "_Open", "win.open", "Return");
    item(s1, "Open _Inside", "win.open-inside", "<Control>Page_Down");
    item(s1, "Open O_utside", "win.open-outside", "<Shift>Return");
    g_menu_append_section(file, nullptr, G_MENU_MODEL(s1));
    GMenu *s2 = g_menu_new();
    item(s2, "Re_name", "win.rename", "F2");
    item(s2, "_Copy To...", "win.copy-to", "F5");
    item(s2, "_Move To...", "win.move-to", "F6");
    item(s2, "_Delete", "win.delete", "Delete");
    g_menu_append_section(file, nullptr, G_MENU_MODEL(s2));
    GMenu *s3 = g_menu_new();
    g_menu_append(s3, "P_roperties", "win.properties");
    GMenu *crc = g_menu_new();
    for (const auto &m : hash_methods()) {
        const std::string a = "win.hash('" + m + "')";
        g_menu_append(crc, m.c_str(), a.c_str());
    }
    g_menu_append(crc, "*", "win.hash('*')");
    g_menu_append_submenu(s3, "CRC", G_MENU_MODEL(crc));
    g_menu_append_section(file, nullptr, G_MENU_MODEL(s3));
    GMenu *s4 = g_menu_new();
    item(s4, "Create Folder", "win.new-folder", "F7");
    g_menu_append(s4, "Create File", "win.new-file");
    g_menu_append_section(file, nullptr, G_MENU_MODEL(s4));
    GMenu *s5 = g_menu_new();
    g_menu_append(s5, "E_xit", "win.close");
    g_menu_append_section(file, nullptr, G_MENU_MODEL(s5));
    g_menu_append_submenu(bar, "_File", G_MENU_MODEL(file));

    GMenu *edit = g_menu_new();
    item(edit, "Select _All", "win.select-all", "<Control>a");
    item(edit, "Deselect All", "win.deselect-all", "<Shift>KP_Subtract");
    item(edit, "_Invert Selection", "win.invert-selection", "KP_Multiply");
    item(edit, "Select...", "win.select-mask", "KP_Add");
    item(edit, "Deselect...", "win.deselect-mask", "KP_Subtract");
    g_menu_append_submenu(bar, "_Edit", G_MENU_MODEL(edit));

    GMenu *view = g_menu_new();
    GMenu *v1 = g_menu_new();
    g_menu_append(v1, "Name", "win.sort-by(0)");
    g_menu_append(v1, "Size", "win.sort-by(1)");
    g_menu_append(v1, "Date", "win.sort-by(3)");
    g_menu_append_section(view, "Arrange by", G_MENU_MODEL(v1));
    GMenu *v2 = g_menu_new();
    g_menu_append(v2, "Toolbar", "win.show-toolbar");
    g_menu_append(v2, "Show Buttons Text", "win.toolbar-text");
    g_menu_append(v2, "Status Bar", "win.show-statusbar");
    g_menu_append(v2, "Hidden Files", "win.show-hidden");
    g_menu_append_section(view, nullptr, G_MENU_MODEL(v2));
    GMenu *v3 = g_menu_new();
    g_menu_append(v3, "Open Root Folder", "win.root");
    item(v3, "Up One Level", "win.up", "BackSpace");
    g_menu_append(v3, "_Refresh", "win.refresh");
    g_menu_append_section(view, nullptr, G_MENU_MODEL(v3));
    g_menu_append_submenu(bar, "_View", G_MENU_MODEL(view));

    favorites_menu_ = g_menu_new();
    g_menu_append_submenu(bar, "F_avorites", G_MENU_MODEL(favorites_menu_));
    rebuild_favorites_menu();

    GMenu *tools = g_menu_new();
    g_menu_append(tools, "_Options...", "win.options");
    g_menu_append_submenu(bar, "_Tools", G_MENU_MODEL(tools));

    GMenu *help = g_menu_new();
    g_menu_append(help, "_About 7zipl...", "win.about");
    g_menu_append_submenu(bar, "_Help", G_MENU_MODEL(help));

    for (GMenu *m : {file, s1, s2, s3, s4, s5, crc, edit, view, v1, v2, v3, tools, help}) g_object_unref(m);
    return G_MENU_MODEL(bar);
}

void MainWindow::rebuild_favorites_menu()
{
    g_menu_remove_all(favorites_menu_);
    GMenu *add = g_menu_new();
    for (int i = 0; i < 10; i++) {
        const std::string a = "win.add-favorite(" + std::to_string(i) + ")";
        g_menu_append(add, ("Bookmark " + std::to_string(i)).c_str(), a.c_str());
    }
    g_menu_append_submenu(favorites_menu_, "Add folder to Favorites as", G_MENU_MODEL(add));
    g_object_unref(add);
    GMenu *list = g_menu_new();
    for (int i = 0; i < 10; i++) {
        const std::string &f = settings().favorites[(size_t)i];
        if (f.empty()) continue;
        const std::string a = "win.goto-favorite(" + std::to_string(i) + ")";
        g_menu_append(list, (std::to_string(i) + "  " + f).c_str(), a.c_str());
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
        gtk_box_append(GTK_BOX(tb), btn);
    }
    return tb;
}

// ---------- commands ----------

void MainWindow::cmd_open(int mode)
{
    // Enter in the address bar is handled by the entry itself.
    if (gtk_widget_has_focus(address_) || gtk_widget_is_ancestor(gtk_root_get_focus(GTK_ROOT(win_)), address_)) return;
    auto r = panel_->focused();
    if (!r) return;
    panel_->activate(window(), *r, mode);
}

void MainWindow::cmd_add()
{
    if (panel_->in_archive()) {
        if (!panel_->arc().can_update) {
            show_message(window(), "7zipl", "This archive can't be changed (" + panel_->arc().format + ").", true);
            return;
        }
        choose_open_files(window(), "Add files to archive", panel_->fs_dir(),
                          [this](const std::vector<std::string> &files) { add_files_to_archive(files); });
        return;
    }
    auto sel = panel_->selected();
    if (sel.empty()) {
        show_message(window(), "7zipl", "Select one or more files and folders to add to an archive.");
        return;
    }
    compress_dialog(window(), panel_->fs_dir(), paths_of(sel), {}, [this](const CompressRequest &req) {
        JobSpec spec;
        spec.title = "Compressing";
        spec.archive = req.archive;
        spec.show_ratio = true;
        const auto args = req.args;
        const std::string archive = req.archive;
        start_job(window(), spec, [args](Ui &ui) { return run(args, ui); },
                  [this, archive](const Result &) {
                      if (panel_->in_archive()) return;
                      panel_->refresh(window());
                      if (path_dirname(archive) == panel_->fs_dir()) panel_->select_name(path_basename(archive));
                  });
    });
}

void MainWindow::add_files_to_archive(const std::vector<std::string> &files)
{
    if (!panel_->in_archive() || files.empty()) return;
    ArcLevel &a = panel_->arc();
    if (!a.can_update) {
        show_message(window(), "7zipl", "This archive can't be changed (" + a.format + ").", true);
        return;
    }
    std::vector<std::string> args = {"a"};
    add_password_args(args, a);
    args.push_back("-spd");
    args.push_back("--");
    args.push_back(a.file);
    if (panel_->prefix().empty()) {
        for (const auto &f : files) args.push_back(f);
    } else {
        const std::string top = stage_for_prefix(panel_->prefix(), files, {});
        if (top.empty()) {
            show_message(window(), "7zipl", "Cannot create a temporary folder", true);
            return;
        }
        args.push_back(top);
    }
    JobSpec spec;
    spec.title = "Compressing";
    spec.archive = a.file;
    spec.show_ratio = true;
    spec.password = a.password;
    start_job(window(), spec, [args](Ui &ui) { return run(args, ui); }, [this](const Result &) { after_change(); });
}

void MainWindow::copy_files_here(const std::vector<std::string> &files)
{
    const std::string dest = panel_->fs_dir();
    JobSpec spec;
    spec.title = "Copying";
    start_job(window(), spec, [files, dest](Ui &ui) { return copy_files(ui, files, dest, false); },
              [this](const Result &) { after_change(); });
}

void MainWindow::cmd_extract()
{
    if (panel_->in_archive()) {
        ArcLevel &a = panel_->arc();
        auto sel = panel_->selected();
        const std::string prefix = panel_->prefix();
        std::vector<std::string> items = paths_of(sel);
        if (items.empty() && !prefix.empty()) items.push_back(prefix);
        const std::string file = a.file, password = a.password;
        const std::string base = panel_->fs_dir();
        extract_dialog(window(), base, {a.virtual_path}, true,
                       [this, file, password, items, prefix](const ExtractChoice &c) {
                           ExtractRequest req;
                           req.archive = file;
                           req.out_dir = c.out_dir;
                           req.items = items;
                           req.strip_prefix = prefix;
                           req.path_mode = c.path_mode;
                           req.overwrite = c.overwrite;
                           req.password = c.password.empty() ? password : c.password;
                           JobSpec spec;
                           spec.title = "Extracting";
                           spec.archive = file;
                           spec.password = req.password;
                           start_job(window(), spec, [req](Ui &ui) { return extract_items(req, ui); });
                       });
        return;
    }
    std::vector<std::string> archives;
    for (const Row &r : panel_->selected())
        if (!r.dir) archives.push_back(r.full);
    if (archives.empty()) {
        show_message(window(), "7zipl", "Select one or more archives to extract.");
        return;
    }
    extract_dialog(window(), panel_->fs_dir(), archives, false, [this, archives](const ExtractChoice &c) {
        JobSpec spec;
        spec.title = "Extracting";
        spec.archive = archives.size() == 1 ? archives[0] : std::string();
        const auto args = extract_args(c, archives);
        start_job(window(), spec, [args](Ui &ui) { return run(args, ui); }, [this](const Result &) { after_change(); });
    });
}

void MainWindow::cmd_test()
{
    JobSpec spec;
    spec.title = "Testing";
    spec.report_success = true;
    if (panel_->in_archive()) {
        ArcLevel &a = panel_->arc();
        ExtractRequest req;
        req.archive = a.file;
        req.test = true;
        req.items = paths_of(panel_->selected());
        if (req.items.empty() && !panel_->prefix().empty()) req.items.push_back(panel_->prefix());
        req.password = a.password;
        spec.archive = a.virtual_path;
        spec.password = a.password;
        start_job(window(), spec, [req](Ui &ui) { return extract_items(req, ui); });
        return;
    }
    std::vector<std::string> archives;
    for (const Row &r : panel_->selected())
        if (!r.dir) archives.push_back(r.full);
    if (archives.empty()) {
        show_message(window(), "7zipl", "Select one or more archives to test.");
        return;
    }
    spec.archive = archives.size() == 1 ? archives[0] : std::string();
    ExtractChoice c;
    const auto args = extract_args(c, archives, true);
    start_job(window(), spec, [args](Ui &ui) { return run(args, ui); });
}

void MainWindow::cmd_copy_move(bool move)
{
    auto sel = panel_->selected();
    if (sel.empty()) return;
    if (panel_->in_archive()) {
        ArcLevel &a = panel_->arc();
        if (move && !a.can_update) return;
        const std::string file = a.file, password = a.password, prefix = panel_->prefix();
        const auto items = paths_of(sel);
        choose_folder(window(), move ? "Move to" : "Copy to", panel_->fs_dir(),
                      [this, file, password, prefix, items, move](const std::string &dir) {
                          ExtractRequest req;
                          req.archive = file;
                          req.out_dir = dir;
                          req.items = items;
                          req.strip_prefix = prefix;
                          req.password = password;
                          JobSpec spec;
                          spec.title = move ? "Moving" : "Extracting";
                          spec.archive = file;
                          spec.password = password;
                          std::vector<std::string> del = {"d"};
                          if (!password.empty()) del.push_back("-p" + password);
                          del.push_back("-spd");
                          del.push_back("--");
                          del.push_back(file);
                          for (const auto &i : items) del.push_back(i);
                          if (!password.empty() && panel_->in_archive() && panel_->arc().format == "7z")
                              del.insert(del.begin() + 1, "-mhe=on");
                          start_job(window(), spec,
                                    [req, del, move](Ui &ui) {
                                        Result r = extract_items(req, ui);
                                        if (move && r.ok && !r.cancelled) {
                                            Result d = run(del, ui);
                                            d.files = r.files;
                                            return d;
                                        }
                                        return r;
                                    },
                                    [this, move](const Result &) {
                                        if (move) after_change();
                                    });
                      });
        return;
    }
    const auto files = paths_of(sel);
    choose_folder(window(), move ? "Move to" : "Copy to", panel_->fs_dir(), [this, files, move](const std::string &dir) {
        JobSpec spec;
        spec.title = move ? "Moving" : "Copying";
        start_job(window(), spec, [files, dir, move](Ui &ui) { return copy_files(ui, files, dir, move); },
                  [this](const Result &) { after_change(); });
    });
}

void MainWindow::cmd_delete(bool permanently)
{
    auto sel = panel_->selected();
    if (sel.empty()) return;
    const std::string what = sel.size() == 1 ? "'" + sel[0].name + "'" : std::to_string(sel.size()) + " items";
    if (panel_->in_archive()) {
        ArcLevel &a = panel_->arc();
        if (!a.can_update) {
            show_message(window(), "7zipl", "This archive can't be changed (" + a.format + ").", true);
            return;
        }
        std::vector<std::string> args = {"d"};
        add_password_args(args, a);
        args.push_back("-spd");
        args.push_back("--");
        args.push_back(a.file);
        for (const Row &r : sel) args.push_back(r.full);
        const std::string file = a.file, password = a.password;
        ask_yes_no(window(), "Confirm File Delete", "Are you sure you want to delete " + what + " from the archive?",
                   [this, args, file, password](bool yes) {
                       if (!yes) return;
                       JobSpec spec;
                       spec.title = "Deleting";
                       spec.archive = file;
                       spec.password = password;
                       start_job(window(), spec, [args](Ui &ui) { return run(args, ui); },
                                 [this](const Result &) { after_change(); });
                   });
        return;
    }
    const auto files = paths_of(sel);
    const std::string q = permanently ? "Are you sure you want to permanently delete " + what + "?"
                                      : "Are you sure you want to move " + what + " to the Trash?";
    ask_yes_no(window(), permanently ? "Confirm Delete" : "Confirm Move to Trash", q, [this, files, permanently](bool yes) {
        if (!yes) return;
        JobSpec spec;
        spec.title = "Deleting";
        spec.show_delay_ms = 300;
        start_job(window(), spec,
                  [files, permanently](Ui &ui) {
                      Result r;
                      ui.set_num_files(files.size());
                      for (const auto &f : files) {
                          if (ui.cancelled()) break;
                          ui.set_current("Deleting", f);
                          if (permanently) {
                              remove_tree(f);
                              if (g_file_test(f.c_str(), G_FILE_TEST_EXISTS) || g_file_test(f.c_str(), G_FILE_TEST_IS_SYMLINK)) {
                                  r.errors++;
                                  r.messages.push_back("Cannot delete " + f);
                              }
                          } else {
                              GFile *gf = g_file_new_for_path(f.c_str());
                              GError *err = nullptr;
                              if (!g_file_trash(gf, nullptr, &err)) {
                                  r.errors++;
                                  r.messages.push_back(std::string(err->message) + " : " + f);
                                  g_error_free(err);
                              }
                              g_object_unref(gf);
                          }
                          ui.file_done();
                      }
                      r.ok = r.errors == 0;
                      return r;
                  },
                  [this](const Result &) { after_change(); });
    });
}

void MainWindow::cmd_rename()
{
    auto r = panel_->focused();
    if (!r) return;
    const Row row = *r;
    ask_text(window(), "Rename", "New name:", row.name, [this, row](const std::string &name_in) {
        const std::string name = trim(name_in);
        if (name.empty() || name == row.name) return;
        if (name.find('/') != std::string::npos) {
            show_message(window(), "7zipl", "The name can't contain '/'", true);
            return;
        }
        if (panel_->in_archive()) {
            ArcLevel &a = panel_->arc();
            if (!a.can_update) return;
            const std::string dir = row.full.find('/') == std::string::npos ? std::string() : path_dirname(row.full);
            std::vector<std::string> args = {"rn"};
            add_password_args(args, a);
            args.push_back("-spd");
            args.push_back("--");
            args.push_back(a.file);
            args.push_back(row.full);
            args.push_back(dir.empty() ? name : dir + "/" + name);
            JobSpec spec;
            spec.title = "Renaming";
            spec.archive = a.file;
            spec.password = a.password;
            spec.show_delay_ms = 300;
            start_job(window(), spec, [args](Ui &ui) { return run(args, ui); }, [this, name](const Result &) {
                after_change();
            });
            return;
        }
        const std::string dst = path_join(panel_->fs_dir(), name);
        if (g_file_test(dst.c_str(), G_FILE_TEST_EXISTS)) {
            show_message(window(), "7zipl", "A file or folder with this name already exists:\n" + dst, true);
            return;
        }
        if (g_rename(row.full.c_str(), dst.c_str()) != 0) {
            show_message(window(), "7zipl", std::string("Cannot rename: ") + g_strerror(errno), true);
            return;
        }
        panel_->refresh(window());
        panel_->select_name(name);
    });
}

void MainWindow::cmd_new_folder()
{
    ask_text(window(), "Create Folder", "Folder name:", "New Folder", [this](const std::string &name_in) {
        const std::string name = trim(name_in);
        if (name.empty() || name.find('/') != std::string::npos) return;
        if (panel_->in_archive()) {
            ArcLevel &a = panel_->arc();
            if (!a.can_update) return;
            const std::string prefix = panel_->prefix();
            std::vector<std::string> args = {"a"};
            add_password_args(args, a);
            args.push_back("-spd");
            args.push_back("--");
            args.push_back(a.file);
            const std::string top = stage_for_prefix(prefix.empty() ? name : prefix, {}, prefix.empty() ? std::vector<std::string>{} : std::vector<std::string>{name});
            if (top.empty()) return;
            args.push_back(top);
            JobSpec spec;
            spec.title = "Updating";
            spec.archive = a.file;
            spec.password = a.password;
            spec.show_delay_ms = 300;
            start_job(window(), spec, [args](Ui &ui) { return run(args, ui); }, [this, name](const Result &) {
                after_change();
            });
            return;
        }
        const std::string p = path_join(panel_->fs_dir(), name);
        if (g_mkdir(p.c_str(), 0755) != 0) {
            show_message(window(), "7zipl", std::string("Cannot create folder: ") + g_strerror(errno), true);
            return;
        }
        panel_->refresh(window());
        panel_->select_name(name);
    });
}

void MainWindow::cmd_new_file()
{
    if (panel_->in_archive()) return;
    ask_text(window(), "Create File", "File name:", "New File.txt", [this](const std::string &name_in) {
        const std::string name = trim(name_in);
        if (name.empty() || name.find('/') != std::string::npos) return;
        const std::string p = path_join(panel_->fs_dir(), name);
        if (g_file_test(p.c_str(), G_FILE_TEST_EXISTS)) {
            show_message(window(), "7zipl", "The file already exists", true);
            return;
        }
        if (!g_file_set_contents(p.c_str(), "", 0, nullptr)) {
            show_message(window(), "7zipl", "Cannot create the file", true);
            return;
        }
        panel_->refresh(window());
        panel_->select_name(name);
    });
}

void MainWindow::cmd_properties()
{
    auto sel = panel_->selected();
    std::vector<std::pair<std::string, std::string>> rows;
    auto time_row = [&](const char *n, bool has, int64_t t) {
        if (has) rows.emplace_back(n, format_time(t));
    };
    if (sel.size() == 1) {
        const Row &r = sel[0];
        rows.emplace_back("Name", r.name);
        rows.emplace_back("Path", panel_->in_archive() ? r.full : r.full);
        rows.emplace_back("Folder", r.dir ? "+" : "-");
        if (r.has_size) rows.emplace_back("Size", format_number(r.size));
        if (r.has_packed) rows.emplace_back("Packed Size", format_number(r.packed));
        if (r.dir && panel_->in_archive()) {
            rows.emplace_back("Folders", format_number(r.num_folders));
            rows.emplace_back("Files", format_number(r.num_files));
        }
        time_row("Modified", r.has_mtime, r.mtime);
        time_row("Created", r.has_ctime, r.ctime);
        time_row("Accessed", r.has_atime, r.atime);
        if (!r.attrib.empty()) rows.emplace_back("Attributes", r.attrib);
        if (panel_->in_archive() && !r.dir) rows.emplace_back("Encrypted", r.encrypted ? "+" : "-");
        if (!r.method.empty()) rows.emplace_back("Method", r.method);
        if (r.has_crc) {
            char b[16];
            g_snprintf(b, sizeof b, "%08X", r.crc);
            rows.emplace_back("CRC", b);
        }
        if (!r.comment.empty()) rows.emplace_back("Comment", r.comment);
        if (!r.user.empty()) rows.emplace_back("User", r.user);
        if (!r.group.empty()) rows.emplace_back("Group", r.group);
        if (!r.link.empty()) rows.emplace_back("Link", r.link);
        if (!panel_->in_archive() && !r.content_type.empty()) {
            char *d = g_content_type_get_description(r.content_type.c_str());
            if (d) rows.emplace_back("Type", d);
            g_free(d);
        }
    } else if (sel.size() > 1) {
        uint64_t size = 0, packed = 0, files = 0, dirs = 0;
        for (const Row &r : sel) {
            size += r.size;
            packed += r.packed;
            if (r.dir) {
                dirs++;
                files += r.num_files;
                dirs += r.num_folders;
            } else {
                files++;
            }
        }
        rows.emplace_back("Selected", format_number(sel.size()));
        rows.emplace_back("Size", format_number(size));
        if (panel_->in_archive()) {
            rows.emplace_back("Packed Size", format_number(packed));
            rows.emplace_back("Folders", format_number(dirs));
            rows.emplace_back("Files", format_number(files));
        }
    }
    if (panel_->in_archive()) {
        const ArcLevel &a = panel_->arc();
        rows.emplace_back("--", path_basename(a.virtual_path));
        rows.emplace_back("Path", a.virtual_path);
        for (const auto &p : a.props) rows.push_back(p);
    }
    if (rows.empty()) return;
    properties_window(window(), "Properties", rows);
}

void MainWindow::cmd_hash(const std::string &method)
{
    auto sel = panel_->selected();
    if (sel.empty()) return;
    settings().hash_method = method;
    JobSpec spec;
    spec.title = "Checksum calculating";
    spec.silent = false;
    std::vector<std::string> base = {"h", "-scrc" + method, "-spd", "--"};
    GtkWindow *parent = window();
    auto done = [parent](const Result &r) {
        if (r.cancelled || r.hash.methods.empty()) return;
        hash_window(parent, r.hash);
    };
    if (panel_->in_archive()) {
        // Checksums of archive items: extract them to a temporary folder first.
        ArcLevel &a = panel_->arc();
        const std::string tmp = make_temp_dir("hash");
        ExtractRequest req;
        req.archive = a.file;
        req.out_dir = tmp;
        req.items = paths_of(sel);
        req.strip_prefix = panel_->prefix();
        req.password = a.password;
        req.overwrite = 1;
        std::vector<std::string> args = base;
        for (const Row &r : sel) args.push_back(path_join(tmp, r.name));
        spec.password = a.password;
        start_job(parent, spec,
                  [req, args, tmp](Ui &ui) {
                      Result r = extract_items(req, ui);
                      if (!r.ok) return r;
                      Result h = run(args, ui);
                      remove_tree(tmp);
                      return h;
                  },
                  done);
        return;
    }
    std::vector<std::string> args = base;
    for (const Row &r : sel) args.push_back(r.full);
    start_job(parent, spec, [args](Ui &ui) { return run(args, ui); }, done);
}

void MainWindow::cmd_select_mask(bool select)
{
    ask_text(window(), select ? "Select" : "Deselect", "Mask:", "*", [this, select](const std::string &m) {
        panel_->select_mask(m.empty() ? "*" : m, select);
    });
}

void MainWindow::cmd_add_favorite(int i)
{
    if (i < 0 || i > 9) return;
    std::string where = panel_->address();
    while (where.size() > 1 && where.back() == '/') where.pop_back();
    settings().favorites[(size_t)i] = where;
    settings().save();
    rebuild_favorites_menu();
}

void MainWindow::cmd_goto_favorite(int i)
{
    if (i < 0 || i > 9) return;
    const std::string f = settings().favorites[(size_t)i];
    if (!f.empty()) panel_->navigate_to(window(), f);
}

void MainWindow::cmd_options()
{
    GtkWidget *w = dialog_window(window(), "Options", 380, -1);
    GtkWidget *box = vbox(6);
    set_margins(box, 12);
    struct O {
        const char *label;
        const char *action;
    };
    static const O opts[] = {
        {"Show \"..\" item", "win.show-dotdot"},
        {"Show grid lines", "win.show-grid"},
        {"Single-click to open an item", "win.single-click"},
        {"Show hidden files", "win.show-hidden"},
        {"Show toolbar", "win.show-toolbar"},
        {"Show buttons text", "win.toolbar-text"},
        {"Show status bar", "win.show-statusbar"},
    };
    gtk_box_append(GTK_BOX(box), bold_label("Settings"));
    for (const O &o : opts) {
        GtkWidget *c = gtk_check_button_new_with_label(o.label);
        gtk_actionable_set_action_name(GTK_ACTIONABLE(c), o.action);
        gtk_box_append(GTK_BOX(box), c);
    }
    GtkWidget *cfg = label("Settings are stored in " + config_dir() + "/7zipl.conf");
    gtk_label_set_use_underline(GTK_LABEL(cfg), FALSE);
    gtk_label_set_text(GTK_LABEL(cfg), ("Settings are stored in " + config_dir() + "/7zipl.conf").c_str());
    gtk_label_set_wrap(GTK_LABEL(cfg), TRUE);
    gtk_widget_add_css_class(cfg, "dim-label");
    gtk_widget_set_margin_top(cfg, 8);
    gtk_box_append(GTK_BOX(box), cfg);
    auto b = button_row(box, {"Close"});
    g_signal_connect_swapped(b[0], "clicked", G_CALLBACK(gtk_window_destroy), w);
    gtk_window_set_child(GTK_WINDOW(w), box);
    // The check buttons use the main window's actions.
    gtk_widget_insert_action_group(w, "win", G_ACTION_GROUP(win_));
    gtk_window_present(GTK_WINDOW(w));
}

}  // namespace szl
