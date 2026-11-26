// SPDX-License-Identifier: LGPL-2.1-or-later
#include "dialogs.hpp"

#include "gtkutil.hpp"
#include "util.hpp"

namespace szl {

namespace {

struct PwState {
    std::function<void(bool, const std::string &)> cb;
    GtkWidget *win, *e1, *e2, *show, *err;
    bool answered = false;
    bool for_new;
};

void pw_answer(PwState *s, bool ok)
{
    if (s->answered) return;
    std::string pw;
    if (ok) {
        pw = gtk_editable_get_text(GTK_EDITABLE(s->e1));
        if (s->for_new && pw != gtk_editable_get_text(GTK_EDITABLE(s->e2))) {
            gtk_label_set_text(GTK_LABEL(s->err), "Passwords do not match");
            gtk_widget_set_visible(s->err, TRUE);
            return;
        }
    }
    s->answered = true;
    auto cb = s->cb;
    gtk_window_destroy(GTK_WINDOW(s->win));
    cb(ok, pw);
}

}  // namespace

void password_dialog(GtkWindow *parent, const std::string &archive, bool for_new,
                     std::function<void(bool, const std::string &)> cb)
{
    GtkWidget *w = dialog_window(parent, "Enter password", 380, -1);
    GtkWidget *box = vbox(6);
    set_margins(box, 12);
    if (!archive.empty()) {
        GtkWidget *a = gtk_label_new(path_basename(archive).c_str());
        gtk_label_set_xalign(GTK_LABEL(a), 0);
        gtk_label_set_ellipsize(GTK_LABEL(a), PANGO_ELLIPSIZE_MIDDLE);
        gtk_widget_add_css_class(a, "dim-label");
        gtk_box_append(GTK_BOX(box), a);
    }
    auto *s = new PwState{std::move(cb), w, nullptr, nullptr, nullptr, nullptr, false, for_new};
    s->e1 = gtk_entry_new();
    gtk_entry_set_visibility(GTK_ENTRY(s->e1), FALSE);
    gtk_entry_set_input_purpose(GTK_ENTRY(s->e1), GTK_INPUT_PURPOSE_PASSWORD);
    GtkWidget *l1 = label("_Enter password:");
    gtk_label_set_mnemonic_widget(GTK_LABEL(l1), s->e1);
    gtk_box_append(GTK_BOX(box), l1);
    gtk_box_append(GTK_BOX(box), s->e1);
    s->e2 = gtk_entry_new();
    gtk_entry_set_visibility(GTK_ENTRY(s->e2), FALSE);
    gtk_entry_set_input_purpose(GTK_ENTRY(s->e2), GTK_INPUT_PURPOSE_PASSWORD);
    if (for_new) {
        GtkWidget *l2 = label("_Reenter password:");
        gtk_label_set_mnemonic_widget(GTK_LABEL(l2), s->e2);
        gtk_box_append(GTK_BOX(box), l2);
        gtk_box_append(GTK_BOX(box), s->e2);
    }
    s->show = gtk_check_button_new_with_mnemonic("_Show password");
    gtk_box_append(GTK_BOX(box), s->show);
    g_signal_connect(s->show, "toggled", CB([](GtkCheckButton *c, PwState *s) {
                         const bool v = gtk_check_button_get_active(c);
                         gtk_entry_set_visibility(GTK_ENTRY(s->e1), v);
                         gtk_entry_set_visibility(GTK_ENTRY(s->e2), v);
                         gtk_widget_set_visible(s->e2, !v && s->for_new);
                     }),
                     s);
    s->err = label("");
    gtk_widget_add_css_class(s->err, "szl-warning");
    gtk_widget_set_visible(s->err, FALSE);
    gtk_box_append(GTK_BOX(box), s->err);
    gtk_entry_set_activates_default(GTK_ENTRY(s->e1), TRUE);
    gtk_entry_set_activates_default(GTK_ENTRY(s->e2), TRUE);
    auto b = button_row(box, {"OK", "Cancel"});
    g_signal_connect_swapped(b[0], "clicked", CB([](PwState *s) { pw_answer(s, true); }), s);
    g_signal_connect_swapped(b[1], "clicked", CB([](PwState *s) { pw_answer(s, false); }), s);
    // Closing the window = Cancel; the state is freed with the window.
    g_signal_connect_swapped(w, "close-request", CB([](PwState *s) -> gboolean {
                                 pw_answer(s, false);
                                 return TRUE;
                             }),
                             s);
    g_object_set_data_full(G_OBJECT(w), "state", s, [](gpointer p) {
        auto *s = static_cast<PwState *>(p);
        if (!s->answered) {
            s->answered = true;
            s->cb(false, {});
        }
        delete s;
    });
    gtk_window_set_child(GTK_WINDOW(w), box);
    gtk_window_set_default_widget(GTK_WINDOW(w), b[0]);
    gtk_window_present(GTK_WINDOW(w));
    gtk_widget_grab_focus(s->e1);
}

namespace {
struct OwState {
    std::function<void(Ui::Overwrite)> cb;
    GtkWidget *win;
    bool answered = false;
};
void ow_answer(OwState *s, Ui::Overwrite a)
{
    if (s->answered) return;
    s->answered = true;
    auto cb = s->cb;
    gtk_window_destroy(GTK_WINDOW(s->win));
    cb(a);
}
GtkWidget *file_info(const std::string &name, uint64_t size, int64_t mtime)
{
    GtkWidget *row = hbox(10);
    GIcon *gi = nullptr;
    char *ct = g_content_type_guess(name.c_str(), nullptr, 0, nullptr);
    if (ct) {
        gi = g_content_type_get_icon(ct);
        g_free(ct);
    }
    GtkWidget *icon = gi ? gtk_image_new_from_gicon(gi) : gtk_image_new_from_icon_name("text-x-generic");
    if (gi) g_object_unref(gi);
    gtk_image_set_icon_size(GTK_IMAGE(icon), GTK_ICON_SIZE_LARGE);
    gtk_box_append(GTK_BOX(row), icon);
    std::string text = name + "\n" + format_number(size) + " bytes";
    if (mtime) text += "\nmodified on " + format_time(mtime);
    GtkWidget *l = gtk_label_new(text.c_str());
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_label_set_wrap(GTK_LABEL(l), TRUE);
    gtk_label_set_wrap_mode(GTK_LABEL(l), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_selectable(GTK_LABEL(l), TRUE);
    gtk_widget_set_hexpand(l, TRUE);
    gtk_box_append(GTK_BOX(row), l);
    gtk_widget_set_margin_start(row, 18);
    return row;
}
}  // namespace

void overwrite_dialog(GtkWindow *parent, const std::string &existing, uint64_t es, int64_t et,
                      const std::string &incoming, uint64_t is, int64_t it, std::function<void(Ui::Overwrite)> cb)
{
    GtkWidget *w = dialog_window(parent, "Confirm File Replace", 520, -1);
    GtkWidget *box = vbox(8);
    set_margins(box, 12);
    gtk_box_append(GTK_BOX(box), label("Destination folder already contains processed file."));
    gtk_box_append(GTK_BOX(box), label("Would you like to replace the existing file"));
    gtk_box_append(GTK_BOX(box), file_info(existing, es, et));
    gtk_box_append(GTK_BOX(box), label("with this one?"));
    gtk_box_append(GTK_BOX(box), file_info(incoming, is, it));
    auto *s = new OwState{std::move(cb), w};
    g_object_set_data_full(G_OBJECT(w), "state", s, [](gpointer p) {
        auto *s = static_cast<OwState *>(p);
        if (!s->answered) {
            s->answered = true;
            s->cb(Ui::Overwrite::Cancel);
        }
        delete s;
    });
    auto b = button_row(box, {"_Yes", "Yes to _All", "_No", "No to A_ll", "A_uto Rename", "Cancel"});
    static const Ui::Overwrite answers[] = {Ui::Overwrite::Yes, Ui::Overwrite::YesAll, Ui::Overwrite::No,
                                            Ui::Overwrite::NoAll, Ui::Overwrite::Rename, Ui::Overwrite::Cancel};
    for (size_t i = 0; i < b.size(); i++) {
        gtk_widget_set_size_request(b[i], -1, -1);
        g_object_set_data(G_OBJECT(b[i]), "answer", GINT_TO_POINTER((int)answers[i]));
        g_signal_connect(b[i], "clicked", CB([](GtkButton *btn, OwState *s) {
                             ow_answer(s, (Ui::Overwrite)GPOINTER_TO_INT(g_object_get_data(G_OBJECT(btn), "answer")));
                         }),
                         s);
    }
    gtk_window_set_child(GTK_WINDOW(w), box);
    gtk_window_set_default_widget(GTK_WINDOW(w), b[0]);
    gtk_window_present(GTK_WINDOW(w));
}

void properties_window(GtkWindow *parent, const std::string &title,
                       const std::vector<std::pair<std::string, std::string>> &rows)
{
    GtkWidget *w = dialog_window(parent, title, 480, 420);
    gtk_window_set_modal(GTK_WINDOW(w), FALSE);
    GtkWidget *box = vbox(6);
    set_margins(box, 12);
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 16);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 3);
    int r = 0;
    for (const auto &[k, v] : rows) {
        if (k == "--") {  // section separator (nested archive level)
            GtkWidget *sep = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
            gtk_widget_set_margin_top(sep, 6);
            gtk_grid_attach(GTK_GRID(grid), sep, 0, r++, 2, 1);
            GtkWidget *l = gtk_label_new(v.c_str());
            gtk_label_set_xalign(GTK_LABEL(l), 0);
            gtk_widget_add_css_class(l, "heading");
            gtk_grid_attach(GTK_GRID(grid), l, 0, r++, 2, 1);
            continue;
        }
        GtkWidget *kl = gtk_label_new(k.c_str());
        gtk_label_set_xalign(GTK_LABEL(kl), 0);
        gtk_widget_set_valign(kl, GTK_ALIGN_START);
        gtk_widget_add_css_class(kl, "dim-label");
        GtkWidget *vl = gtk_label_new(v.c_str());
        gtk_label_set_xalign(GTK_LABEL(vl), 0);
        gtk_label_set_selectable(GTK_LABEL(vl), TRUE);
        gtk_label_set_wrap(GTK_LABEL(vl), TRUE);
        gtk_label_set_wrap_mode(GTK_LABEL(vl), PANGO_WRAP_WORD_CHAR);
        gtk_widget_set_hexpand(vl, TRUE);
        gtk_grid_attach(GTK_GRID(grid), kl, 0, r, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), vl, 1, r, 1, 1);
        r++;
    }
    GtkWidget *sw = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sw), grid);
    gtk_widget_set_vexpand(sw, TRUE);
    gtk_box_append(GTK_BOX(box), sw);
    auto b = button_row(box, {"OK"});
    g_signal_connect_swapped(b[0], "clicked", G_CALLBACK(gtk_window_destroy), w);
    gtk_window_set_child(GTK_WINDOW(w), box);
    gtk_window_set_default_widget(GTK_WINDOW(w), b[0]);
    gtk_window_present(GTK_WINDOW(w));
}

