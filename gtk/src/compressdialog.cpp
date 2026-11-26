// SPDX-License-Identifier: LGPL-2.1-or-later
#include "compressdialog.hpp"

#include "engine.hpp"
#include "gtkutil.hpp"
#include "settings.hpp"
#include "util.hpp"

#include <algorithm>
#include <thread>

namespace szl {

namespace {

struct Level {
    const char *name;
    int value;
};
const Level kLevels[] = {{"0 - Store", 0}, {"1 - Fastest", 1}, {"3 - Fast", 3},
                         {"5 - Normal", 5}, {"7 - Maximum", 7}, {"9 - Ultra", 9}};

std::vector<std::string> methods_for(const std::string &fmt)
{
    if (fmt == "7z") return {"LZMA2", "LZMA", "PPMd", "BZip2"};
    if (fmt == "zip") return {"Deflate", "Deflate64", "BZip2", "LZMA", "PPMd"};
    if (fmt == "gzip") return {"Deflate"};
    if (fmt == "bzip2") return {"BZip2"};
    if (fmt == "xz") return {"LZMA2"};
    return {};
}

std::string ext_for(const std::string &fmt)
{
    if (fmt == "gzip") return "gz";
    if (fmt == "bzip2") return "bz2";
    return fmt;
}

bool is_stream_format(const std::string &fmt) { return fmt == "gzip" || fmt == "bzip2" || fmt == "xz"; }

const char *const kDicts[] = {"Auto",   "64 KB",  "256 KB", "1 MB",   "2 MB",   "4 MB",   "8 MB",
                              "16 MB",  "32 MB",  "64 MB",  "128 MB", "256 MB", "512 MB", "1024 MB",
                              "1536 MB"};
const char *const kWordsLzma[] = {"Auto", "8", "12", "16", "24", "32", "48", "64", "96", "128", "192", "256", "273"};
const char *const kWordsPpmd[] = {"Auto", "2", "3", "4", "5", "6", "7", "8", "10", "12", "16", "20", "24", "32"};
const char *const kWordsDeflate[] = {"Auto", "8", "12", "16", "24", "32", "48", "64", "96", "128", "192", "258"};
const char *const kSolid[] = {"Auto",  "Non-solid", "1 MB",  "16 MB", "128 MB", "1 GB",
                              "4 GB",  "16 GB",     "64 GB", "Solid"};
const char *const kVolumes[] = {"",      "10M",    "100M",   "650M - CD", "700M - CD", "1000M",
                                "4092M - FAT", "4480M - DVD", "8128M - DVD DL", "23040M - BD"};

template <size_t N>
std::vector<std::string> vec(const char *const (&a)[N])
{
    return std::vector<std::string>(a, a + N);
}

// "64 MB" -> "64m", "Auto" -> ""
std::string size_switch(const std::string &s)
{
    if (s.empty() || s == "Auto") return {};
    std::string num, unit;
    for (char c : s) {
        if (g_ascii_isdigit(c))
            num += c;
        else if (g_ascii_isalpha(c) && unit.empty())
            unit = (char)g_ascii_tolower(c);
    }
    if (num.empty()) return {};
    return num + (unit.empty() || unit == "b" ? "" : unit);
}

std::string volume_switch(const std::string &s)
{
    // "650M - CD" -> "650m"; "1.5G" is not supported by 7-Zip, so digits and one unit only.
    std::string t = trim(s.substr(0, s.find(" - ")));
    if (t.empty()) return {};
    std::string out;
    for (char c : t) {
        if (g_ascii_isdigit(c)) out += c;
        else if (std::string("bkmgBKMG").find(c) != std::string::npos) {
            out += (char)g_ascii_tolower(c);
            break;
        } else if (c != ' ')
            return "?";
    }
    return out.empty() ? "?" : out;
}

struct Dlg {
    GtkWidget *win;
    GtkWidget *archive, *format, *level, *method, *dict, *word, *solid, *threads, *volumes, *volumes_preset, *params;
    GtkWidget *update, *pathmode;
    GtkWidget *o_shared, *o_delete, *o_symlinks, *o_hardlinks;
    GtkWidget *pw1, *pw2, *pw_show, *enc_method, *enc_names, *err;
    std::string base_dir;
    std::vector<std::string> files;
    std::vector<std::string> formats;
    std::function<void(const CompressRequest &)> cb;
    bool updating = false;
};

std::string cur_format(Dlg *d) { return dropdown_text(d->format); }

void fill_method_lists(Dlg *d)
{
    const std::string fmt = cur_format(d);
    const auto methods = methods_for(fmt);
    const CompressDefaults &c = settings().compress;
    dropdown_set_items(d->method, methods.empty() ? std::vector<std::string>{"-"} : methods);
    if (!c.method.empty()) dropdown_select(d->method, c.method);
    gtk_widget_set_sensitive(d->method, methods.size() > 1);
    const bool has_level = fmt != "tar" && fmt != "wim";
    gtk_widget_set_sensitive(d->level, has_level);
    if (!has_level) gtk_drop_down_set_selected(GTK_DROP_DOWN(d->level), 0);
    gtk_widget_set_sensitive(d->solid, fmt == "7z");
    gtk_widget_set_sensitive(d->dict, has_level);
    gtk_widget_set_sensitive(d->word, has_level);
    const bool enc = fmt == "7z" || fmt == "zip";
    gtk_widget_set_sensitive(d->pw1, enc);
    gtk_widget_set_sensitive(d->pw2, enc);
    gtk_widget_set_sensitive(d->enc_names, fmt == "7z");
    dropdown_set_items(d->enc_method, fmt == "zip" ? std::vector<std::string>{"ZipCrypto", "AES-256"}
                                                    : std::vector<std::string>{"AES-256"});
    if (fmt == "zip") dropdown_select(d->enc_method, c.encryption);
    gtk_widget_set_sensitive(d->enc_method, fmt == "zip");
}

void fill_word_list(Dlg *d)
{
    const std::string m = dropdown_text(d->method);
    if (m == "PPMd")
        dropdown_set_items(d->word, vec(kWordsPpmd));
    else if (m == "Deflate" || m == "Deflate64")
        dropdown_set_items(d->word, vec(kWordsDeflate));
    else if (m == "BZip2")
        dropdown_set_items(d->word, {"Auto"});
    else
        dropdown_set_items(d->word, vec(kWordsLzma));
    if (!settings().compress.word.empty()) dropdown_select(d->word, settings().compress.word);
    gtk_widget_set_sensitive(d->word, m != "BZip2" && gtk_widget_get_sensitive(d->level));
}

// Changes the archive name's extension to the format's one.
void update_archive_ext(Dlg *d)
{
    std::string name = gtk_editable_get_text(GTK_EDITABLE(d->archive));
    const std::string fmt = cur_format(d);
    const std::string base = path_basename(name);
    const auto dot = base.rfind('.');
    if (dot != std::string::npos && dot > 0) {
        std::string old = base.substr(dot + 1);
        for (auto &c : old) c = (char)g_ascii_tolower(c);
        bool known = false;
        for (const auto &f : d->formats)
            if (old == ext_for(f)) known = true;
        if (known) name = name.substr(0, name.size() - (base.size() - dot));
    }
    // "x.tar" + gzip -> "x.tar.gz"
    name += "." + ext_for(fmt);
    gtk_editable_set_text(GTK_EDITABLE(d->archive), name.c_str());
}

void on_ok(Dlg *d)
{
    const std::string fmt = cur_format(d);
    std::string archive = trim(gtk_editable_get_text(GTK_EDITABLE(d->archive)));
    auto fail = [d](const std::string &msg) {
        gtk_label_set_text(GTK_LABEL(d->err), msg.c_str());
        gtk_widget_set_visible(d->err, TRUE);
    };
    if (archive.empty()) return fail("Enter the archive name");
    if (archive[0] == '~') archive = home_dir() + archive.substr(1);
    if (archive[0] != '/') archive = path_join(d->base_dir, archive);
    if (is_stream_format(fmt) &&
        (d->files.size() != 1 || g_file_test(d->files[0].c_str(), G_FILE_TEST_IS_DIR)))
        return fail("The " + fmt + " format can compress only one file. Use tar first, or choose 7z.");
    const std::string pw = gtk_editable_get_text(GTK_EDITABLE(d->pw1));
    const bool show = gtk_check_button_get_active(GTK_CHECK_BUTTON(d->pw_show));
    if (!show && pw != gtk_editable_get_text(GTK_EDITABLE(d->pw2))) return fail("Passwords do not match");
    const std::string vol = volume_switch(gtk_editable_get_text(GTK_EDITABLE(d->volumes)));
    if (vol == "?") return fail("Incorrect volume size");

    CompressDefaults &c = settings().compress;
    c.format = fmt;
    const guint li = gtk_drop_down_get_selected(GTK_DROP_DOWN(d->level));
    const int level = li < G_N_ELEMENTS(kLevels) ? kLevels[li].value : 5;
    if (gtk_widget_get_sensitive(d->level)) c.level = level;
    const std::string method = gtk_widget_get_sensitive(d->method) ? dropdown_text(d->method) : std::string();
    c.method = method;
    const std::string dict = gtk_widget_get_sensitive(d->dict) ? dropdown_text(d->dict) : std::string("Auto");
    c.dictionary = dict == "Auto" ? "" : dict;
    const std::string word = gtk_widget_get_sensitive(d->word) ? dropdown_text(d->word) : std::string("Auto");
    c.word = word == "Auto" ? "" : word;
    const std::string solid = gtk_widget_get_sensitive(d->solid) ? dropdown_text(d->solid) : std::string("Auto");
    c.solid = solid == "Auto" ? "" : solid;
    const guint ti = gtk_drop_down_get_selected(GTK_DROP_DOWN(d->threads));
    c.threads = (int)ti;  // 0 = Auto
    c.update_mode = (int)gtk_drop_down_get_selected(GTK_DROP_DOWN(d->update));
    c.path_mode = (int)gtk_drop_down_get_selected(GTK_DROP_DOWN(d->pathmode));
    c.shared_files = gtk_check_button_get_active(GTK_CHECK_BUTTON(d->o_shared));
    c.store_symlinks = gtk_check_button_get_active(GTK_CHECK_BUTTON(d->o_symlinks));
    c.store_hardlinks = gtk_check_button_get_active(GTK_CHECK_BUTTON(d->o_hardlinks));
    const bool del = gtk_check_button_get_active(GTK_CHECK_BUTTON(d->o_delete));
    c.encrypt_names = gtk_check_button_get_active(GTK_CHECK_BUTTON(d->enc_names));
    if (fmt == "zip") c.encryption = dropdown_text(d->enc_method);
    c.parameters = gtk_editable_get_text(GTK_EDITABLE(d->params));
    settings().push_history(settings().archive_history, archive);
    settings().save();

    CompressRequest req;
    req.archive = archive;
    auto &a = req.args;
    a.push_back(c.update_mode == 0 ? "a" : "u");
    if (c.update_mode == 2) a.push_back("-ur0");  // freshen: don't add files that aren't in the archive
    if (c.update_mode == 3) a.push_back("-uq0");  // synchronize: delete items whose files are gone
    a.push_back("-t" + fmt);
    if (gtk_widget_get_sensitive(d->level)) a.push_back("-mx" + std::to_string(level));
    if (!method.empty() && method != "-") {
        if (fmt == "7z") a.push_back("-m0=" + method);
        else if (fmt == "zip") a.push_back("-mm=" + method);
    }
    if (const std::string s = size_switch(c.dictionary); !s.empty()) {
        if (method == "PPMd") a.push_back("-mmem=" + s);
        else a.push_back("-md=" + s);
    }
    if (!c.word.empty()) a.push_back((method == "PPMd" ? "-mo=" : "-mfb=") + c.word);
    if (fmt == "7z" && !c.solid.empty()) {
        if (c.solid == "Non-solid") a.push_back("-ms=off");
        else if (c.solid == "Solid") a.push_back("-ms=on");
        else a.push_back("-ms=" + size_switch(c.solid));
    }
    if (c.threads > 0) a.push_back("-mmt=" + std::to_string(c.threads));
    if (!vol.empty()) a.push_back("-v" + vol);
    if (c.path_mode == 1) a.push_back("-spf2");
    else if (c.path_mode == 2) a.push_back("-spf");
    if (c.shared_files) a.push_back("-ssw");
    if (c.store_symlinks) a.push_back("-snl");
    if (c.store_hardlinks) a.push_back("-snh");
    if (del) a.push_back("-sdel");
    if (!pw.empty() && (fmt == "7z" || fmt == "zip")) {
        a.push_back("-p" + pw);
        if (fmt == "7z" && c.encrypt_names) a.push_back("-mhe=on");
        if (fmt == "zip" && c.encryption == "AES-256") a.push_back("-mem=AES256");
    }
    for (const auto &p : split_args(c.parameters)) a.push_back(p[0] == '-' ? p : "-m" + p);
    a.push_back("-spd");  // names are literal, not wildcards
    a.push_back("--");
    a.push_back(archive);
    for (const auto &f : d->files) a.push_back(f);

    auto cb = d->cb;
    gtk_window_destroy(GTK_WINDOW(d->win));
    cb(req);
}

GtkWidget *grid_row(GtkWidget *grid, int row, const char *title, GtkWidget *w)
{
    GtkWidget *l = label(title);
    gtk_label_set_mnemonic_widget(GTK_LABEL(l), w);
    gtk_grid_attach(GTK_GRID(grid), l, 0, row, 1, 1);
    gtk_widget_set_hexpand(w, TRUE);
    gtk_grid_attach(GTK_GRID(grid), w, 1, row, 1, 1);
    return l;
}

}  // namespace

void compress_dialog(GtkWindow *parent, const std::string &base_dir, const std::vector<std::string> &files,
                     const std::string &archive, std::function<void(const CompressRequest &)> cb)
{
    auto *d = new Dlg();
    d->base_dir = base_dir;
    d->files = files;
    d->cb = std::move(cb);
    d->formats = update_formats();
    const CompressDefaults &c = settings().compress;

    d->win = dialog_window(parent, "Add to Archive", 720, -1);
    g_object_set_data_full(G_OBJECT(d->win), "dlg", d, [](gpointer p) { delete static_cast<Dlg *>(p); });
    GtkWidget *box = vbox(8);
    set_margins(box, 12);

    // Archive name
    GtkWidget *al = label("_Archive:");
    gtk_box_append(GTK_BOX(box), al);
    GtkWidget *arow = hbox(6);
    d->archive = gtk_entry_new();
    gtk_label_set_mnemonic_widget(GTK_LABEL(al), d->archive);
    gtk_widget_set_hexpand(d->archive, TRUE);
    std::string name = archive;
    if (name.empty()) {
        const std::string first = files.empty() ? std::string("archive") : path_basename(files[0]);
        std::string stem = files.size() == 1 ? first : path_basename(base_dir);
        if (files.size() == 1 && !g_file_test(files[0].c_str(), G_FILE_TEST_IS_DIR)) {
            const auto dot = stem.rfind('.');
            if (dot != std::string::npos && dot > 0) stem = stem.substr(0, dot);
        }
        if (stem.empty() || stem == "/") stem = "archive";
        name = stem + "." + ext_for(c.format);
    } else if (path_dirname(name) == base_dir) {
        name = path_basename(name);
    }
    gtk_editable_set_text(GTK_EDITABLE(d->archive), name.c_str());
    gtk_box_append(GTK_BOX(arow), d->archive);
    GtkWidget *browse = gtk_button_new_with_label("...");
    gtk_box_append(GTK_BOX(arow), browse);
    g_signal_connect_swapped(browse, "clicked", CB([](Dlg *d) {
                                 std::string cur = gtk_editable_get_text(GTK_EDITABLE(d->archive));
                                 if (cur.empty() || cur[0] != '/') cur = path_join(d->base_dir, cur);
                                 choose_save_file(GTK_WINDOW(d->win), "Browse", cur, [d](const std::string &p) {
                                     gtk_editable_set_text(GTK_EDITABLE(d->archive), p.c_str());
                                 });
                             }),
                             d);
    gtk_box_append(GTK_BOX(box), arow);

    GtkWidget *cols = hbox(16);
    gtk_box_append(GTK_BOX(box), cols);

    // Left column: format and compression settings
    GtkWidget *g = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(g), 6);
    gtk_grid_set_column_spacing(GTK_GRID(g), 10);
    gtk_widget_set_hexpand(g, TRUE);
    int r = 0;
    unsigned fsel = 0;
    for (unsigned i = 0; i < d->formats.size(); i++)
        if (d->formats[i] == c.format) fsel = i;
    d->format = dropdown(d->formats, fsel);
    grid_row(g, r++, "Archive _format:", d->format);
    std::vector<std::string> levels;
    unsigned lsel = 3;
    for (unsigned i = 0; i < G_N_ELEMENTS(kLevels); i++) {
        levels.emplace_back(kLevels[i].name);
        if (kLevels[i].value == c.level) lsel = i;
    }
    d->level = dropdown(levels, lsel);
    grid_row(g, r++, "Compression _level:", d->level);
    d->method = dropdown({});
    grid_row(g, r++, "Compression _method:", d->method);
    d->dict = dropdown(vec(kDicts));
    if (!c.dictionary.empty()) dropdown_select(d->dict, c.dictionary);
    grid_row(g, r++, "_Dictionary size:", d->dict);
    d->word = dropdown(vec(kWordsLzma));
    grid_row(g, r++, "_Word size:", d->word);
    d->solid = dropdown(vec(kSolid));
    if (!c.solid.empty()) dropdown_select(d->solid, c.solid);
    grid_row(g, r++, "_Solid Block size:", d->solid);
    std::vector<std::string> th = {"Auto"};
    const unsigned ncpu = std::max(1u, std::thread::hardware_concurrency());
    for (unsigned i = 1; i <= ncpu * 2; i++) th.push_back(std::to_string(i));
    d->threads = dropdown(th, c.threads >= 0 && (unsigned)c.threads < th.size() ? (unsigned)c.threads : 0);
    GtkWidget *trow = hbox(6);
    gtk_box_append(GTK_BOX(trow), d->threads);
    gtk_box_append(GTK_BOX(trow), label("/ " + std::to_string(ncpu)));
    grid_row(g, r++, "Number of CPU _threads:", trow);
    gtk_label_set_mnemonic_widget(GTK_LABEL(gtk_grid_get_child_at(GTK_GRID(g), 0, r - 1)), d->threads);

