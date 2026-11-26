// SPDX-License-Identifier: LGPL-2.1-or-later
#include "optionsdialog.hpp"

#include "gtkutil.hpp"
#include "integration.hpp"
#include "settings.hpp"
#include "util.hpp"

#include <algorithm>

namespace szl {

namespace {

struct Dlg {
    GtkWindow *win;
    std::function<void()> applied;
    // System
    GtkWidget *entry_state, *entry_button, *assoc_box;
    std::vector<std::pair<const AssocFormat *, GtkWidget *>> assoc_checks;
    std::vector<std::pair<const AssocFormat *, GtkWidget *>> assoc_current;
    // 7-Zip
    std::vector<std::pair<std::string, GtkWidget *>> fm_checks;
    std::vector<std::pair<unsigned, GtkWidget *>> item_checks;
    GtkWidget *cascaded, *elim_dup;
    // Folders
    GtkWidget *work_system, *work_current, *work_specified, *work_dir;
    // Editor
    GtkWidget *viewer, *editor, *diff;
    // Settings
    GtkWidget *dotdot, *real_icons, *grid, *single_click, *hidden, *mem_limit, *mem_gb;
};

GtkWidget *page_box()
{
    GtkWidget *b = vbox(6);
    set_margins(b, 12);
    return b;
}

GtkWidget *note(const std::string &text)
{
    GtkWidget *l = gtk_label_new(text.c_str());
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_label_set_wrap(GTK_LABEL(l), TRUE);
    gtk_widget_add_css_class(l, "dim-label");
    return l;
}

// ---- System ----

void refresh_entry_state(Dlg *d)
{
    const EntryState st = desktop_entry_state();
    std::string text;
    switch (st) {
    case EntryState::System: text = "7zipl is installed for all users (" + desktop_entry_path() + ")."; break;
    case EntryState::User: text = "7zipl's desktop entry is installed for the current user (" + desktop_entry_path() + ")."; break;
    case EntryState::Missing:
        text = "7zipl isn't installed as a desktop application, so it can't be the default program for archives yet.";
        break;
    }
    gtk_label_set_text(GTK_LABEL(d->entry_state), text.c_str());
    gtk_widget_set_visible(d->entry_button, st != EntryState::System);
    gtk_button_set_label(GTK_BUTTON(d->entry_button), st == EntryState::User ? "Remove the user entry" : "Install for the current user");
    gtk_widget_set_sensitive(d->assoc_box, st != EntryState::Missing);
    for (auto &[f, l] : d->assoc_current) {
        const std::string n = current_default_name(*f);
        gtk_label_set_text(GTK_LABEL(l), n.empty() ? "(none)" : n.c_str());
    }
}

GtkWidget *system_page(Dlg *d)
{
    GtkWidget *b = page_box();
    d->entry_state = note("");
    gtk_box_append(GTK_BOX(b), d->entry_state);
    d->entry_button = gtk_button_new_with_label("");
    gtk_widget_set_halign(d->entry_button, GTK_ALIGN_START);
    g_signal_connect_swapped(d->entry_button, "clicked", CB([](Dlg *d) {
                                 std::string err;
                                 if (desktop_entry_state() == EntryState::User) {
                                     remove_user_entry();
                                 } else if (!install_user_entry(err)) {
                                     show_message(d->win, "7zipl", "Cannot install the desktop entry: " + err, true);
                                 }
                                 refresh_entry_state(d);
                                 for (auto &[f, c] : d->assoc_checks) gtk_check_button_set_active(GTK_CHECK_BUTTON(c), is_default_for(*f));
                             }),
                             d);
    gtk_box_append(GTK_BOX(b), d->entry_button);

    gtk_box_append(GTK_BOX(b), bold_label("Associate 7zipl with (current user):"));
    d->assoc_box = vbox(4);
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 16);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 2);
    const char *heads[] = {"Type", "Extensions", "Opens with now"};
    for (int i = 0; i < 3; i++) {
        GtkWidget *h = gtk_label_new(heads[i]);
        gtk_label_set_xalign(GTK_LABEL(h), 0);
        gtk_widget_add_css_class(h, "dim-label");
        gtk_grid_attach(GTK_GRID(grid), h, i, 0, 1, 1);
    }
    int row = 1;
    for (const AssocFormat &f : assoc_formats()) {
        GtkWidget *c = gtk_check_button_new_with_label(f.name.c_str());
        gtk_check_button_set_active(GTK_CHECK_BUTTON(c), is_default_for(f));
        std::string ext;
        for (const auto &e : f.extensions) ext += (ext.empty() ? "" : " ") + e;
        GtkWidget *el = gtk_label_new(ext.c_str());
        gtk_label_set_xalign(GTK_LABEL(el), 0);
        GtkWidget *cur = gtk_label_new("");
        gtk_label_set_xalign(GTK_LABEL(cur), 0);
        gtk_label_set_ellipsize(GTK_LABEL(cur), PANGO_ELLIPSIZE_END);
        gtk_widget_set_hexpand(cur, TRUE);
        std::string tip;
        for (const auto &m : f.mime_types) tip += m + "\n";
        gtk_widget_set_tooltip_text(c, trim(tip).c_str());
        gtk_grid_attach(GTK_GRID(grid), c, 0, row, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), el, 1, row, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), cur, 2, row, 1, 1);
        d->assoc_checks.emplace_back(&f, c);
        d->assoc_current.emplace_back(&f, cur);
        row++;
    }
    GtkWidget *sw = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sw), grid);
    gtk_widget_set_vexpand(sw, TRUE);
    gtk_box_append(GTK_BOX(d->assoc_box), sw);
    GtkWidget *btns = hbox(6);
    GtkWidget *all = gtk_button_new_with_label("Select archive types");
    GtkWidget *none = gtk_button_new_with_label("Clear");
    g_signal_connect_swapped(all, "clicked", CB([](Dlg *d) {
                                 for (auto &[f, c] : d->assoc_checks)
                                     gtk_check_button_set_active(GTK_CHECK_BUTTON(c), f->default_on || gtk_check_button_get_active(GTK_CHECK_BUTTON(c)));
                             }),
                             d);
    g_signal_connect_swapped(none, "clicked", CB([](Dlg *d) {
                                 for (auto &[f, c] : d->assoc_checks) gtk_check_button_set_active(GTK_CHECK_BUTTON(c), FALSE);
                             }),
                             d);
    gtk_box_append(GTK_BOX(btns), all);
    gtk_box_append(GTK_BOX(btns), none);
    gtk_box_append(GTK_BOX(d->assoc_box), btns);
    gtk_box_append(GTK_BOX(d->assoc_box),
                   note("Sets 7zipl as the default application in ~/.config/mimeapps.list, the same setting that file "
                        "managers' \"Open With\" dialogs change. Unchecking a type gives it back to the system's default."));
    gtk_box_append(GTK_BOX(b), d->assoc_box);
    refresh_entry_state(d);
    return b;
}

