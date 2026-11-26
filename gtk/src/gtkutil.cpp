// SPDX-License-Identifier: LGPL-2.1-or-later
#include "gtkutil.hpp"

namespace szl {

void use_window_class()
{
    // GDK uses the program name for X11's WM_CLASS and the app ID for Wayland; keep "7zipl" for X11.
    g_set_prgname(kWindowClass);
}

namespace {
struct MainFn {
    std::function<void()> fn;
};
gboolean main_fn_cb(gpointer p)
{
    auto *m = static_cast<MainFn *>(p);
    m->fn();
    delete m;
    return G_SOURCE_REMOVE;
}
}  // namespace

void run_on_main(std::function<void()> fn)
{
    g_main_context_invoke(nullptr, main_fn_cb, new MainFn{std::move(fn)});
}

GtkWidget *vbox(int spacing) { return gtk_box_new(GTK_ORIENTATION_VERTICAL, spacing); }
GtkWidget *hbox(int spacing) { return gtk_box_new(GTK_ORIENTATION_HORIZONTAL, spacing); }

GtkWidget *label(const std::string &text, float xalign)
{
    GtkWidget *l = gtk_label_new_with_mnemonic(text.c_str());
    gtk_label_set_xalign(GTK_LABEL(l), xalign);
    return l;
}

GtkWidget *bold_label(const std::string &text)
{
    GtkWidget *l = label(text);
    gtk_widget_add_css_class(l, "heading");
    return l;
}

GtkWidget *frame(const std::string &title, GtkWidget *child)
{
    GtkWidget *f = gtk_frame_new(title.empty() ? nullptr : title.c_str());
    set_margins(child, 6);
    gtk_frame_set_child(GTK_FRAME(f), child);
    return f;
}

void set_margins(GtkWidget *w, int m)
{
    gtk_widget_set_margin_start(w, m);
    gtk_widget_set_margin_end(w, m);
    gtk_widget_set_margin_top(w, m);
    gtk_widget_set_margin_bottom(w, m);
}

GtkWidget *dropdown(const std::vector<std::string> &items, unsigned selected)
{
    GtkWidget *dd = gtk_drop_down_new(G_LIST_MODEL(gtk_string_list_new(nullptr)), nullptr);
    dropdown_set_items(dd, items, selected);
    return dd;
}

void dropdown_set_items(GtkWidget *dd, const std::vector<std::string> &items, unsigned selected)
{
    GtkStringList *sl = gtk_string_list_new(nullptr);
    for (const auto &s : items) gtk_string_list_append(sl, s.c_str());
    gtk_drop_down_set_model(GTK_DROP_DOWN(dd), G_LIST_MODEL(sl));
    g_object_unref(sl);
    if (!items.empty()) gtk_drop_down_set_selected(GTK_DROP_DOWN(dd), selected < items.size() ? selected : 0);
}

std::string dropdown_text(GtkWidget *dd)
{
    GObject *o = G_OBJECT(gtk_drop_down_get_selected_item(GTK_DROP_DOWN(dd)));
    if (!o || !GTK_IS_STRING_OBJECT(o)) return {};
    return gtk_string_object_get_string(GTK_STRING_OBJECT(o));
}

bool dropdown_select(GtkWidget *dd, const std::string &text)
{
    GListModel *m = gtk_drop_down_get_model(GTK_DROP_DOWN(dd));
    const guint n = g_list_model_get_n_items(m);
    for (guint i = 0; i < n; i++) {
        GtkStringObject *o = GTK_STRING_OBJECT(g_list_model_get_item(m, i));
        const bool eq = text == gtk_string_object_get_string(o);
        g_object_unref(o);
        if (eq) {
            gtk_drop_down_set_selected(GTK_DROP_DOWN(dd), i);
            return true;
        }
    }
    return false;
}

GtkWidget *dialog_window(GtkWindow *parent, const std::string &title, int width, int height)
{
    GtkWidget *w = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(w), title.c_str());
    if (parent) {
        gtk_window_set_transient_for(GTK_WINDOW(w), parent);
        gtk_window_set_modal(GTK_WINDOW(w), TRUE);
        GtkApplication *app = gtk_window_get_application(parent);
        if (app) gtk_window_set_application(GTK_WINDOW(w), app);
    } else if (GTK_IS_APPLICATION(g_application_get_default())) {
        // No main window (file-manager actions): the window keeps the application running.
        gtk_window_set_application(GTK_WINDOW(w), GTK_APPLICATION(g_application_get_default()));
    }
    gtk_window_set_default_size(GTK_WINDOW(w), width, height);
    gtk_window_set_hide_on_close(GTK_WINDOW(w), FALSE);
    GtkEventController *k = gtk_shortcut_controller_new();
    gtk_shortcut_controller_add_shortcut(
        GTK_SHORTCUT_CONTROLLER(k),
        gtk_shortcut_new(gtk_keyval_trigger_new(GDK_KEY_Escape, GdkModifierType(0)), gtk_named_action_new("window.close")));
    gtk_widget_add_controller(w, k);
    return w;
}