    GtkWidget *vrow = hbox(6);
    d->volumes = gtk_entry_new();
    gtk_widget_set_hexpand(d->volumes, TRUE);
    gtk_entry_set_placeholder_text(GTK_ENTRY(d->volumes), "e.g. 100M");
    d->volumes_preset = dropdown(vec(kVolumes));
    gtk_box_append(GTK_BOX(vrow), d->volumes);
    gtk_box_append(GTK_BOX(vrow), d->volumes_preset);
    g_signal_connect(d->volumes_preset, "notify::selected", CB([](GtkWidget *dd, GParamSpec *, Dlg *d) {
                         gtk_editable_set_text(GTK_EDITABLE(d->volumes), dropdown_text(dd).c_str());
                     }),
                     d);
    GtkWidget *vl = label("Split to _volumes, bytes:");
    gtk_label_set_mnemonic_widget(GTK_LABEL(vl), d->volumes);
    gtk_grid_attach(GTK_GRID(g), vl, 0, r, 2, 1);
    r++;
    gtk_grid_attach(GTK_GRID(g), vrow, 0, r++, 2, 1);
    d->params = gtk_entry_new();
    gtk_editable_set_text(GTK_EDITABLE(d->params), c.parameters.c_str());
    gtk_entry_set_placeholder_text(GTK_ENTRY(d->params), "e.g. qs=on mtf=off");
    GtkWidget *pl = label("_Parameters:");
    gtk_label_set_mnemonic_widget(GTK_LABEL(pl), d->params);
    gtk_grid_attach(GTK_GRID(g), pl, 0, r++, 2, 1);
    gtk_grid_attach(GTK_GRID(g), d->params, 0, r++, 2, 1);
    gtk_box_append(GTK_BOX(cols), g);