void apply_system(Dlg *d)
{
    std::string errors;
    for (auto &[f, c] : d->assoc_checks) {
        const bool want = gtk_check_button_get_active(GTK_CHECK_BUTTON(c));
        if (want == is_default_for(*f)) continue;
        std::string e;
        if (!set_default(*f, want, e)) errors += f->name + ": " + e + "\n";
    }
    refresh_entry_state(d);
    if (!errors.empty()) show_message(d->win, "7zipl", "Some associations couldn't be changed:\n" + errors, true);
}

// ---- 7-Zip (context menus) ----

GtkWidget *menu_page(Dlg *d)
{
    GtkWidget *b = page_box();
    gtk_box_append(GTK_BOX(b), bold_label("Integrate 7zipl to the context menu of:"));
    for (const FileManager &fm : file_managers()) {
        std::string l = fm.name + (fm.detected ? "" : "  (not found on this system)");
        GtkWidget *c = gtk_check_button_new_with_label(l.c_str());
        gtk_check_button_set_active(GTK_CHECK_BUTTON(c), fm.installed);
        gtk_box_append(GTK_BOX(b), c);
        d->fm_checks.emplace_back(fm.id, c);
    }
    const Settings &s = settings();
    d->cascaded = gtk_check_button_new_with_mnemonic("_Cascaded context menu (a \"7-Zip\" submenu)");
    gtk_check_button_set_active(GTK_CHECK_BUTTON(d->cascaded), s.ctx_cascaded);
    gtk_widget_set_margin_top(d->cascaded, 8);
    gtk_box_append(GTK_BOX(b), d->cascaded);
    d->elim_dup = gtk_check_button_new_with_mnemonic("_Eliminate duplication of root folder (Extract to folder)");
    gtk_check_button_set_active(GTK_CHECK_BUTTON(d->elim_dup), s.extract.elim_dup);
    gtk_box_append(GTK_BOX(b), d->elim_dup);

    GtkWidget *items = bold_label("Context menu items:");
    gtk_widget_set_margin_top(items, 8);
    gtk_box_append(GTK_BOX(b), items);
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 16);
    int i = 0;
    for (unsigned bit = 1; bit <= CTX_HASH; bit <<= 1, i++) {
        GtkWidget *c = gtk_check_button_new_with_label(ctx_item_label(bit));
        gtk_check_button_set_active(GTK_CHECK_BUTTON(c), (s.ctx_items & bit) != 0);
        gtk_grid_attach(GTK_GRID(grid), c, i % 2, i / 2, 1, 1);
        d->item_checks.emplace_back(bit, c);
    }
    gtk_box_append(GTK_BOX(b), grid);
    gtk_box_append(GTK_BOX(b), note("The entries run 7zipl with --extract, --extract-here, --compress and so on. "
                                    "File managers read them when they start, so restart an open file manager. "
                                    "Thunar keeps its actions in one shared file and isn't changed; add them in "
                                    "Thunar's Edit > Configure custom actions with the same commands."));
    return b;
}