void hash_window(GtkWindow *parent, const HashResult &h)
{
    std::string methods;
    for (const auto &m : h.methods) methods += (methods.empty() ? "" : " ") + m;
    GtkWidget *w = dialog_window(parent, "Checksum information", 640, 420);
    gtk_window_set_modal(GTK_WINDOW(w), FALSE);
    GtkWidget *box = vbox(6);
    set_margins(box, 12);

    std::string text;
    const bool single = h.files.size() == 1 && !h.files[0].dir;
    if (!single) {
        for (const auto &f : h.files) {
            if (f.dir) continue;
            for (size_t i = 0; i < f.digests.size(); i++) text += f.digests[i] + "  ";
            text += f.name + "\n";
        }
        text += "\n";
    }
    text += "Name: " + (single ? h.files[0].name : std::string()) + "\n";
    text += "Folders: " + format_number(h.num_dirs) + "\n";
    text += "Files: " + format_number(h.num_files) + "\n";
    text += "Size: " + format_number(h.total_size) + "\n";
    for (size_t i = 0; i < h.methods.size(); i++) {
        if (single && i < h.files[0].digests.size())
            text += h.methods[i] + ": " + h.files[0].digests[i] + "\n";
        else if (i < h.totals.size())
            text += h.methods[i] + " for data: " + h.totals[i] + "\n";
    }
    if (h.errors) text += "Errors: " + format_number(h.errors) + "\n";

    GtkWidget *tv = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(tv), FALSE);
    gtk_widget_add_css_class(tv, "szl-hash");
    gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(tv)), text.c_str(), -1);
    GtkWidget *sw = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sw), tv);
    gtk_widget_set_vexpand(sw, TRUE);
    gtk_box_append(GTK_BOX(box), sw);
    auto b = button_row(box, {"_Copy", "OK"}, 1);
    g_object_set_data_full(G_OBJECT(b[0]), "text", g_strdup(text.c_str()), g_free);
    g_signal_connect(b[0], "clicked", CB([](GtkWidget *btn) {
                         copy_to_clipboard(btn, (const char *)g_object_get_data(G_OBJECT(btn), "text"));
                     }),
                     nullptr);
    g_signal_connect_swapped(b[1], "clicked", G_CALLBACK(gtk_window_destroy), w);
    gtk_window_set_child(GTK_WINDOW(w), box);
    gtk_window_set_default_widget(GTK_WINDOW(w), b[1]);
    gtk_window_present(GTK_WINDOW(w));
}