std::vector<GtkWidget *> button_row(GtkWidget *box, const std::vector<std::string> &labels, int default_index)
{
    GtkWidget *row = hbox(6);
    gtk_widget_set_halign(row, GTK_ALIGN_END);
    gtk_widget_set_margin_top(row, 6);
    std::vector<GtkWidget *> out;
    for (size_t i = 0; i < labels.size(); i++) {
        GtkWidget *b = gtk_button_new_with_mnemonic(labels[i].c_str());
        gtk_widget_set_size_request(b, 90, -1);
        if ((int)i == default_index) gtk_widget_add_css_class(b, "suggested-action");
        gtk_box_append(GTK_BOX(row), b);
        out.push_back(b);
    }
    gtk_box_append(GTK_BOX(box), row);
    return out;
}

void show_message(GtkWindow *parent, const std::string &title, const std::string &text, bool error)
{
    GtkWidget *w = dialog_window(parent, title, 420, -1);
    GtkWidget *box = vbox(12);
    set_margins(box, 12);
    GtkWidget *row = hbox(12);
    GtkWidget *icon = gtk_image_new_from_icon_name(error ? "dialog-error" : "dialog-information");
    gtk_image_set_icon_size(GTK_IMAGE(icon), GTK_ICON_SIZE_LARGE);
    gtk_widget_set_valign(icon, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(row), icon);
    GtkWidget *l = label(text);
    gtk_label_set_use_underline(GTK_LABEL(l), FALSE);
    gtk_label_set_text(GTK_LABEL(l), text.c_str());
    gtk_label_set_wrap(GTK_LABEL(l), TRUE);
    gtk_label_set_selectable(GTK_LABEL(l), TRUE);
    gtk_widget_set_hexpand(l, TRUE);
    gtk_box_append(GTK_BOX(row), l);
    gtk_box_append(GTK_BOX(box), row);
    auto b = button_row(box, {"OK"});
    g_signal_connect_swapped(b[0], "clicked", G_CALLBACK(gtk_window_destroy), w);
    gtk_window_set_child(GTK_WINDOW(w), box);
    gtk_window_set_default_widget(GTK_WINDOW(w), b[0]);
    gtk_window_present(GTK_WINDOW(w));
}

namespace {
struct Ask {
    std::function<void(bool)> cb;
    GtkWidget *win;
    bool answered = false;
};
}  // namespace

