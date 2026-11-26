// SPDX-License-Identifier: LGPL-2.1-or-later
// 7zipl: the main window's commands (File, Edit, Favorites and Tools menus, the toolbar).
#include "compressdialog.hpp"
#include "dialogs.hpp"
#include "extractdialog.hpp"
#include "gtkutil.hpp"
#include "job.hpp"
#include "mainwindow.hpp"
#include "optionsdialog.hpp"
#include "settings.hpp"
#include "util.hpp"

#include <gio/gio.h>
#include <glib/gstdio.h>

#include <atomic>
#include <set>
#include <thread>

#include <unistd.h>

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

// The start of a command that changes an archive: the verb, the working folder (Options > Folders) and the
// archive's password, kept with header encryption.
std::vector<std::string> update_args(const char *verb, const std::string &format, const std::string &password)
{
    std::vector<std::string> a = {verb};
    for (auto &w : settings().work_args()) a.push_back(w);
    if (!password.empty()) {
        a.push_back("-p" + password);
        if (format == "7z") a.push_back("-mhe=on");
    }
    return a;
}
std::vector<std::string> update_args(const char *verb, const ArcLevel &arc) { return update_args(verb, arc.format, arc.password); }

// A path relative to `from_dir` that leads to `target` (for relative symbolic links).
std::string relative_path(const std::string &from_dir, const std::string &target)
{
    const auto a = split(from_dir, '/'), b = split(target, '/');
    std::vector<std::string> fa, fb;
    for (const auto &x : a) if (!x.empty()) fa.push_back(x);
    for (const auto &x : b) if (!x.empty()) fb.push_back(x);
    size_t i = 0;
    while (i < fa.size() && i < fb.size() && fa[i] == fb[i]) i++;
    std::string out;
    for (size_t k = i; k < fa.size(); k++) out += "../";
    for (size_t k = i; k < fb.size(); k++) out += fb[k] + (k + 1 < fb.size() ? "/" : "");
    return out.empty() ? "." : out;
}

std::string file_stamp(const std::string &path)
{
    GFile *f = g_file_new_for_path(path.c_str());
    GFileInfo *i = g_file_query_info(f, "time::modified,time::modified-usec,standard::size", G_FILE_QUERY_INFO_NONE, nullptr, nullptr);
    g_object_unref(f);
    if (!i) return {};
    const std::string s = std::to_string(g_file_info_get_attribute_uint64(i, "time::modified")) + "." +
                          std::to_string(g_file_info_get_attribute_uint32(i, "time::modified-usec")) + "/" +
                          std::to_string(g_file_info_get_size(i));
    g_object_unref(i);
    return s;
}

// A "path [...]" row: an entry and a button that picks a folder.
GtkWidget *folder_row(GtkWindow *parent, GtkWidget *box, const std::string &value)
{
    GtkWidget *row = hbox(6);
    GtkWidget *e = gtk_entry_new();
    gtk_editable_set_text(GTK_EDITABLE(e), value.c_str());
    gtk_widget_set_hexpand(e, TRUE);
    gtk_entry_set_activates_default(GTK_ENTRY(e), TRUE);
    gtk_box_append(GTK_BOX(row), e);
    GtkWidget *b = gtk_button_new_with_label("...");
    g_object_set_data(G_OBJECT(b), "entry", e);
    g_signal_connect(b, "clicked", CB([](GtkWidget *b, GtkWindow *parent) {
                         GtkWidget *e = GTK_WIDGET(g_object_get_data(G_OBJECT(b), "entry"));
                         choose_folder(GTK_WINDOW(gtk_widget_get_root(b)), "Browse", gtk_editable_get_text(GTK_EDITABLE(e)),
                                       [e](const std::string &p) { gtk_editable_set_text(GTK_EDITABLE(e), p.c_str()); });
                         (void)parent;
                     }),
                     parent);
    gtk_box_append(GTK_BOX(row), b);
    gtk_box_append(GTK_BOX(box), row);
    return e;
}

std::vector<Row> files_only(const std::vector<Row> &rows)
{
    std::vector<Row> out;
    for (const Row &r : rows)
        if (!r.dir && !r.up) out.push_back(r);
    return out;
}

// "650M - CD" -> bytes; 0 = invalid
uint64_t parse_size(const std::string &s)
{
    std::string t = trim(s.substr(0, s.find(" - ")));
    uint64_t n = 0;
    size_t i = 0;
    for (; i < t.size() && g_ascii_isdigit(t[i]); i++) n = n * 10 + (uint64_t)(t[i] - '0');
    if (i == 0) return 0;
    while (i < t.size() && t[i] == ' ') i++;
    const char u = i < t.size() ? (char)g_ascii_tolower(t[i]) : 'b';
    switch (u) {
    case 'b': return n;
    case 'k': return n << 10;
    case 'm': return n << 20;
    case 'g': return n << 30;
    case 't': return n << 40;
    default: return 0;
    }
}

// The diff program: Options > Editor, else the first one installed.
std::string diff_program()
{
    if (!settings().diff.empty()) return settings().diff;
    for (const char *p : {"meld", "kompare", "kdiff3", "diffuse", "kdiff", "tkdiff"}) {
        char *f = g_find_program_in_path(p);
        if (f) {
            g_free(f);
            return p;
        }
    }
    return {};
}

}  // namespace

void MainWindow::cmd_open(int mode)
{
    // Enter in an address bar is handled by the entry itself.
    GtkWidget *f = gtk_root_get_focus(GTK_ROOT(win_));
    if (f && GTK_IS_TEXT(f)) return;
    auto r = panel().focused();
    if (!r) return;
    panel().activate(window(), *r, mode);
}

void MainWindow::cmd_add()
{
    if (panel().in_archive()) {
        if (!panel().arc().can_update) {
            show_message(window(), "7zipl", "This archive can't be changed (" + panel().arc().format + ").", true);
            return;
        }
        choose_open_files(window(), "Add files to archive", panel().fs_dir(),
                          [this](const std::vector<std::string> &files) { add_files_to_archive(files); });
        return;
    }
    auto sel = panel().selected();
    if (sel.empty()) {
        show_message(window(), "7zipl", "Select one or more files and folders to add to an archive.");
        return;
    }
    compress_dialog(window(), panel().fs_dir(), paths_of(sel), {}, [this](const CompressRequest &req) {
        JobSpec spec;
        spec.title = "Compressing";
        spec.archive = req.archive;
        spec.show_ratio = true;
        auto args = req.args;
        const auto work = settings().work_args();
        args.insert(args.begin() + 1, work.begin(), work.end());
        const std::string archive = req.archive;
        start_job(window(), spec, [args](Ui &ui) { return run(args, ui); },
                  [this, archive](const Result &) {
                      if (panel().in_archive()) return;
                      panel().refresh(window());
                      if (path_dirname(archive) == panel().fs_dir()) panel().select_name(path_basename(archive));
                  });
    });
}