void apply_menu(Dlg *d)
{
    Settings &s = settings();
    unsigned items = 0;
    for (auto &[bit, c] : d->item_checks)
        if (gtk_check_button_get_active(GTK_CHECK_BUTTON(c))) items |= bit;
    const bool cascaded = gtk_check_button_get_active(GTK_CHECK_BUTTON(d->cascaded));
    const bool changed = items != s.ctx_items || cascaded != s.ctx_cascaded;
    s.ctx_items = items;
    s.ctx_cascaded = cascaded;
    s.extract.elim_dup = gtk_check_button_get_active(GTK_CHECK_BUTTON(d->elim_dup));
    const auto fms = file_managers();
    std::string errors;
    for (auto &[id, c] : d->fm_checks) {
        const bool want = gtk_check_button_get_active(GTK_CHECK_BUTTON(c));
        bool have = false;
        for (const auto &fm : fms)
            if (fm.id == id) have = fm.installed;
        if (want == have && !(want && changed)) continue;
        std::string e;
        if (!set_context_menu(id, want ? items : 0, cascaded, e)) errors += id + ": " + e + "\n";
    }
    if (!errors.empty()) show_message(d->win, "7zipl", "Some context menus couldn't be changed:\n" + errors, true);
}

// ---- Folders ----

GtkWidget *folders_page(Dlg *d)
{
    const Settings &s = settings();
    GtkWidget *b = page_box();
    gtk_box_append(GTK_BOX(b), bold_label("Working folder"));
    gtk_box_append(GTK_BOX(b), note("Specify a location for temporary archive files: while an archive is created or "
                                    "updated, 7-Zip writes it there first and then moves it into place."));
    d->work_system = gtk_check_button_new_with_mnemonic(("_System temp folder (" + std::string(g_get_tmp_dir()) + ")").c_str());
    d->work_current = gtk_check_button_new_with_mnemonic("_Current (the archive's folder)");
    d->work_specified = gtk_check_button_new_with_mnemonic("Specified:");
    gtk_check_button_set_group(GTK_CHECK_BUTTON(d->work_current), GTK_CHECK_BUTTON(d->work_system));
    gtk_check_button_set_group(GTK_CHECK_BUTTON(d->work_specified), GTK_CHECK_BUTTON(d->work_system));
    GtkWidget *radios[] = {d->work_system, d->work_current, d->work_specified};
    gtk_check_button_set_active(GTK_CHECK_BUTTON(radios[std::clamp(s.work_mode, 0, 2)]), TRUE);
    gtk_box_append(GTK_BOX(b), d->work_system);
    gtk_box_append(GTK_BOX(b), d->work_current);
    GtkWidget *row = hbox(6);
    gtk_box_append(GTK_BOX(row), d->work_specified);
    d->work_dir = gtk_entry_new();
    gtk_editable_set_text(GTK_EDITABLE(d->work_dir), s.work_dir.c_str());
    gtk_widget_set_hexpand(d->work_dir, TRUE);
    gtk_box_append(GTK_BOX(row), d->work_dir);
    GtkWidget *browse = gtk_button_new_with_label("...");
    g_signal_connect_swapped(browse, "clicked", CB([](Dlg *d) {
                                 choose_folder(d->win, "Working folder", gtk_editable_get_text(GTK_EDITABLE(d->work_dir)),
                                               [d](const std::string &p) {
                                                   gtk_editable_set_text(GTK_EDITABLE(d->work_dir), p.c_str());
                                                   gtk_check_button_set_active(GTK_CHECK_BUTTON(d->work_specified), TRUE);
                                               });
                             }),
                             d);
    gtk_box_append(GTK_BOX(row), browse);
    gtk_box_append(GTK_BOX(b), row);
    gtk_box_append(GTK_BOX(b), note("On most Linux systems /tmp is in memory (tmpfs), so big archives are better "
                                    "written next to themselves (Current)."));
    return b;
}