void about_dialog(GtkWindow *parent)
{
    GtkWidget *w = dialog_window(parent, "About 7zipl", 420, -1);
    GtkWidget *box = vbox(8);
    set_margins(box, 16);
    GtkWidget *icon = gtk_image_new_from_icon_name(SZL_APP_ID);
    gtk_image_set_pixel_size(GTK_IMAGE(icon), 64);
    gtk_box_append(GTK_BOX(box), icon);
    GtkWidget *t = gtk_label_new(nullptr);
    std::string markup = "<b><big>7zipl " SZL_VERSION "</big></b>\n7-Zip engine " + version();
    gtk_label_set_markup(GTK_LABEL(t), markup.c_str());
    gtk_label_set_justify(GTK_LABEL(t), GTK_JUSTIFY_CENTER);
    gtk_box_append(GTK_BOX(box), t);
    GtkWidget *d = gtk_label_new(
        "7-Zip File Manager for Linux, with a native GTK 4 interface.\n\n"
        "7-Zip: Copyright (c) 1999-2026 Igor Pavlov.\n"
        "7-Zip is free software (GNU LGPL; parts under the BSD 3-clause license\n"
        "and the unRAR license restriction). See /usr/share/doc/7zipl.\n\n"
        "7zipl is not made or endorsed by the 7-Zip author.");
    gtk_label_set_justify(GTK_LABEL(d), GTK_JUSTIFY_CENTER);
    gtk_label_set_wrap(GTK_LABEL(d), TRUE);
    gtk_box_append(GTK_BOX(box), d);
    GtkWidget *link = gtk_link_button_new_with_label("https://github.com/TheElevatedOne/7zipl", "github.com/TheElevatedOne/7zipl");
    gtk_box_append(GTK_BOX(box), link);
    auto b = button_row(box, {"OK"});
    gtk_widget_set_halign(gtk_widget_get_parent(b[0]), GTK_ALIGN_CENTER);
    g_signal_connect_swapped(b[0], "clicked", G_CALLBACK(gtk_window_destroy), w);
    gtk_window_set_child(GTK_WINDOW(w), box);
    gtk_window_present(GTK_WINDOW(w));
}

}  // namespace szl
