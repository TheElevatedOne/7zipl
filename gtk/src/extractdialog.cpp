// SPDX-License-Identifier: LGPL-2.1-or-later
#include "extractdialog.hpp"

#include "gtkutil.hpp"
#include "settings.hpp"
#include "util.hpp"

#include <algorithm>

namespace szl {

namespace {

struct Dlg {
    GtkWidget *win, *dir, *history, *subfolder, *pathmode, *overwrite, *elim, *pw, *pw_show;
    std::string base_dir;
    std::vector<std::string> archives;
    bool in_archive = false;
    std::function<void(const ExtractChoice &)> cb;
};

// The folder name 7-Zip proposes: the archive name without its extension, or "*" for several archives.
std::string subfolder_name(const Dlg *d)
{
    if (d->archives.size() != 1) return "*";
    return strip_archive_ext(path_basename(d->archives[0]));
}

void on_ok(Dlg *d)
{
    ExtractChoice c;
    std::string dir = trim(gtk_editable_get_text(GTK_EDITABLE(d->dir)));
    if (dir.empty()) dir = d->base_dir;
    if (dir[0] == '~') dir = home_dir() + dir.substr(1);
    if (dir[0] != '/') dir = path_join(d->base_dir, dir);
    while (dir.size() > 1 && dir.back() == '/') dir.pop_back();
    Settings &s = settings();
    s.push_history(s.extract_history, dir);
    const bool sub = d->subfolder && gtk_check_button_get_active(GTK_CHECK_BUTTON(d->subfolder));
    if (sub) dir = path_join(dir, subfolder_name(d));
    c.out_dir = dir;
    c.path_mode = (int)gtk_drop_down_get_selected(GTK_DROP_DOWN(d->pathmode));
    c.overwrite = (int)gtk_drop_down_get_selected(GTK_DROP_DOWN(d->overwrite));
    c.elim_dup = d->elim && gtk_check_button_get_active(GTK_CHECK_BUTTON(d->elim));
    c.password = gtk_editable_get_text(GTK_EDITABLE(d->pw));
    s.extract.path_mode = c.path_mode;
    s.extract.overwrite = c.overwrite;
    if (d->subfolder) s.extract.subfolder = sub;
    if (d->elim) s.extract.elim_dup = c.elim_dup;
    s.save();
    auto cb = d->cb;
    gtk_window_destroy(GTK_WINDOW(d->win));
    cb(c);
}

}  // namespace

void extract_dialog(GtkWindow *parent, const std::string &base_dir, const std::vector<std::string> &archives,
                    bool in_archive, std::function<void(const ExtractChoice &)> cb)
{
    auto *d = new Dlg();
    d->base_dir = base_dir;
    d->archives = archives;
    d->in_archive = in_archive;
    d->cb = std::move(cb);
    const ExtractDefaults &ed = settings().extract;

    std::string title = "Extract";
    if (archives.size() == 1) title += " : " + path_basename(archives[0]);
    d->win = dialog_window(parent, title, 560, -1);
    g_object_set_data_full(G_OBJECT(d->win), "dlg", d, [](gpointer p) { delete static_cast<Dlg *>(p); });
    GtkWidget *box = vbox(8);
    set_margins(box, 12);

    GtkWidget *l = label("E_xtract to:");
    gtk_box_append(GTK_BOX(box), l);
    GtkWidget *row = hbox(6);
    d->dir = gtk_entry_new();
    gtk_label_set_mnemonic_widget(GTK_LABEL(l), d->dir);
    gtk_widget_set_hexpand(d->dir, TRUE);
    gtk_editable_set_text(GTK_EDITABLE(d->dir), (base_dir == "/" ? base_dir : base_dir + "/").c_str());
    gtk_entry_set_activates_default(GTK_ENTRY(d->dir), TRUE);
    gtk_box_append(GTK_BOX(row), d->dir);
    // History of destination folders
    std::vector<std::string> hist = {"History"};
    for (const auto &h : settings().extract_history) hist.push_back(h);
    d->history = dropdown(hist);
    gtk_widget_set_sensitive(d->history, hist.size() > 1);
    gtk_widget_set_size_request(d->history, 90, -1);
    g_signal_connect(d->history, "notify::selected", CB([](GtkWidget *dd, GParamSpec *, Dlg *d) {
                         if (gtk_drop_down_get_selected(GTK_DROP_DOWN(dd)) == 0) return;
                         gtk_editable_set_text(GTK_EDITABLE(d->dir), dropdown_text(dd).c_str());
                     }),
                     d);
    gtk_box_append(GTK_BOX(row), d->history);
    GtkWidget *browse = gtk_button_new_with_label("...");
    g_signal_connect_swapped(browse, "clicked", CB([](Dlg *d) {
                                 std::string cur = gtk_editable_get_text(GTK_EDITABLE(d->dir));
                                 choose_folder(GTK_WINDOW(d->win), "Extract to", cur, [d](const std::string &p) {
                                     gtk_editable_set_text(GTK_EDITABLE(d->dir), (p + "/").c_str());
                                 });
                             }),
                             d);
    gtk_box_append(GTK_BOX(row), browse);
    gtk_box_append(GTK_BOX(box), row);

    if (!in_archive) {
        const std::string sub = subfolder_name(d);
        d->subfolder = gtk_check_button_new_with_label(
            (sub == "*" ? std::string("Extract each archive to its own folder") : "Extract into folder \"" + sub + "\"").c_str());
        gtk_check_button_set_active(GTK_CHECK_BUTTON(d->subfolder), ed.subfolder);
        gtk_box_append(GTK_BOX(box), d->subfolder);
    }

    GtkWidget *cols = hbox(16);
    GtkWidget *left = vbox(6);
    gtk_widget_set_hexpand(left, TRUE);
    GtkWidget *pl = label("_Path mode:");
    d->pathmode = dropdown({"Full pathnames", "No pathnames", "Absolute pathnames"},
                           (unsigned)std::clamp(ed.path_mode, 0, 2));
    gtk_label_set_mnemonic_widget(GTK_LABEL(pl), d->pathmode);
    gtk_box_append(GTK_BOX(left), pl);
    gtk_box_append(GTK_BOX(left), d->pathmode);
    if (!in_archive) {
        d->elim = gtk_check_button_new_with_mnemonic("_Eliminate duplication of root folder");
        gtk_check_button_set_active(GTK_CHECK_BUTTON(d->elim), ed.elim_dup);
        gtk_box_append(GTK_BOX(left), d->elim);
    }
    GtkWidget *ol = label("_Overwrite mode:");
    d->overwrite = dropdown({"Ask before overwrite", "Overwrite without prompt", "Skip existing files",
                             "Auto rename", "Auto rename existing files"},
                            (unsigned)std::clamp(ed.overwrite, 0, 4));
    gtk_label_set_mnemonic_widget(GTK_LABEL(ol), d->overwrite);
    gtk_box_append(GTK_BOX(left), ol);
    gtk_box_append(GTK_BOX(left), d->overwrite);
    gtk_box_append(GTK_BOX(cols), left);

    GtkWidget *right = vbox(6);
    gtk_widget_set_hexpand(right, TRUE);
    d->pw = gtk_entry_new();
    gtk_entry_set_visibility(GTK_ENTRY(d->pw), FALSE);
    gtk_entry_set_input_purpose(GTK_ENTRY(d->pw), GTK_INPUT_PURPOSE_PASSWORD);
    gtk_entry_set_activates_default(GTK_ENTRY(d->pw), TRUE);
    gtk_box_append(GTK_BOX(right), d->pw);
    d->pw_show = gtk_check_button_new_with_mnemonic("_Show Password");
    g_signal_connect(d->pw_show, "toggled", CB([](GtkCheckButton *b, Dlg *d) {
                         gtk_entry_set_visibility(GTK_ENTRY(d->pw), gtk_check_button_get_active(b));
                     }),
                     d);
    gtk_box_append(GTK_BOX(right), d->pw_show);
    gtk_box_append(GTK_BOX(cols), frame("Password", right));
    gtk_box_append(GTK_BOX(box), cols);

    auto b = button_row(box, {"OK", "Cancel"});
    g_signal_connect_swapped(b[0], "clicked", CB([](Dlg *d) { on_ok(d); }), d);
    g_signal_connect_swapped(b[1], "clicked", G_CALLBACK(gtk_window_destroy), d->win);
    gtk_window_set_child(GTK_WINDOW(d->win), box);
    gtk_window_set_default_widget(GTK_WINDOW(d->win), b[0]);
    gtk_window_present(GTK_WINDOW(d->win));
    gtk_widget_grab_focus(d->dir);
}

std::vector<std::string> extract_args(const ExtractChoice &c, const std::vector<std::string> &archives, bool test)
{
    std::vector<std::string> a;
    if (test) {
        a.push_back("t");
    } else {
        a.push_back(c.path_mode == 1 ? "e" : "x");
        a.push_back("-o" + c.out_dir);
        if (c.path_mode == 2) a.push_back("-spf");
        static const char *const ao[] = {nullptr, "-aoa", "-aos", "-aou", "-aot"};
        if (c.overwrite > 0 && c.overwrite < 5) a.push_back(ao[c.overwrite]);
        if (c.elim_dup) a.push_back("-spe");
    }
    if (!c.password.empty()) a.push_back("-p" + c.password);
    a.push_back("-spd");  // names are literal, not wildcards
    a.push_back("--");
    for (const auto &f : archives) a.push_back(f);
    return a;
}

}  // namespace szl