void apply_folders(Dlg *d)
{
    Settings &s = settings();
    s.work_mode = gtk_check_button_get_active(GTK_CHECK_BUTTON(d->work_system))    ? 0
                  : gtk_check_button_get_active(GTK_CHECK_BUTTON(d->work_specified)) ? 2
                                                                                     : 1;
    s.work_dir = trim(gtk_editable_get_text(GTK_EDITABLE(d->work_dir)));
}

// ---- Editor ----

GtkWidget *program_row(Dlg *d, GtkWidget *b, const char *title, const std::string &value, const char *placeholder)
{
    gtk_box_append(GTK_BOX(b), label(title));
    GtkWidget *row = hbox(6);
    GtkWidget *e = gtk_entry_new();
    gtk_editable_set_text(GTK_EDITABLE(e), value.c_str());
    gtk_entry_set_placeholder_text(GTK_ENTRY(e), placeholder);
    gtk_widget_set_hexpand(e, TRUE);
    gtk_box_append(GTK_BOX(row), e);
    GtkWidget *browse = gtk_button_new_with_label("...");
    g_object_set_data(G_OBJECT(browse), "entry", e);
    g_signal_connect(browse, "clicked", CB([](GtkWidget *btn, Dlg *d) {
                         GtkWidget *e = GTK_WIDGET(g_object_get_data(G_OBJECT(btn), "entry"));
                         choose_open_files(d->win, "Program", "/usr/bin", [e](const std::vector<std::string> &f) {
                             gtk_editable_set_text(GTK_EDITABLE(e), f[0].c_str());
                         });
                     }),
                     d);
    gtk_box_append(GTK_BOX(row), browse);
    gtk_box_append(GTK_BOX(b), row);
    return e;
}

GtkWidget *editor_page(Dlg *d)
{
    const Settings &s = settings();
    GtkWidget *b = page_box();
    d->viewer = program_row(d, b, "_View (F3):", s.viewer, "the file type's default application");
    d->editor = program_row(d, b, "_Editor (F4):", s.editor, "the default application for text files");
    d->diff = program_row(d, b, "_Diff:", s.diff, "meld, kompare, kdiff3 or diffuse, whichever is installed");
    gtk_box_append(GTK_BOX(b), note("A program name or path; the file is added at the end, or where %f stands. "
                                    "Files edited from inside an archive are put back into it after you save them "
                                    "(7zipl asks first)."));
    return b;
}

void apply_editor(Dlg *d)
{
    Settings &s = settings();
    s.viewer = trim(gtk_editable_get_text(GTK_EDITABLE(d->viewer)));
    s.editor = trim(gtk_editable_get_text(GTK_EDITABLE(d->editor)));
    s.diff = trim(gtk_editable_get_text(GTK_EDITABLE(d->diff)));
}

// ---- Settings ----

GtkWidget *check(GtkWidget *b, const char *text, bool value)
{
    GtkWidget *c = gtk_check_button_new_with_mnemonic(text);
    gtk_check_button_set_active(GTK_CHECK_BUTTON(c), value);
    gtk_box_append(GTK_BOX(b), c);
    return c;
}