void ask_yes_no(GtkWindow *parent, const std::string &title, const std::string &text, std::function<void(bool)> cb)
{
    GtkWidget *w = dialog_window(parent, title, 400, -1);
    GtkWidget *box = vbox(12);
    set_margins(box, 12);
    GtkWidget *row = hbox(12);
    GtkWidget *icon = gtk_image_new_from_icon_name("dialog-question");
    gtk_image_set_icon_size(GTK_IMAGE(icon), GTK_ICON_SIZE_LARGE);
    gtk_widget_set_valign(icon, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(row), icon);
    GtkWidget *l = gtk_label_new(text.c_str());
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_label_set_wrap(GTK_LABEL(l), TRUE);
    gtk_widget_set_hexpand(l, TRUE);
    gtk_box_append(GTK_BOX(row), l);
    gtk_box_append(GTK_BOX(box), row);
    auto b = button_row(box, {"_Yes", "_No"});
    auto *a = new Ask{std::move(cb), w};
    // No, Escape and closing the window all answer false, so callers waiting for an answer always get one.
    g_object_set_data_full(G_OBJECT(w), "ask", a, [](gpointer p) {
        auto *a = static_cast<Ask *>(p);
        auto cb = std::move(a->cb);
        const bool answered = a->answered;
        delete a;
        if (!answered && cb) cb(false);
    });
    g_signal_connect(b[0], "clicked", CB([](GtkButton *, gpointer p) {
                         auto *a = static_cast<Ask *>(p);
                         auto cb = a->cb;
                         a->answered = true;
                         gtk_window_destroy(GTK_WINDOW(a->win));
                         cb(true);
                     }),
                     a);
    g_signal_connect_swapped(b[1], "clicked", G_CALLBACK(gtk_window_destroy), w);
    gtk_window_set_child(GTK_WINDOW(w), box);
    gtk_window_set_default_widget(GTK_WINDOW(w), b[0]);
    gtk_window_present(GTK_WINDOW(w));
}

namespace {
struct AskText {
    std::function<void(const std::string &)> cb;
    GtkWidget *win;
    GtkWidget *entry;
};
void ask_text_ok(GtkWidget *, gpointer p)
{
    auto *a = static_cast<AskText *>(p);
    std::string text = gtk_editable_get_text(GTK_EDITABLE(a->entry));
    auto cb = a->cb;
    gtk_window_destroy(GTK_WINDOW(a->win));
    cb(text);
}
}  // namespace

void ask_text(GtkWindow *parent, const std::string &title, const std::string &prompt, const std::string &initial,
              std::function<void(const std::string &)> cb)
{
    GtkWidget *w = dialog_window(parent, title, 420, -1);
    GtkWidget *box = vbox(6);
    set_margins(box, 12);
    GtkWidget *e = gtk_entry_new();
    GtkWidget *l = label(prompt);
    gtk_label_set_mnemonic_widget(GTK_LABEL(l), e);
    gtk_box_append(GTK_BOX(box), l);
    gtk_editable_set_text(GTK_EDITABLE(e), initial.c_str());
    gtk_entry_set_activates_default(GTK_ENTRY(e), TRUE);
    gtk_box_append(GTK_BOX(box), e);
    auto b = button_row(box, {"OK", "Cancel"});
    auto *a = new AskText{std::move(cb), w, e};
    g_object_set_data_full(G_OBJECT(w), "ask", a, [](gpointer p) { delete static_cast<AskText *>(p); });
    g_signal_connect(b[0], "clicked", G_CALLBACK(ask_text_ok), a);
    g_signal_connect_swapped(b[1], "clicked", G_CALLBACK(gtk_window_destroy), w);
    gtk_window_set_child(GTK_WINDOW(w), box);
    gtk_window_set_default_widget(GTK_WINDOW(w), b[0]);
    gtk_window_present(GTK_WINDOW(w));
    // Select the name without the extension, like a file manager's rename.
    const auto dot = initial.rfind('.');
    gtk_widget_grab_focus(e);
    if (dot != std::string::npos && dot > 0)
        gtk_editable_select_region(GTK_EDITABLE(e), 0, (int)g_utf8_pointer_to_offset(initial.c_str(), initial.c_str() + dot));
}

namespace {
template <typename T>
struct FileCb {
    std::function<void(T)> cb;
};

void folder_done(GObject *src, GAsyncResult *res, gpointer p)
{
    auto *f = static_cast<FileCb<const std::string &> *>(p);
    GFile *file = gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(src), res, nullptr);
    if (file) {
        char *path = g_file_get_path(file);
        if (path) f->cb(path);
        g_free(path);
        g_object_unref(file);
    }
    delete f;
}