void MainWindow::add_files_to_archive(Panel &p, const std::vector<std::string> &files, bool move)
{
    if (!p.in_archive() || files.empty()) return;
    ArcLevel &a = p.arc();
    if (!a.can_update) {
        show_message(window(), "7zipl", "This archive can't be changed (" + a.format + ").", true);
        return;
    }
    std::vector<std::string> args = update_args("a", a);
    if (move) args.push_back("-sdel");
    args.push_back("-spd");
    args.push_back("--");
    args.push_back(a.file);
    if (p.prefix().empty()) {
        for (const auto &f : files) args.push_back(f);
    } else {
        const std::string top = stage_for_prefix(p.prefix(), files, {});
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

void MainWindow::copy_files_to(const std::vector<std::string> &files, const std::string &dest, bool move)
{
    JobSpec spec;
    spec.title = move ? "Moving" : "Copying";
    start_job(window(), spec, [files, dest, move](Ui &ui) { return copy_files(ui, files, dest, move); },
              [this](const Result &) { after_change(); });
}

void MainWindow::cmd_extract()
{
    if (panel().in_archive()) {
        ArcLevel &a = panel().arc();
        auto sel = panel().selected();
        const std::string prefix = panel().prefix();
        std::vector<std::string> items = paths_of(sel);
        if (items.empty() && !prefix.empty()) items.push_back(prefix);
        const std::string file = a.file, password = a.password;
        const std::string base = panel().fs_dir();
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
    for (const Row &r : panel().selected())
        if (!r.dir) archives.push_back(r.full);
    if (archives.empty()) {
        show_message(window(), "7zipl", "Select one or more archives to extract.");
        return;
    }
    extract_dialog(window(), panel().fs_dir(), archives, false, [this, archives](const ExtractChoice &c) {
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
    if (panel().in_archive()) {
        ArcLevel &a = panel().arc();
        ExtractRequest req;
        req.archive = a.file;
        req.test = true;
        req.items = paths_of(panel().selected());
        if (req.items.empty() && !panel().prefix().empty()) req.items.push_back(panel().prefix());
        req.password = a.password;
        spec.archive = a.virtual_path;
        spec.password = a.password;
        start_job(window(), spec, [req](Ui &ui) { return extract_items(req, ui); });
        return;
    }
    std::vector<std::string> archives;
    for (const Row &r : panel().selected())
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
    auto sel = panel().selected();
    if (sel.empty()) return;
    // 2 Panels: like 7-Zip, F5/F6 go to the other panel (into its archive when it shows one).
    if (two_) {
        Panel &o = other_panel();
        const std::string dest = o.in_archive() ? o.address() : o.fs_dir();
        const std::string what = sel.size() == 1 ? "'" + sel[0].name + "'" : std::to_string(sel.size()) + " items";
        ask_text(window(), move ? "Move" : "Copy", (move ? "Move " : "Copy ") + what + " to:", dest,
                 [this, move, dest](const std::string &to_in) {
                     const std::string to = trim(to_in);
                     if (to.empty()) return;
                     Panel &o = other_panel();
                     auto sel = panel().selected();
                     if (o.in_archive() && to == dest) {
                         if (panel().in_archive()) {
                             show_message(window(), "7zipl", "Copying from one archive into another isn't supported. Extract the files first.", true);
                             return;
                         }
                         add_files_to_archive(o, paths_of(sel), move);
                         return;
                     }
                     if (panel().in_archive())
                         extract_selection_to(sel, to, move);
                     else
                         copy_files_to(paths_of(sel), to, move);
                 });
        return;
    }
    if (panel().in_archive()) {
        choose_folder(window(), move ? "Move to" : "Copy to", panel().fs_dir(),
                      [this, sel, move](const std::string &dir) { extract_selection_to(sel, dir, move); });
        return;
    }
    const auto files = paths_of(sel);
    choose_folder(window(), move ? "Move to" : "Copy to", panel().fs_dir(), [this, files, move](const std::string &dir) {
        copy_files_to(files, dir, move);
    });
}

void MainWindow::extract_selection_to(const std::vector<Row> &sel, const std::string &dir, bool move)
{
    ArcLevel &a = panel().arc();
    if (move && !a.can_update) return;
    const std::string file = a.file, password = a.password;
    const auto items = paths_of(sel);
    g_mkdir_with_parents(dir.c_str(), 0755);
    ExtractRequest req;
    req.archive = file;
    req.out_dir = dir;
    req.items = items;
    req.strip_prefix = panel().prefix();
    req.password = password;
    JobSpec spec;
    spec.title = move ? "Moving" : "Extracting";
    spec.archive = file;
    spec.password = password;
    std::vector<std::string> del = update_args("d", a);
    del.push_back("-spd");
    del.push_back("--");
    del.push_back(file);
    for (const auto &i : items) del.push_back(i);
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
              [this](const Result &) { after_change(); });
}

void MainWindow::cmd_delete(bool permanently)
{
    auto sel = panel().selected();
    if (sel.empty()) return;
    const std::string what = sel.size() == 1 ? "'" + sel[0].name + "'" : std::to_string(sel.size()) + " items";
    if (panel().in_archive()) {
        ArcLevel &a = panel().arc();
        if (!a.can_update) {
            show_message(window(), "7zipl", "This archive can't be changed (" + a.format + ").", true);
            return;
        }
        std::vector<std::string> args = update_args("d", a);
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
    auto r = panel().focused();
    if (!r) return;
    const Row row = *r;
    ask_text(window(), "Rename", "New name:", row.name, [this, row](const std::string &name_in) {
        const std::string name = trim(name_in);
        if (name.empty() || name == row.name) return;
        if (name.find('/') != std::string::npos) {
            show_message(window(), "7zipl", "The name can't contain '/'", true);
            return;
        }
        if (panel().in_archive()) {
            ArcLevel &a = panel().arc();
            if (!a.can_update) return;
            const std::string dir = row.full.find('/') == std::string::npos ? std::string() : path_dirname(row.full);
            std::vector<std::string> args = update_args("rn", a);
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
        const std::string dst = path_join(panel().fs_dir(), name);
        if (g_file_test(dst.c_str(), G_FILE_TEST_EXISTS)) {
            show_message(window(), "7zipl", "A file or folder with this name already exists:\n" + dst, true);
            return;
        }
        if (g_rename(row.full.c_str(), dst.c_str()) != 0) {
            show_message(window(), "7zipl", std::string("Cannot rename: ") + g_strerror(errno), true);
            return;
        }
        panel().refresh(window());
        panel().select_name(name);
    });
}

void MainWindow::cmd_new_folder()
{
    ask_text(window(), "Create Folder", "Folder name:", "New Folder", [this](const std::string &name_in) {
        const std::string name = trim(name_in);
        if (name.empty() || name.find('/') != std::string::npos) return;
        if (panel().in_archive()) {
            ArcLevel &a = panel().arc();
            if (!a.can_update) return;
            const std::string prefix = panel().prefix();
            std::vector<std::string> args = update_args("a", a);
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
        const std::string p = path_join(panel().fs_dir(), name);
        if (g_mkdir(p.c_str(), 0755) != 0) {
            show_message(window(), "7zipl", std::string("Cannot create folder: ") + g_strerror(errno), true);
            return;
        }
        panel().refresh(window());
        panel().select_name(name);
    });
}

void MainWindow::cmd_new_file()
{
    if (panel().in_archive()) return;
    ask_text(window(), "Create File", "File name:", "New File.txt", [this](const std::string &name_in) {
        const std::string name = trim(name_in);
        if (name.empty() || name.find('/') != std::string::npos) return;
        const std::string p = path_join(panel().fs_dir(), name);
        if (g_file_test(p.c_str(), G_FILE_TEST_EXISTS)) {
            show_message(window(), "7zipl", "The file already exists", true);
            return;
        }
        if (!g_file_set_contents(p.c_str(), "", 0, nullptr)) {
            show_message(window(), "7zipl", "Cannot create the file", true);
            return;
        }
        panel().refresh(window());
        panel().select_name(name);
    });
}

void MainWindow::cmd_properties()
{
    auto sel = panel().selected();
    std::vector<std::pair<std::string, std::string>> rows;
    auto time_row = [&](const char *n, bool has, int64_t t) {
        if (has) rows.emplace_back(n, format_time(t));
    };
    if (sel.size() == 1) {
        const Row &r = sel[0];
        rows.emplace_back("Name", r.name);
        rows.emplace_back("Path", panel().in_archive() ? r.full : r.full);
        rows.emplace_back("Folder", r.dir ? "+" : "-");
        if (r.has_size) rows.emplace_back("Size", format_number(r.size));
        if (r.has_packed) rows.emplace_back("Packed Size", format_number(r.packed));
        if (r.dir && panel().in_archive()) {
            rows.emplace_back("Folders", format_number(r.num_folders));
            rows.emplace_back("Files", format_number(r.num_files));
        }
        time_row("Modified", r.has_mtime, r.mtime);
        time_row("Created", r.has_ctime, r.ctime);
        time_row("Accessed", r.has_atime, r.atime);
        if (!r.attrib.empty()) rows.emplace_back("Attributes", r.attrib);
        if (panel().in_archive() && !r.dir) rows.emplace_back("Encrypted", r.encrypted ? "+" : "-");
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
        if (!panel().in_archive() && !r.content_type.empty()) {
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
        if (panel().in_archive()) {
            rows.emplace_back("Packed Size", format_number(packed));
            rows.emplace_back("Folders", format_number(dirs));
            rows.emplace_back("Files", format_number(files));
        }
    }
    if (panel().in_archive()) {
        const ArcLevel &a = panel().arc();
        rows.emplace_back("--", path_basename(a.virtual_path));
        rows.emplace_back("Path", a.virtual_path);
        for (const auto &p : a.props) rows.push_back(p);
    }
    if (rows.empty()) return;
    properties_window(window(), "Properties", rows);
}

void MainWindow::cmd_hash(const std::string &method)
{
    auto sel = panel().selected();
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
    if (panel().in_archive()) {
        // Checksums of archive items: extract them to a temporary folder first.
        ArcLevel &a = panel().arc();
        const std::string tmp = make_temp_dir("hash");
        ExtractRequest req;
        req.archive = a.file;
        req.out_dir = tmp;
        req.items = paths_of(sel);
        req.strip_prefix = panel().prefix();
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
        panel().select_mask(m.empty() ? "*" : m, select);
    });
}

void MainWindow::cmd_add_favorite(int i)
{
    if (i < 0 || i > 9) return;
    std::string where = panel().address();
    while (where.size() > 1 && where.back() == '/') where.pop_back();
    settings().favorites[(size_t)i] = where;
    settings().save();
    rebuild_favorites_menu();
}

void MainWindow::cmd_goto_favorite(int i)
{
    if (i < 0 || i > 9) return;
    const std::string f = settings().favorites[(size_t)i];
    if (!f.empty()) panel().navigate_to(window(), f);
}

// ---------- File menu: open with a type, View / Edit ----------

void MainWindow::cmd_open_inside_type(const std::string &type)
{
    auto r = panel().focused();
    if (!r || r->dir || r->up) return;
    if (panel().in_archive()) {
        show_message(window(), "7zipl", "Open Inside * and Open Inside # work on files in folders.");
        return;
    }
    panel().open_archive(window(), r->full, {}, nullptr, type);
}

namespace {
// Files of `rows` as paths on disk: themselves in a folder, extracted to a temporary folder in an archive.
void materialize(GtkWindow *parent, Panel &p, const std::vector<Row> &rows, std::function<void(const std::vector<std::string> &)> cb)
{
    if (!p.in_archive()) {
        cb(paths_of(rows));
        return;
    }
    p.extract_to_temp(parent, rows, std::move(cb));
}
}  // namespace

void MainWindow::cmd_view(bool edit)
{
    auto rows = files_only(panel().selected());
    if (rows.empty()) {
        auto f = panel().focused();
        if (f && !f->dir && !f->up) rows.push_back(*f);
    }
    if (rows.empty()) return;
    const std::string cmd = edit ? settings().editor : settings().viewer;
    const char *fallback = edit ? "text/plain" : nullptr;
    if (!panel().in_archive()) {
        launch_with(cmd, paths_of(rows), fallback);
        return;
    }
    const ArcLevel &a = panel().arc();
    const bool track = edit && a.can_update;
    const std::string archive = a.file, password = a.password, format = a.format;
    panel().extract_to_temp(window(), rows, [=](const std::vector<std::string> &files) {
        launch_with(cmd, files, fallback);
        if (!track) return;
        for (size_t i = 0; i < files.size() && i < rows.size(); i++) {
            const std::string &full = rows[i].full;
            const std::string dir = full.find('/') == std::string::npos ? std::string() : path_dirname(full);
            watch_edited(files[i], archive, dir, password, format);
        }
    });
}

void MainWindow::launch_with(const std::string &command, const std::vector<std::string> &files, const char *fallback_type)
{
    if (files.empty()) return;
    if (command.empty()) {
        GAppInfo *app = fallback_type ? g_app_info_get_default_for_type(fallback_type, FALSE) : nullptr;
        if (!app) {
            for (const auto &f : files) launch_file(window(), f);
            return;
        }
        GList *list = nullptr;
        for (const auto &f : files) list = g_list_append(list, g_file_new_for_path(f.c_str()));
        GdkAppLaunchContext *ctx = gdk_display_get_app_launch_context(gtk_widget_get_display(win_));
        GError *e = nullptr;
        if (!g_app_info_launch(app, list, G_APP_LAUNCH_CONTEXT(ctx), &e)) {
            show_message(window(), "7zipl", std::string("Cannot start ") + g_app_info_get_display_name(app) + ": " + (e ? e->message : ""), true);
            if (e) g_error_free(e);
        }
        g_list_free_full(list, g_object_unref);
        g_object_unref(ctx);
        g_object_unref(app);
        return;
    }
    std::vector<std::string> argv;
    bool placed = false;
    for (const auto &a : split_args(command)) {
        if (a == "%f" || a == "%F") {
            for (const auto &f : files) argv.push_back(f);
            placed = true;
        } else {
            argv.push_back(a);
        }
    }
    if (argv.empty()) return;
    if (!placed)
        for (const auto &f : files) argv.push_back(f);
    std::vector<char *> cargv;
    for (auto &a : argv) cargv.push_back(a.data());
    cargv.push_back(nullptr);
    GError *e = nullptr;
    if (!g_spawn_async(nullptr, cargv.data(), nullptr, G_SPAWN_SEARCH_PATH, nullptr, nullptr, nullptr, &e)) {
        show_message(window(), "7zipl", "Cannot start " + argv[0] + ": " + (e ? e->message : "") +
                                            "\n\nCheck Tools > Options > Editor.", true);
        if (e) g_error_free(e);
    }
}

namespace {
struct EditWatch {
    MainWindow *w;
    std::string file, archive, dir, password, format, stamp;
    guint timer = 0;
    bool asking = false;
};
}  // namespace

// Edit (F4) inside an archive: when the editor saves the temporary copy, ask to put it back, like 7-Zip.
void MainWindow::watch_edited(const std::string &file, const std::string &archive, const std::string &dir,
                              const std::string &password, const std::string &format)
{
    GFile *f = g_file_new_for_path(file.c_str());
    GFileMonitor *m = g_file_monitor_file(f, G_FILE_MONITOR_WATCH_MOVES, nullptr, nullptr);
    g_object_unref(f);
    if (!m) return;
    auto *ew = new EditWatch{this, file, archive, dir, password, format, file_stamp(file)};
    g_object_set_data_full(G_OBJECT(m), "watch", ew, [](gpointer p) {
        auto *ew = static_cast<EditWatch *>(p);
        if (ew->timer) g_source_remove(ew->timer);
        delete ew;
    });
    g_signal_connect(m, "changed", CB([](GFileMonitor *m, GFile *, GFile *, GFileMonitorEvent ev, gpointer) {
                         if (ev == G_FILE_MONITOR_EVENT_DELETED || ev == G_FILE_MONITOR_EVENT_ATTRIBUTE_CHANGED) return;
                         auto *ew = static_cast<EditWatch *>(g_object_get_data(G_OBJECT(m), "watch"));
                         if (ew->timer) g_source_remove(ew->timer);
                         ew->timer = g_timeout_add(600, [](gpointer p) -> gboolean {
                             auto *ew = static_cast<EditWatch *>(p);
                             ew->timer = 0;
                             const std::string st = file_stamp(ew->file);
                             if (ew->asking || st.empty() || st == ew->stamp) return G_SOURCE_REMOVE;
                             ew->stamp = st;
                             ew->asking = true;
                             MainWindow *w = ew->w;
                             ask_yes_no(w->window(), "7zipl",
                                        "File '" + path_basename(ew->file) + "' was modified.\nDo you want to update it in the archive?\n\n" +
                                            ew->archive,
                                        [ew, w](bool yes) {
                                            ew->asking = false;
                                            if (!yes) return;
                                            std::vector<std::string> args = update_args("a", ew->format, ew->password);
                                            args.push_back("-spd");
                                            args.push_back("--");
                                            args.push_back(ew->archive);
                                            if (ew->dir.empty()) {
                                                args.push_back(ew->file);
                                            } else {
                                                const std::string top = stage_for_prefix(ew->dir, {ew->file}, {});
                                                if (top.empty()) return;
                                                args.push_back(top);
                                            }
                                            JobSpec spec;
                                            spec.title = "Updating";
                                            spec.archive = ew->archive;
                                            spec.password = ew->password;
                                            spec.show_delay_ms = 300;
                                            start_job(w->window(), spec, [args](Ui &ui) { return run(args, ui); },
                                                      [w](const Result &) { w->refresh_all(); });
                                        });
                             return G_SOURCE_REMOVE;
                         }, ew);
                     }),
                     nullptr);
    edit_monitors_.push_back(m);
}

void MainWindow::refresh_all() { after_change(); }

// ---------- Split / Combine ----------

Result split_file(Ui &ui, const std::string &src, const std::string &dir, const std::string &name, uint64_t vsize)
{
                                                   Result res;
                                                   g_mkdir_with_parents(dir.c_str(), 0755);
                                                   FILE *in = g_fopen(src.c_str(), "rb");
                                                   if (!in) {
                                                       res.errors++;
                                                       res.messages.push_back("Cannot open " + src);
                                                       return res;
                                                   }
                                                   GStatBuf st;
                                                   g_stat(src.c_str(), &st);
                                                   ui.set_total((uint64_t)st.st_size);
                                                   std::vector<char> buf(1 << 20);
                                                   uint64_t done = 0;
                                                   for (unsigned part = 1;; part++) {
                                                       char ext[16];
                                                       g_snprintf(ext, sizeof ext, ".%03u", part);
                                                       const std::string out = path_join(dir, name + ext);
                                                       uint64_t left = vsize;
                                                       FILE *o = nullptr;
                                                       size_t got = 0;
                                                       while (left && (got = fread(buf.data(), 1, (size_t)std::min<uint64_t>(left, buf.size()), in)) > 0) {
                                                           if (!o) {
                                                               ui.set_current("Splitting", out);
                                                               o = g_fopen(out.c_str(), "wb");
                                                               if (!o) {
                                                                   res.errors++;
                                                                   res.messages.push_back("Cannot create " + out);
                                                                   fclose(in);
                                                                   return res;
                                                               }
                                                           }
                                                           if (fwrite(buf.data(), 1, got, o) != got) {
                                                               res.errors++;
                                                               res.messages.push_back("Write error: " + out);
                                                               fclose(o);
                                                               fclose(in);
                                                               return res;
                                                           }
                                                           left -= got;
                                                           done += got;
                                                           ui.set_completed(done);
                                                           if (ui.cancelled()) {
                                                               fclose(o);
                                                               fclose(in);
                                                               res.cancelled = true;
                                                               return res;
                                                           }
                                                       }
                                                       if (!o) break;
                                                       fclose(o);
                                                       ui.file_done();
                                                       res.files++;
                                                       if (got == 0 && left) break;
                                                   }
                                                   fclose(in);
                                                   res.ok = true;
                                                   return res;
                                               }

Result combine_files(Ui &ui, const std::vector<std::string> &parts, const std::string &out, uint64_t total)
{
                                   Result res;
                                   g_mkdir_with_parents(path_dirname(out).c_str(), 0755);
                                   FILE *o = g_fopen(out.c_str(), "wb");
                                   if (!o) {
                                       res.errors++;
                                       res.messages.push_back("Cannot create " + out);
                                       return res;
                                   }
                                   ui.set_total(total);
                                   ui.set_num_files(parts.size());
                                   std::vector<char> buf(1 << 20);
                                   uint64_t done = 0;
                                   for (const auto &p : parts) {
                                       ui.set_current("Combining", p);
                                       FILE *in = g_fopen(p.c_str(), "rb");
                                       if (!in) {
                                           res.errors++;
                                           res.messages.push_back("Cannot open " + p);
                                           break;
                                       }
                                       size_t got;
                                       while ((got = fread(buf.data(), 1, buf.size(), in)) > 0) {
                                           if (fwrite(buf.data(), 1, got, o) != got) {
                                               res.errors++;
                                               res.messages.push_back("Write error: " + out);
                                               break;
                                           }
                                           done += got;
                                           ui.set_completed(done);
                                           if (ui.cancelled()) break;
                                       }
                                       fclose(in);
                                       ui.file_done();
                                       if (res.errors || ui.cancelled()) break;
                                   }
                                   fclose(o);
                                   res.cancelled = ui.cancelled();
                                   if (res.errors || res.cancelled) g_unlink(out.c_str());
                                   res.ok = !res.errors && !res.cancelled;
                                   return res;
                               }


void MainWindow::cmd_split()
{
    auto rows = files_only(panel().selected());
    if (rows.empty()) {
        auto f = panel().focused();
        if (f && !f->dir && !f->up) rows.push_back(*f);
    }
    if (panel().in_archive() || rows.size() != 1) {
        show_message(window(), "7zipl", "Select one file to split.");
        return;
    }
    const Row r = rows[0];
    GtkWidget *w = dialog_window(window(), "Split File", 460, -1);
    GtkWidget *box = vbox(6);
    set_margins(box, 12);
    gtk_box_append(GTK_BOX(box), label("_Split " + r.name + " to:"));
    GtkWidget *dest = folder_row(GTK_WINDOW(w), box, two_ && !other_panel().in_archive() ? other_panel().fs_dir() : panel().fs_dir());
    gtk_box_append(GTK_BOX(box), label("Split to _volumes, bytes:"));
    GtkWidget *vol = gtk_entry_new();
    gtk_editable_set_text(GTK_EDITABLE(vol), "10M");
    gtk_entry_set_activates_default(GTK_ENTRY(vol), TRUE);
    GtkWidget *preset = dropdown({"Presets", "10M", "100M", "650M - CD", "700M - CD", "1000M", "4092M - FAT", "4480M - DVD",
                                  "8128M - DVD DL", "23040M - BD"});
    g_object_set_data(G_OBJECT(preset), "entry", vol);
    g_signal_connect(preset, "notify::selected", CB([](GtkWidget *dd, GParamSpec *, gpointer) {
                         if (gtk_drop_down_get_selected(GTK_DROP_DOWN(dd)) == 0) return;
                         gtk_editable_set_text(GTK_EDITABLE(g_object_get_data(G_OBJECT(dd), "entry")), dropdown_text(dd).c_str());
                     }),
                     nullptr);
    GtkWidget *vr = hbox(6);
    gtk_widget_set_hexpand(vol, TRUE);
    gtk_box_append(GTK_BOX(vr), vol);
    gtk_box_append(GTK_BOX(vr), preset);
    gtk_box_append(GTK_BOX(box), vr);
    auto b = button_row(box, {"OK", "Cancel"});
    g_signal_connect_swapped(b[1], "clicked", G_CALLBACK(gtk_window_destroy), w);
    struct D {
        MainWindow *mw;
        Row r;
        GtkWidget *w, *dest, *vol;
    };
    auto *d = new D{this, r, w, dest, vol};
    g_object_set_data_full(G_OBJECT(w), "d", d, [](gpointer p) { delete static_cast<D *>(p); });
    g_signal_connect_swapped(b[0], "clicked", CB([](D *d) {
                                 const uint64_t vsize = parse_size(gtk_editable_get_text(GTK_EDITABLE(d->vol)));
                                 const std::string dir = trim(gtk_editable_get_text(GTK_EDITABLE(d->dest)));
                                 if (!vsize) {
                                     show_message(GTK_WINDOW(d->w), "7zipl", "Incorrect volume size", true);
                                     return;
                                 }
                                 if (dir.empty()) return;
                                 const uint64_t n = d->r.size / vsize + (d->r.size % vsize ? 1 : 0);
                                 MainWindow *mw = d->mw;
                                 const std::string src = d->r.full, name = d->r.name;
                                 auto go = [mw, src, name, dir, vsize]() {
                                     JobSpec spec;
                                     spec.title = "Splitting";
                                     spec.archive = src;
                                     start_job(mw->window(), spec,
                                               [src, name, dir, vsize](Ui &ui) { return split_file(ui, src, dir, name, vsize); },
                                               [mw](const Result &) { mw->refresh_all(); });
                                 };
                                 gtk_window_destroy(GTK_WINDOW(d->w));  // frees d; the values were copied
                                 if (n > 100) {
                                     ask_yes_no(mw->window(), "Split File",
                                                "Are you sure you want to split the file into " + std::to_string(n) + " volumes?",
                                                [go](bool yes) {
                                                    if (yes) go();
                                                });
                                 } else {
                                     go();
                                 }
                             }),
                             d);
    gtk_window_set_child(GTK_WINDOW(w), box);
    gtk_window_present(GTK_WINDOW(w));
}

void MainWindow::cmd_combine()
{
    auto f = panel().focused();
    auto sel = files_only(panel().selected());
    if (sel.size() == 1) f = std::make_unique<Row>(sel[0]);
    if (panel().in_archive() || !f || f->dir || f->up) {
        show_message(window(), "7zipl", "Select the first part of a split file (name.001).");
        return;
    }
    // name.001, name.002, ... (any part selects the set)
    const std::string name = f->name;
    const auto dot = name.rfind('.');
    const std::string ext = dot == std::string::npos ? "" : name.substr(dot + 1);
    if (ext.size() < 3 || ext.find_first_not_of("0123456789") != std::string::npos) {
        show_message(window(), "7zipl", "Can't detect the file as a split file. Select a file named like name.001.", true);
        return;
    }
    const std::string stem = name.substr(0, dot), src_dir = path_dirname(f->full);
    const int width = (int)ext.size();
    std::vector<std::string> parts;
    uint64_t total = 0;
    for (unsigned i = 1;; i++) {
        char e[24];
        g_snprintf(e, sizeof e, ".%0*u", width, i);
        const std::string p = path_join(src_dir, stem + e);
        GStatBuf st;
        if (g_stat(p.c_str(), &st) != 0) break;
        parts.push_back(p);
        total += (uint64_t)st.st_size;
    }
    if (parts.size() < 1) {
        show_message(window(), "7zipl", "Cannot find the first part " + stem + "." + std::string((size_t)width - 1, '0') + "1", true);
        return;
    }
    const std::string def = two_ && !other_panel().in_archive() ? other_panel().fs_dir() : panel().fs_dir();
    ask_text(window(), "Combine Files",
             "Combine " + std::to_string(parts.size()) + " files (" + format_number(total) + " bytes) to " + stem + " in:", def,
             [this, parts, stem, total](const std::string &dir_in) {
                 const std::string dir = trim(dir_in);
                 if (dir.empty()) return;
                 const std::string out = path_join(dir, stem);
                 auto go = [this, parts, out, total]() {
                     JobSpec spec;
                     spec.title = "Combining";
                     spec.archive = out;
                     start_job(window(), spec,
                               [parts, out, total](Ui &ui) { return combine_files(ui, parts, out, total); },
                               [this](const Result &) { after_change(); });
                 };
                 if (g_file_test(out.c_str(), G_FILE_TEST_EXISTS)) {
                     ask_yes_no(window(), "Confirm File Replace", "The file already exists:\n" + out + "\n\nReplace it?",
                                [go](bool yes) {
                                    if (yes) go();
                                });
                 } else {
                     go();
                 }
             });
}

// ---------- Diff, Link ----------

void MainWindow::cmd_diff()
{
    std::vector<std::pair<Panel *, Row>> picks;
    auto sel = files_only(panel().selected());
    if (sel.size() == 2) {
        picks = {{&panel(), sel[0]}, {&panel(), sel[1]}};
    } else if (two_ && sel.size() <= 1) {
        // One file in each panel, like 7-Zip.
        std::unique_ptr<Row> a = sel.size() == 1 ? std::make_unique<Row>(sel[0]) : panel().focused();
        auto osel = files_only(other_panel().selected());
        std::unique_ptr<Row> b = osel.size() == 1 ? std::make_unique<Row>(osel[0]) : other_panel().focused();
        if (a && b && !a->dir && !b->dir && !a->up && !b->up) picks = {{&panel(), *a}, {&other_panel(), *b}};
    }
    if (picks.size() != 2) {
        show_message(window(), "Diff", "Select two files, or one file in each panel (View > 2 Panels).");
        return;
    }
    const std::string prog = diff_program();
    if (prog.empty()) {
        show_message(window(), "Diff",
                     "No diff program was found. Install Meld, Kompare or KDiff3, or set one in Tools > Options > Editor.", true);
        return;
    }
    Panel *p2 = picks[1].first;
    const Row r2 = picks[1].second;
    materialize(window(), *picks[0].first, {picks[0].second}, [this, p2, r2, prog](const std::vector<std::string> &a) {
        materialize(window(), *p2, {r2}, [this, a, prog](const std::vector<std::string> &b) {
            if (!a.empty() && !b.empty()) launch_with(prog, {a[0], b[0]}, nullptr);
        });
    });
}

void MainWindow::cmd_link()
{
    if (panel().in_archive()) return;
    auto r = panel().focused();
    const std::string target = r && !r->up ? r->full : std::string();
    const std::string dir = two_ && !other_panel().in_archive() ? other_panel().fs_dir() : panel().fs_dir();
    std::string from = target.empty() ? dir + "/" : path_join(dir, path_basename(target));
    if (from == target) from += " (link)";
    GtkWidget *w = dialog_window(window(), "Link", 480, -1);
    GtkWidget *box = vbox(6);
    set_margins(box, 12);
    gtk_box_append(GTK_BOX(box), label("Link _from (the new link):"));
    GtkWidget *e_from = gtk_entry_new();
    gtk_editable_set_text(GTK_EDITABLE(e_from), from.c_str());
    gtk_box_append(GTK_BOX(box), e_from);
    gtk_box_append(GTK_BOX(box), label("Link _to (the existing file or folder):"));
    GtkWidget *e_to = gtk_entry_new();
    gtk_editable_set_text(GTK_EDITABLE(e_to), target.c_str());
    gtk_box_append(GTK_BOX(box), e_to);
    GtkWidget *f = vbox(2);
    GtkWidget *sym = gtk_check_button_new_with_mnemonic("_Symbolic link");
    GtkWidget *rel = gtk_check_button_new_with_mnemonic("_Relative symbolic link");
    GtkWidget *hard = gtk_check_button_new_with_mnemonic("_Hard link");
    gtk_check_button_set_group(GTK_CHECK_BUTTON(rel), GTK_CHECK_BUTTON(sym));
    gtk_check_button_set_group(GTK_CHECK_BUTTON(hard), GTK_CHECK_BUTTON(sym));
    gtk_check_button_set_active(GTK_CHECK_BUTTON(sym), TRUE);
    for (GtkWidget *c : {sym, rel, hard}) gtk_box_append(GTK_BOX(f), c);
    gtk_box_append(GTK_BOX(box), frame("Link type", f));
    auto b = button_row(box, {"OK", "Cancel"});
    g_signal_connect_swapped(b[1], "clicked", G_CALLBACK(gtk_window_destroy), w);
    struct D {
        MainWindow *mw;
        GtkWidget *w, *from, *to, *rel, *hard;
    };
    auto *d = new D{this, w, e_from, e_to, rel, hard};
    g_object_set_data_full(G_OBJECT(w), "d", d, [](gpointer p) { delete static_cast<D *>(p); });
    g_signal_connect_swapped(b[0], "clicked", CB([](D *d) {
                                 const std::string from = trim(gtk_editable_get_text(GTK_EDITABLE(d->from)));
                                 const std::string to = trim(gtk_editable_get_text(GTK_EDITABLE(d->to)));
                                 if (from.empty() || to.empty() || from.back() == '/') return;
                                 if (g_file_test(from.c_str(), G_FILE_TEST_EXISTS) || g_file_test(from.c_str(), G_FILE_TEST_IS_SYMLINK)) {
                                     show_message(GTK_WINDOW(d->w), "Link", "The file already exists:\n" + from, true);
                                     return;
                                 }
                                 int rc;
                                 if (gtk_check_button_get_active(GTK_CHECK_BUTTON(d->hard)))
                                     rc = link(to.c_str(), from.c_str());
                                 else if (gtk_check_button_get_active(GTK_CHECK_BUTTON(d->rel)))
                                     rc = symlink(relative_path(path_dirname(from), to).c_str(), from.c_str());
                                 else
                                     rc = symlink(to.c_str(), from.c_str());
                                 if (rc != 0) {
                                     show_message(GTK_WINDOW(d->w), "Link", std::string("Cannot create the link: ") + g_strerror(errno), true);
                                     return;
                                 }
                                 MainWindow *mw = d->mw;
                                 gtk_window_destroy(GTK_WINDOW(d->w));
                                 mw->refresh_all();
                             }),
                             d);
    gtk_window_set_child(GTK_WINDOW(w), box);
    gtk_window_present(GTK_WINDOW(w));
}

// ---------- View > Folders History ----------

namespace {
void history_go(GtkWidget *w)
{
    GtkListBoxRow *row = gtk_list_box_get_selected_row(GTK_LIST_BOX(g_object_get_data(G_OBJECT(w), "list")));
    auto *mw = static_cast<MainWindow *>(g_object_get_data(G_OBJECT(w), "mw"));
    if (!row) return;
    const std::string path = gtk_label_get_text(GTK_LABEL(gtk_list_box_row_get_child(row)));
    gtk_window_destroy(GTK_WINDOW(w));
    mw->panel().navigate_to(mw->window(), path);
    mw->panel().focus();
}
}  // namespace

void MainWindow::cmd_folders_history()
{
    GtkWidget *w = dialog_window(window(), "Folders History", 520, 400);
    GtkWidget *box = vbox(6);
    set_margins(box, 8);
    GtkWidget *list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_SINGLE);
    for (const auto &f : settings().folder_history) {
        GtkWidget *l = label(f);
        gtk_label_set_ellipsize(GTK_LABEL(l), PANGO_ELLIPSIZE_MIDDLE);
        set_margins(l, 3);
        gtk_list_box_append(GTK_LIST_BOX(list), l);
    }
    gtk_list_box_select_row(GTK_LIST_BOX(list), gtk_list_box_get_row_at_index(GTK_LIST_BOX(list), 0));
    GtkWidget *sw = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sw), list);
    gtk_widget_set_vexpand(sw, TRUE);
    gtk_box_append(GTK_BOX(box), sw);
    auto b = button_row(box, {"OK", "Cancel", "_Delete"});
    g_object_set_data(G_OBJECT(w), "list", list);
    g_object_set_data(G_OBJECT(w), "mw", this);
    g_signal_connect_swapped(b[0], "clicked", G_CALLBACK(history_go), w);
    g_signal_connect(list, "row-activated", CB([](GtkListBox *l, GtkListBoxRow *) { history_go(GTK_WIDGET(gtk_widget_get_root(GTK_WIDGET(l)))); }), nullptr);
    g_signal_connect_swapped(b[1], "clicked", G_CALLBACK(gtk_window_destroy), w);
    g_signal_connect_swapped(b[2], "clicked", CB([](GtkWidget *list) {
                                 GtkListBoxRow *row = gtk_list_box_get_selected_row(GTK_LIST_BOX(list));
                                 if (!row) return;
                                 auto &h = settings().folder_history;
                                 const int i = gtk_list_box_row_get_index(row);
                                 if (i >= 0 && i < (int)h.size()) h.erase(h.begin() + i);
                                 gtk_list_box_remove(GTK_LIST_BOX(list), GTK_WIDGET(row));
                                 settings().save();
                             }),
                             list);
    gtk_window_set_child(GTK_WINDOW(w), box);
    gtk_window_present(GTK_WINDOW(w));
}

// ---------- Tools ----------

void MainWindow::cmd_options(int page)
{
    options_dialog(window(), [this] {
        apply_view_settings();
        after_change();
    }, page);
}

namespace {

struct BenchState {
    std::atomic<bool> stop{false}, alive{true}, running{false}, closing{false};
    GtkWidget *win = nullptr, *text = nullptr, *dict = nullptr, *threads = nullptr, *passes = nullptr, *start = nullptr,
              *stopb = nullptr;
};

class BenchUi final : public Ui {
public:
    explicit BenchUi(std::shared_ptr<BenchState> s) : st(std::move(s)) {}
    void set_total(uint64_t) override {}
    void set_completed(uint64_t) override {}
    void set_ratio(uint64_t, uint64_t) override {}
    void set_num_files(uint64_t) override {}
    void file_done() override {}
    void set_current(const std::string &, const std::string &) override {}
    bool cancelled() override { return st->stop; }
    bool ask_password(const std::string &, std::string &) override { return false; }
    Overwrite ask_overwrite(const std::string &, uint64_t, int64_t, const std::string &, uint64_t, int64_t) override
    {
        return Overwrite::Cancel;
    }
    void message(Level, const std::string &) override {}
    std::shared_ptr<BenchState> st;
};

void bench_append(BenchState *st, const std::string &s)
{
    GtkTextBuffer *b = gtk_text_view_get_buffer(GTK_TEXT_VIEW(st->text));
    GtkTextIter end;
    gtk_text_buffer_get_end_iter(b, &end);
    gtk_text_buffer_insert(b, &end, s.c_str(), -1);
    gtk_text_buffer_get_end_iter(b, &end);
    GtkTextMark *m = gtk_text_buffer_create_mark(b, nullptr, &end, FALSE);
    gtk_text_view_scroll_mark_onscreen(GTK_TEXT_VIEW(st->text), m);
    gtk_text_buffer_delete_mark(b, m);
}

void bench_start(std::shared_ptr<BenchState> st)
{
    if (st->running) return;
    st->running = true;
    st->stop = false;
    gtk_widget_set_sensitive(st->start, FALSE);
    gtk_widget_set_sensitive(st->stopb, TRUE);
    gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(st->text)), "", -1);
    const unsigned dsel = gtk_drop_down_get_selected(GTK_DROP_DOWN(st->dict));
    const unsigned dict_log = dsel == 0 ? 0 : 21 + dsel;  // 4 MB = 2^22
    const unsigned tsel = gtk_drop_down_get_selected(GTK_DROP_DOWN(st->threads));
    const unsigned threads = tsel;  // 0 = all
    static const unsigned kPasses[] = {1, 2, 3, 5, 10};
    const unsigned passes = kPasses[std::min(4u, gtk_drop_down_get_selected(GTK_DROP_DOWN(st->passes)))];
    std::thread([st, threads, dict_log, passes] {
        BenchUi ui(st);
        std::string pending;
        gint64 last = 0;
        auto flush = [&](bool force) {
            const gint64 now = g_get_monotonic_time();
            if (pending.empty() || (!force && now - last < 100000 && pending.find('\n') == std::string::npos)) return;
            last = now;
            std::string text;
            text.swap(pending);
            run_on_main([st, text] {
                if (st->alive) bench_append(st.get(), text);
            });
        };
        Result r = benchmark(threads, dict_log, passes, ui, [&](const std::string &s) {
            pending += s;
            flush(false);
        });
        flush(true);
        const bool stopped = st->stop;
        run_on_main([st, r, stopped] {
            st->running = false;
            if (!st->alive) return;
            if (st->closing) {
                gtk_window_destroy(GTK_WINDOW(st->win));
                return;
            }
            if (stopped) bench_append(st.get(), "\nStopped.\n");
            else if (!r.ok && !r.messages.empty()) bench_append(st.get(), "\n" + r.messages[0] + "\n");
            gtk_widget_set_sensitive(st->start, TRUE);
            gtk_widget_set_sensitive(st->stopb, FALSE);
        });
    }).detach();
}

}  // namespace

void MainWindow::cmd_benchmark()
{
    auto st = std::make_shared<BenchState>();
    GtkWidget *w = dialog_window(window(), "Benchmark", 760, 560);
    gtk_window_set_modal(GTK_WINDOW(w), FALSE);
    st->win = w;
    GtkWidget *box = vbox(6);
    set_margins(box, 8);
    GtkWidget *opts = hbox(8);
    std::vector<std::string> dicts = {"Default"};
    for (unsigned l = 22; l <= 30; l++) dicts.push_back(format_size_short(1ull << l));
    st->dict = dropdown(dicts);
    const unsigned n = g_get_num_processors();
    std::vector<std::string> th = {"All (" + std::to_string(n) + ")"};
    for (unsigned i = 1; i <= n; i++) th.push_back(std::to_string(i));
    st->threads = dropdown(th);
    st->passes = dropdown({"1", "2", "3", "5", "10"}, 2);
    gtk_box_append(GTK_BOX(opts), label("_Dictionary size:"));
    gtk_box_append(GTK_BOX(opts), st->dict);
    gtk_box_append(GTK_BOX(opts), label("_Number of CPU threads:"));
    gtk_box_append(GTK_BOX(opts), st->threads);
    gtk_box_append(GTK_BOX(opts), label("_Passes:"));
    gtk_box_append(GTK_BOX(opts), st->passes);
    gtk_box_append(GTK_BOX(box), opts);
    st->text = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(st->text), FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(st->text), TRUE);
    gtk_widget_add_css_class(st->text, "szl-mono");
    GtkWidget *sw = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sw), st->text);
    gtk_widget_set_vexpand(sw, TRUE);
    gtk_box_append(GTK_BOX(box), sw);
    gtk_box_append(GTK_BOX(box), label("Ratings are in MIPS (million instructions per second): LZMA compression and "
                                       "decompression speed, scaled to a reference CPU. Same numbers as 7z b."));
    auto b = button_row(box, {"_Restart", "_Stop", "_Close"});
    st->start = b[0];
    st->stopb = b[1];
    auto *holder = new std::shared_ptr<BenchState>(st);
    g_object_set_data_full(G_OBJECT(w), "bench", holder, [](gpointer p) {
        auto *h = static_cast<std::shared_ptr<BenchState> *>(p);
        (*h)->alive = false;
        (*h)->stop = true;
        delete h;
    });
    g_signal_connect_swapped(b[0], "clicked", CB([](std::shared_ptr<BenchState> *h) { bench_start(*h); }), holder);
    g_signal_connect_swapped(b[1], "clicked", CB([](std::shared_ptr<BenchState> *h) { (*h)->stop = true; }), holder);
    g_signal_connect_swapped(b[2], "clicked", G_CALLBACK(gtk_window_close), w);
    // Closing while it runs: stop, hide, and destroy the window when the worker has finished.
    g_signal_connect(w, "close-request", CB([](GtkWindow *w, std::shared_ptr<BenchState> *h) -> gboolean {
                         if (!(*h)->running) return FALSE;
                         (*h)->stop = true;
                         (*h)->closing = true;
                         gtk_widget_set_visible(GTK_WIDGET(w), FALSE);
                         return TRUE;
                     }),
                     holder);
    gtk_window_set_child(GTK_WINDOW(w), box);
    gtk_window_present(GTK_WINDOW(w));
    bench_start(st);
}