GtkWidget *settings_page(Dlg *d)
{
    const Settings &s = settings();
    GtkWidget *b = page_box();
    d->dotdot = check(b, "Show \"..\" item", s.show_dotdot);
    d->real_icons = check(b, "Show real file _icons", s.real_icons);
    d->grid = check(b, "Show _grid lines", s.show_grid);
    d->single_click = check(b, "_Single-click to open an item", s.single_click);
    d->hidden = check(b, "Show _hidden files", s.show_hidden);
    GtkWidget *row = hbox(6);
    d->mem_limit = gtk_check_button_new_with_mnemonic("Maximum amount of RAM memory usage allowed to unpack archives:");
    gtk_check_button_set_active(GTK_CHECK_BUTTON(d->mem_limit), s.mem_limit);
    d->mem_gb = gtk_spin_button_new_with_range(1, 1024, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(d->mem_gb), s.mem_limit_gb);
    gtk_box_append(GTK_BOX(row), d->mem_limit);
    gtk_box_append(GTK_BOX(row), d->mem_gb);
    gtk_box_append(GTK_BOX(row), label("GB"));
    gtk_widget_set_margin_top(row, 8);
    gtk_box_append(GTK_BOX(b), row);
    gtk_box_append(GTK_BOX(b), note("Archives that need more memory than this (RAR 7 archives with dictionaries over "
                                    "1 GB) ask before they are unpacked."));
    gtk_box_append(GTK_BOX(b), note("Settings are stored in " + config_dir() + "/7zipl.conf."));
    return b;
}

void apply_settings(Dlg *d)
{
    Settings &s = settings();
    s.show_dotdot = gtk_check_button_get_active(GTK_CHECK_BUTTON(d->dotdot));
    s.real_icons = gtk_check_button_get_active(GTK_CHECK_BUTTON(d->real_icons));
    s.show_grid = gtk_check_button_get_active(GTK_CHECK_BUTTON(d->grid));
    s.single_click = gtk_check_button_get_active(GTK_CHECK_BUTTON(d->single_click));
    s.show_hidden = gtk_check_button_get_active(GTK_CHECK_BUTTON(d->hidden));
    s.mem_limit = gtk_check_button_get_active(GTK_CHECK_BUTTON(d->mem_limit));
    s.mem_limit_gb = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(d->mem_gb));
}

void apply_all(Dlg *d)
{
    apply_system(d);
    apply_menu(d);
    apply_folders(d);
    apply_editor(d);
    apply_settings(d);
    settings().save();
    if (d->applied) d->applied();
}

}  // namespace

void options_dialog(GtkWindow *parent, std::function<void()> applied, int page)
{
    auto *d = new Dlg();
    d->applied = std::move(applied);
    GtkWidget *w = dialog_window(parent, "Options", 620, 560);
    d->win = GTK_WINDOW(w);
    g_object_set_data_full(G_OBJECT(w), "dlg", d, [](gpointer p) { delete static_cast<Dlg *>(p); });
    GtkWidget *box = vbox(6);
    set_margins(box, 8);
    GtkWidget *nb = gtk_notebook_new();
    gtk_widget_set_vexpand(nb, TRUE);
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), system_page(d), gtk_label_new("System"));
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), menu_page(d), gtk_label_new("7-Zip"));
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), folders_page(d), gtk_label_new("Folders"));
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), editor_page(d), gtk_label_new("Editor"));
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), settings_page(d), gtk_label_new("Settings"));
    gtk_notebook_set_current_page(GTK_NOTEBOOK(nb), page);
    gtk_box_append(GTK_BOX(box), nb);
    auto b = button_row(box, {"OK", "Cancel", "_Apply"});
    g_signal_connect_swapped(b[0], "clicked", CB([](Dlg *d) {
                                 apply_all(d);
                                 gtk_window_destroy(d->win);
                             }),
                             d);
    g_signal_connect_swapped(b[1], "clicked", G_CALLBACK(gtk_window_destroy), w);
    g_signal_connect_swapped(b[2], "clicked", CB([](Dlg *d) { apply_all(d); }), d);
    gtk_window_set_child(GTK_WINDOW(w), box);
    gtk_window_present(GTK_WINDOW(w));
}

}  // namespace szl