    // Right column: update mode, path mode, options, encryption
    GtkWidget *right = vbox(8);
    gtk_widget_set_hexpand(right, TRUE);
    GtkWidget *g2 = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(g2), 6);
    gtk_grid_set_column_spacing(GTK_GRID(g2), 10);
    d->update = dropdown({"Add and replace files", "Update and add files", "Freshen existing files",
                          "Synchronize files"},
                         (unsigned)std::clamp(c.update_mode, 0, 3));
    grid_row(g2, 0, "_Update mode:", d->update);
    d->pathmode = dropdown({"Relative pathnames", "Full pathnames", "Absolute pathnames"},
                           (unsigned)std::clamp(c.path_mode, 0, 2));
    grid_row(g2, 1, "Path _mode:", d->pathmode);
    gtk_box_append(GTK_BOX(right), g2);

    GtkWidget *opts = vbox(2);
    d->o_shared = gtk_check_button_new_with_mnemonic("Compress shared files");
    gtk_check_button_set_active(GTK_CHECK_BUTTON(d->o_shared), c.shared_files);
    d->o_delete = gtk_check_button_new_with_mnemonic("Delete files after compression");
    d->o_symlinks = gtk_check_button_new_with_mnemonic("Store symbolic links");
    gtk_check_button_set_active(GTK_CHECK_BUTTON(d->o_symlinks), c.store_symlinks);
    d->o_hardlinks = gtk_check_button_new_with_mnemonic("Store hard links");
    gtk_check_button_set_active(GTK_CHECK_BUTTON(d->o_hardlinks), c.store_hardlinks);
    gtk_box_append(GTK_BOX(opts), d->o_shared);
    gtk_box_append(GTK_BOX(opts), d->o_delete);
    gtk_box_append(GTK_BOX(opts), d->o_symlinks);
    gtk_box_append(GTK_BOX(opts), d->o_hardlinks);
    gtk_box_append(GTK_BOX(right), frame("Options", opts));

    GtkWidget *enc = vbox(4);
    d->pw1 = gtk_entry_new();
    gtk_entry_set_visibility(GTK_ENTRY(d->pw1), FALSE);
    gtk_entry_set_input_purpose(GTK_ENTRY(d->pw1), GTK_INPUT_PURPOSE_PASSWORD);
    d->pw2 = gtk_entry_new();
    gtk_entry_set_visibility(GTK_ENTRY(d->pw2), FALSE);
    gtk_entry_set_input_purpose(GTK_ENTRY(d->pw2), GTK_INPUT_PURPOSE_PASSWORD);
    GtkWidget *l1 = label("Enter _password:");
    gtk_label_set_mnemonic_widget(GTK_LABEL(l1), d->pw1);
    gtk_box_append(GTK_BOX(enc), l1);
    gtk_box_append(GTK_BOX(enc), d->pw1);
    GtkWidget *l2 = label("Reenter password:");
    gtk_box_append(GTK_BOX(enc), l2);
    gtk_box_append(GTK_BOX(enc), d->pw2);
    d->pw_show = gtk_check_button_new_with_mnemonic("Show Password");
    gtk_box_append(GTK_BOX(enc), d->pw_show);
    g_object_set_data(G_OBJECT(d->pw_show), "l2", l2);
    g_signal_connect(d->pw_show, "toggled", CB([](GtkCheckButton *b, Dlg *d) {
                         const bool v = gtk_check_button_get_active(b);
                         gtk_entry_set_visibility(GTK_ENTRY(d->pw1), v);
                         gtk_widget_set_visible(d->pw2, !v);
                         gtk_widget_set_visible(GTK_WIDGET(g_object_get_data(G_OBJECT(b), "l2")), !v);
                     }),
                     d);
    GtkWidget *erow = hbox(6);
    gtk_box_append(GTK_BOX(erow), label("Encryption method:"));
    d->enc_method = dropdown({"AES-256"});
    gtk_widget_set_hexpand(d->enc_method, TRUE);
    gtk_box_append(GTK_BOX(erow), d->enc_method);
    gtk_box_append(GTK_BOX(enc), erow);
    d->enc_names = gtk_check_button_new_with_mnemonic("Encrypt file _names");
    gtk_check_button_set_active(GTK_CHECK_BUTTON(d->enc_names), c.encrypt_names);
    gtk_box_append(GTK_BOX(enc), d->enc_names);
    gtk_box_append(GTK_BOX(right), frame("Encryption", enc));
    gtk_box_append(GTK_BOX(cols), right);

    d->err = label("");
    gtk_widget_add_css_class(d->err, "szl-warning");
    gtk_widget_set_visible(d->err, FALSE);
    gtk_box_append(GTK_BOX(box), d->err);

    auto b = button_row(box, {"OK", "Cancel"});
    g_signal_connect_swapped(b[0], "clicked", CB([](Dlg *d) { on_ok(d); }), d);
    g_signal_connect_swapped(b[1], "clicked", G_CALLBACK(gtk_window_destroy), d->win);
    gtk_entry_set_activates_default(GTK_ENTRY(d->archive), TRUE);

    fill_method_lists(d);
    fill_word_list(d);
    g_signal_connect(d->format, "notify::selected", CB([](GtkWidget *, GParamSpec *, Dlg *d) {
                         fill_method_lists(d);
                         fill_word_list(d);
                         update_archive_ext(d);
                     }),
                     d);
    g_signal_connect(d->method, "notify::selected", CB([](GtkWidget *, GParamSpec *, Dlg *d) { fill_word_list(d); }), d);

    gtk_window_set_child(GTK_WINDOW(d->win), box);
    gtk_window_set_default_widget(GTK_WINDOW(d->win), b[0]);
    gtk_window_present(GTK_WINDOW(d->win));
    gtk_widget_grab_focus(d->archive);
}

CompressRequest quick_compress(const std::string &archive, const std::vector<std::string> &files,
                               const std::string &format, const std::string &password)
{
    const CompressDefaults &c = settings().compress;
    CompressRequest req;
    req.archive = archive;
    req.args.push_back("a");
    if (!format.empty()) {
        req.args.push_back("-t" + format);
        if (format == c.format && format != "tar" && format != "wim") req.args.push_back("-mx" + std::to_string(c.level));
    }
    if (!password.empty()) req.args.push_back("-p" + password);
    req.args.push_back("-spd");
    req.args.push_back("--");
    req.args.push_back(archive);
    for (const auto &f : files) req.args.push_back(f);
    return req;
}

}  // namespace szl