void save_done(GObject *src, GAsyncResult *res, gpointer p)
{
    auto *f = static_cast<FileCb<const std::string &> *>(p);
    GFile *file = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(src), res, nullptr);
    if (file) {
        char *path = g_file_get_path(file);
        if (path) f->cb(path);
        g_free(path);
        g_object_unref(file);
    }
    delete f;
}

void open_done(GObject *src, GAsyncResult *res, gpointer p)
{
    auto *f = static_cast<FileCb<const std::vector<std::string> &> *>(p);
    GListModel *files = gtk_file_dialog_open_multiple_finish(GTK_FILE_DIALOG(src), res, nullptr);
    if (files) {
        std::vector<std::string> out;
        for (guint i = 0; i < g_list_model_get_n_items(files); i++) {
            GFile *file = G_FILE(g_list_model_get_item(files, i));
            char *path = g_file_get_path(file);
            if (path) out.emplace_back(path);
            g_free(path);
            g_object_unref(file);
        }
        g_object_unref(files);
        if (!out.empty()) f->cb(out);
    }
    delete f;
}
}  // namespace

void choose_folder(GtkWindow *parent, const std::string &title, const std::string &initial,
                   std::function<void(const std::string &)> cb)
{
    GtkFileDialog *d = gtk_file_dialog_new();
    gtk_file_dialog_set_title(d, title.c_str());
    gtk_file_dialog_set_modal(d, TRUE);
    if (!initial.empty() && g_file_test(initial.c_str(), G_FILE_TEST_IS_DIR)) {
        GFile *f = g_file_new_for_path(initial.c_str());
        gtk_file_dialog_set_initial_folder(d, f);
        g_object_unref(f);
    }
    gtk_file_dialog_select_folder(d, parent, nullptr, folder_done, new FileCb<const std::string &>{std::move(cb)});
    g_object_unref(d);
}

void choose_save_file(GtkWindow *parent, const std::string &title, const std::string &initial,
                      std::function<void(const std::string &)> cb)
{
    GtkFileDialog *d = gtk_file_dialog_new();
    gtk_file_dialog_set_title(d, title.c_str());
    gtk_file_dialog_set_modal(d, TRUE);
    if (!initial.empty()) {
        char *dir = g_path_get_dirname(initial.c_str());
        char *base = g_path_get_basename(initial.c_str());
        if (g_file_test(dir, G_FILE_TEST_IS_DIR)) {
            GFile *f = g_file_new_for_path(dir);
            gtk_file_dialog_set_initial_folder(d, f);
            g_object_unref(f);
        }
        gtk_file_dialog_set_initial_name(d, base);
        g_free(dir);
        g_free(base);
    }
    gtk_file_dialog_save(d, parent, nullptr, save_done, new FileCb<const std::string &>{std::move(cb)});
    g_object_unref(d);
}

void choose_open_files(GtkWindow *parent, const std::string &title, const std::string &initial_dir,
                       std::function<void(const std::vector<std::string> &)> cb)
{
    GtkFileDialog *d = gtk_file_dialog_new();
    gtk_file_dialog_set_title(d, title.c_str());
    gtk_file_dialog_set_modal(d, TRUE);
    if (!initial_dir.empty() && g_file_test(initial_dir.c_str(), G_FILE_TEST_IS_DIR)) {
        GFile *f = g_file_new_for_path(initial_dir.c_str());
        gtk_file_dialog_set_initial_folder(d, f);
        g_object_unref(f);
    }
    gtk_file_dialog_open_multiple(d, parent, nullptr, open_done, new FileCb<const std::vector<std::string> &>{std::move(cb)});
    g_object_unref(d);
}

void launch_file(GtkWindow *parent, const std::string &path)
{
    GFile *f = g_file_new_for_path(path.c_str());
    GtkFileLauncher *l = gtk_file_launcher_new(f);
    gtk_file_launcher_set_always_ask(l, FALSE);
    gtk_file_launcher_launch(l, parent, nullptr, nullptr, nullptr);
    g_object_unref(l);
    g_object_unref(f);
}

void copy_to_clipboard(GtkWidget *w, const std::string &text)
{
    gdk_clipboard_set_text(gtk_widget_get_clipboard(w), text.c_str());
}

}  // namespace szl