void MainWindow::cmd_delete_temp()
{
    // Folders of 7zipl instances that are no longer running ("<tag>-<pid>-XXXXXX").
    const std::string root = temp_root();
    std::vector<std::string> dirs;
    uint64_t size = 0, files = 0;
    GDir *d = g_dir_open(root.c_str(), 0, nullptr);
    const char *n;
    while (d && (n = g_dir_read_name(d))) {
        const auto parts = split(n, '-');
        if (parts.size() >= 3) {
            const long pid = atol(parts[parts.size() - 2].c_str());
            if (pid == (long)getpid()) continue;
            if (pid > 0 && g_file_test(("/proc/" + std::to_string(pid)).c_str(), G_FILE_TEST_IS_DIR)) continue;
        }
        const std::string p = path_join(root, n);
        size += tree_size(p, files);
        dirs.push_back(p);
    }
    if (d) g_dir_close(d);
    if (dirs.empty()) {
        show_message(window(), "Delete Temporary Files", "There are no temporary files to delete in\n" + root);
        return;
    }
    ask_yes_no(window(), "Delete Temporary Files",
               "Delete " + std::to_string(dirs.size()) + " temporary folders (" + format_number(files) + " files, " +
                   format_size_short(size) + ") left by 7zipl in\n" + root + "?",
               [dirs](bool yes) {
                   if (!yes) return;
                   for (const auto &p : dirs) remove_tree(p);
               });
}

void MainWindow::cmd_help()
{
    GtkUriLauncher *l = gtk_uri_launcher_new("https://github.com/TheElevatedOne/7zipl#readme");
    gtk_uri_launcher_launch(l, window(), nullptr, nullptr, nullptr);
    g_object_unref(l);
}

}  // namespace szl
