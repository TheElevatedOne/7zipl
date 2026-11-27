// SPDX-License-Identifier: LGPL-2.1-or-later
// 7zipl --selftest: checks the engine glue (create, list, extract, test, rename, delete, hash,
// encryption) and drives the main window through an archive. Needs a display (Broadway works);
// exits 77 without one.
#include "engine.hpp"
#include "gtkutil.hpp"
#include "integration.hpp"
#include "settings.hpp"
#include "job.hpp"
#include "mainwindow.hpp"
#include "util.hpp"

#include <gio/gdesktopappinfo.h>
#include <glib/gstdio.h>

#include <algorithm>
#include <unistd.h>
#include <cstdio>
#include <functional>

namespace szl {

namespace {

int g_fail = 0, g_pass = 0;

void check(bool ok, const std::string &what)
{
    if (ok) {
        g_pass++;
    } else {
        g_fail++;
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    }
}

class TestUi final : public Ui {
public:
    std::string password;
    int password_asked = 0;
    void set_total(uint64_t) override {}
    void set_completed(uint64_t) override {}
    void set_ratio(uint64_t, uint64_t) override {}
    void set_num_files(uint64_t) override {}
    void file_done() override {}
    void set_current(const std::string &, const std::string &) override {}
    bool cancelled() override { return false; }
    bool ask_password(const std::string &, std::string &pw) override
    {
        password_asked++;
        if (password.empty()) return false;
        pw = password;
        return true;
    }
    Overwrite ask_overwrite(const std::string &, uint64_t, int64_t, const std::string &, uint64_t, int64_t) override
    {
        return Overwrite::YesAll;
    }
    void message(Level, const std::string &m) override { std::fprintf(stderr, "  engine: %s\n", m.c_str()); }
};

bool has_entry(const ArchiveListing &l, const std::string &path)
{
    return std::any_of(l.entries.begin(), l.entries.end(), [&](const Entry &e) { return e.path == path; });
}

std::string read_file(const std::string &p)
{
    char *data = nullptr;
    gsize len = 0;
    if (!g_file_get_contents(p.c_str(), &data, &len, nullptr)) return {};
    std::string s(data, len);
    g_free(data);
    return s;
}

void write_file(const std::string &p, const std::string &data)
{
    g_mkdir_with_parents(path_dirname(p).c_str(), 0755);
    g_file_set_contents(p.c_str(), data.data(), (gssize)data.size(), nullptr);
}

bool wait_for(const std::function<bool()> &pred, int timeout_ms = 20000)
{
    const gint64 end = g_get_monotonic_time() + timeout_ms * 1000;
    while (!pred()) {
        if (g_get_monotonic_time() > end) return false;
        g_main_context_iteration(nullptr, FALSE);
        g_usleep(2000);
    }
    return true;
}

void engine_tests(const std::string &tmp)
{
    check(available(), "7z.so loaded: " + program());
    check(!formats().empty(), "formats loaded");
    const auto up = update_formats();
    check(std::find(up.begin(), up.end(), "7z") != up.end(), "7z can be created");
    check(std::find(up.begin(), up.end(), "zip") != up.end(), "zip can be created");
    const auto hm = hash_methods();
    check(std::find(hm.begin(), hm.end(), "CRC32") != hm.end(), "CRC32 hasher");
    check(looks_like_archive("/x/a.7z") && looks_like_archive("a.tar.gz") && !looks_like_archive("a.txt"),
          "archive extensions");

    const std::string src = path_join(tmp, "src");
    write_file(path_join(src, "a.txt"), "123456789");
    write_file(path_join(src, "sub/b.txt"), "hello");
    std::string big(65536, '\0');
    for (size_t i = 0; i < big.size(); i++) big[i] = (char)(i * 131 % 251);
    write_file(path_join(src, "sub/deep/c.bin"), big);
    write_file(path_join(src, "we*ird?.txt"), "wild");

    TestUi ui;
    const std::string arc = path_join(tmp, "t.7z");
    Result r = run({"a", "-t7z", "-mx1", "-spd", "--", arc, path_join(src, "a.txt"), path_join(src, "sub"),
                    path_join(src, "we*ird?.txt")},
                   ui);
    check(r.ok && g_file_test(arc.c_str(), G_FILE_TEST_IS_REGULAR), "create 7z: " + r.summary);

    ArchiveListing l = open_archive(arc, ui);
    check(l.ok, "list 7z: " + l.error);
    check(l.format == "7z" && l.can_update, "7z format and updatable");
    check(has_entry(l, "a.txt") && has_entry(l, "sub/b.txt") && has_entry(l, "sub/deep/c.bin"), "7z entries");
    check(has_entry(l, "we*ird?.txt"), "wildcard characters stored literally");

    // Extract a folder from inside the archive with its prefix removed (7-Zip FM's Extract in a folder).
    ExtractRequest req;
    req.archive = arc;
    req.out_dir = path_join(tmp, "out1");
    req.items = {"sub/deep"};
    req.strip_prefix = "sub";
    r = extract_items(req, ui);
    check(r.ok, "extract items");
    check(read_file(path_join(tmp, "out1/deep/c.bin")) == big, "extracted data with prefix removed");
    check(!g_file_test(path_join(tmp, "out1/sub").c_str(), G_FILE_TEST_EXISTS), "prefix not created");

    // Whole archive through the command line path.
    r = run({"x", "-o" + path_join(tmp, "out2"), "-y", "--", arc}, ui);
    check(r.ok && read_file(path_join(tmp, "out2/sub/b.txt")) == "hello", "x whole archive");

    // Symbolic links (-snl): stored as links and recreated; links leaving the output folder are refused.
    {
        const std::string ld = path_join(tmp, "links");
        write_file(path_join(ld, "target.txt"), "t");
        check(symlink("target.txt", path_join(ld, "ok").c_str()) == 0 && symlink("/etc", path_join(ld, "evil").c_str()) == 0,
              "make test links");
        const std::string la = path_join(tmp, "links.7z");
        r = run({"a", "-snl", "-spd", "--", la, ld}, ui);
        check(r.ok, "add symbolic links: " + r.summary);
        const std::string lo = path_join(tmp, "out-links");
        r = run({"x", "-o" + lo, "--", la}, ui);
        gchar *t = g_file_read_link(path_join(lo, "links/ok").c_str(), nullptr);
        check(t && std::string(t) == "target.txt", "symbolic link recreated");
        g_free(t);
        check(!r.ok && !g_file_test(path_join(lo, "links/evil").c_str(), G_FILE_TEST_IS_SYMLINK),
              "link pointing outside the output folder refused");
    }

    req = {};
    req.archive = arc;
    req.test = true;
    check(extract_items(req, ui).ok, "test archive");
    check(run({"t", "--", arc}, ui).ok, "t command");

    r = run({"h", "-scrcCRC32", "-spd", "--", path_join(src, "a.txt")}, ui);
    check(r.ok && r.hash.files.size() == 1 && !r.hash.files[0].digests.empty() &&
              r.hash.files[0].digests[0] == "CBF43926",
          "CRC32 of 123456789");

    r = run({"rn", "-spd", "--", arc, "a.txt", "z.txt"}, ui);
    l = open_archive(arc, ui);
    check(r.ok && has_entry(l, "z.txt") && !has_entry(l, "a.txt"), "rename in archive");
    r = run({"rn", "-spd", "--", arc, "sub/deep", "sub/deeper"}, ui);
    l = open_archive(arc, ui);
    check(r.ok && has_entry(l, "sub/deeper/c.bin"), "rename folder in archive");
    r = run({"d", "-spd", "--", arc, "z.txt", "we*ird?.txt"}, ui);
    l = open_archive(arc, ui);
    check(r.ok && !has_entry(l, "z.txt") && !has_entry(l, "we*ird?.txt") && has_entry(l, "sub/b.txt"),
          "delete from archive");

    // Encrypted headers: a password is needed to list.
    const std::string enc = path_join(tmp, "enc.7z");
    r = run({"a", "-pSecret", "-mhe=on", "--", enc, path_join(src, "a.txt")}, ui);
    check(r.ok, "create encrypted 7z");
    TestUi wrong;
    wrong.password = "nope";
    l = open_archive(enc, wrong);
    check(!l.ok && wrong.password_asked > 0, "wrong password refused");
    TestUi right;
    right.password = "Secret";
    l = open_archive(enc, right);
    check(l.ok && has_entry(l, "a.txt") && l.password == "Secret", "right password lists");
    req = {};
    req.archive = enc;
    req.out_dir = path_join(tmp, "out3");
    req.password = "Secret";
    check(extract_items(req, ui).ok && read_file(path_join(tmp, "out3/a.txt")) == "123456789", "extract encrypted");

    // Other formats
    const std::string zip = path_join(tmp, "t.zip");
    check(run({"a", "-tzip", "--", zip, path_join(src, "sub")}, ui).ok, "create zip");
    l = open_archive(zip, ui);
    check(l.ok && l.format == "zip" && has_entry(l, "sub/b.txt"), "list zip");
    const std::string tar = path_join(tmp, "t.tar");
    check(run({"a", "-ttar", "--", tar, path_join(src, "sub")}, ui).ok, "create tar");
    const std::string tgz = path_join(tmp, "t.tar.gz");
    check(run({"a", "-tgzip", "--", tgz, tar}, ui).ok, "create tar.gz");
    l = open_archive(tgz, ui);
    check(l.ok && l.entries.size() == 1, "list gz (one stream)");

    // Errors are reported, not thrown.
    r = run({"x", "--", path_join(tmp, "missing.7z")}, ui);
    check(!r.ok && r.errors > 0, "missing archive is an error");
    r = run({"a", "-tnope", "--", path_join(tmp, "x.nope"), path_join(src, "a.txt")}, ui);
    check(!r.ok, "unknown format is an error");

    // Open Inside #: an archive after other data is found by the parser.
    // (a 7z: its signature must be at the start, unlike zip, which 7-Zip finds by its end record)
    const std::string wrapped = path_join(tmp, "wrapped.bin");
    write_file(wrapped, std::string(4096, 'x') + read_file(arc));
    l = open_archive(wrapped, ui, {}, "#");
    check(l.ok && !l.entries.empty(), "Open Inside # finds the embedded archive: " + l.error);

    // Benchmark (7z b), one short pass.
    std::string bench_out;
    r = benchmark(1, 22, 1, ui, [&](const std::string &t) { bench_out += t; });
    check(r.ok && bench_out.find("Tot:") != std::string::npos, "benchmark runs");

    // Split and Combine
    {
        const std::string big = std::string(250000, 'q') + "end";
        const std::string f = path_join(tmp, "split.bin");
        write_file(f, big);
        const std::string sd = path_join(tmp, "parts");
        r = split_file(ui, f, sd, "split.bin", 100000);
        check(r.ok && r.files == 3 && g_file_test(path_join(sd, "split.bin.003").c_str(), G_FILE_TEST_EXISTS) &&
                  !g_file_test(path_join(sd, "split.bin.004").c_str(), G_FILE_TEST_EXISTS),
              "split into 3 volumes");
        check(read_file(path_join(sd, "split.bin.001")).size() == 100000, "volume size");
        const std::string joined = path_join(tmp, "joined.bin");
        r = combine_files(ui, {path_join(sd, "split.bin.001"), path_join(sd, "split.bin.002"), path_join(sd, "split.bin.003")},
                          joined, big.size());
        check(r.ok && read_file(joined) == big, "combine gives the original file back");
        r = split_file(ui, f, sd, "exact.bin", big.size());
        check(r.ok && r.files == 1, "split with a volume as big as the file");
    }

    // Options > Folders: the working folder switch
    Settings &s = settings();
    s.work_mode = 1;
    check(s.work_args().empty(), "working folder: current");
    s.work_mode = 0;
    check(s.work_args().size() == 1 && s.work_args()[0] == "-w" + std::string(g_get_tmp_dir()), "working folder: temp");
    s.work_mode = 2;
    s.work_dir = tmp;
    check(s.work_args().size() == 1 && s.work_args()[0] == "-w" + tmp, "working folder: specified");
    r = run({"a", "-w" + tmp, "--", path_join(tmp, "w.7z"), path_join(src, "a.txt")}, ui);
    check(r.ok, "archive written through a working folder");
    s.work_mode = 1;
}

// Desktop integration, in the XDG sandbox main() set up.
void integration_tests()
{
    check(g_str_has_prefix(g_get_user_data_dir(), g_get_tmp_dir()) || strstr(g_get_user_data_dir(), "7zipl-selftest"),
          "integration tests run in a sandbox");
    if (!strstr(g_get_user_data_dir(), "7zipl-selftest")) return;

    const auto &fmts = assoc_formats();
    const AssocFormat *iso = nullptr, *z7 = nullptr;
    for (const auto &f : fmts) {
        if (f.name == "ISO") iso = &f;
        if (f.name == "7z") z7 = &f;
    }
    check(z7 && iso && !iso->mime_types.empty(), "association table has 7z and ISO");
    const auto dm = desktop_mime_types();
    check(std::find(dm.begin(), dm.end(), "application/x-cd-image") != dm.end(), "desktop entry lists ISO aliases");

    if (desktop_entry_state() == EntryState::System) {
        std::fprintf(stderr, "  (7zipl is installed system-wide; skipping the user entry tests)\n");
    } else {
        std::string err;
        check(install_user_entry(err), "install the user desktop entry: " + err);
        check(desktop_entry_state() == EntryState::User, "desktop entry state is User");
        const std::string entry = path_join(g_get_user_data_dir(), "applications/" SZL_APP_ID ".desktop");
        GDesktopAppInfo *ai = g_desktop_app_info_new_from_filename(entry.c_str());
        check(ai && g_strcmp0(g_app_info_get_executable(G_APP_INFO(ai)), self_exe().c_str()) == 0,
              "user entry runs this binary");
        if (ai) g_object_unref(ai);
        check(g_file_test(path_join(g_get_user_data_dir(), "icons/hicolor/scalable/apps/" SZL_APP_ID ".svg").c_str(),
                          G_FILE_TEST_IS_REGULAR),
              "icon installed from the built-in resource");
        // GIO notices the new entry through a file monitor.
        check(wait_for([] {
                  GDesktopAppInfo *a = g_desktop_app_info_new(SZL_APP_ID ".desktop");
                  if (a) g_object_unref(a);
                  return a != nullptr;
              }, 10000),
              "GIO sees the new entry");
        if (iso && z7) {
            GList *isoapps = g_app_info_get_all_for_type(iso->mime_types[0].c_str());
            int n_isoapps = 0;
            for (GList *l = isoapps; l; l = l->next)
                if (g_strcmp0(g_app_info_get_id(G_APP_INFO(l->data)), SZL_APP_ID ".desktop") != 0) n_isoapps++;
            g_list_free_full(isoapps, g_object_unref);
            check(n_isoapps == 0 || !is_default_for(*iso), "installing the entry doesn't take over ISO files");
            check(set_default(*iso, true, err), "make 7zipl the ISO default: " + err);
            check(is_default_for(*iso), "7zipl is the ISO default");
            check(set_default(*z7, true, err) && is_default_for(*z7), "7zipl is the 7z default");
            const std::string mimeapps = read_file(path_join(g_get_user_config_dir(), "mimeapps.list"));
            check(mimeapps.find(iso->mime_types[0] + "=" SZL_APP_ID ".desktop") != std::string::npos,
                  "written to mimeapps.list");
            GList *others = g_app_info_get_all_for_type(iso->mime_types[0].c_str());
            int n_other = 0;
            for (GList *l = others; l; l = l->next)
                if (g_strcmp0(g_app_info_get_id(G_APP_INFO(l->data)), SZL_APP_ID ".desktop") != 0) n_other++;
            g_list_free_full(others, g_object_unref);
            const bool back = set_default(*iso, false, err);
            GAppInfo *now = g_app_info_get_default_for_type(iso->mime_types[0].c_str(), FALSE);
            check(back && (!is_default_for(*iso) || n_other == 0),
                  "ISO given back to the system (default now " + std::string(now ? g_app_info_get_id(now) : "none") +
                      ", " + std::to_string(n_other) + " other programs)");
            if (now) g_object_unref(now);
            check(read_file(path_join(g_get_user_config_dir(), "mimeapps.list")).find(iso->mime_types[0] + "=" SZL_APP_ID) ==
                      std::string::npos,
                  "ISO association removed from mimeapps.list");
            check(is_default_for(*z7), "7z still 7zipl's");
        }
        check(remove_user_entry(), "remove the user entry");
        check(desktop_entry_state() == EntryState::Missing, "entry gone");
        if (z7)
            check(read_file(path_join(g_get_user_config_dir(), "mimeapps.list")).find(z7->mime_types[0] + "=" SZL_APP_ID) ==
                      std::string::npos,
                  "removing the entry takes its associations back");
    }

    // Context menus: every writer, all items, then removal. A user's own Beluga action survives.
    const std::string beluga = path_join(g_get_user_config_dir(), "beluga/actions.conf");
    write_file(beluga, "[Action 000]\nName=Mine\nCommand=echo %F\nFiles=true\n");
    for (const char *fm : {"dolphin", "nautilus", "nemo", "fma", "beluga"}) {
        for (bool cascaded : {true, false}) {
            std::string err;
            check(set_context_menu(fm, kCtxDefault, cascaded, err), std::string("context menu for ") + fm + ": " + err);
            bool inst = false;
            for (const auto &m : file_managers())
                if (m.id == fm) inst = m.installed;
            check(inst, std::string("context menu detected as installed: ") + fm);
        }
    }
    const std::string kio = path_join(g_get_user_data_dir(), "kio/servicemenus/7zipl-archives.desktop");
    GKeyFile *k = g_key_file_new();
    check(g_key_file_load_from_file(k, kio.c_str(), G_KEY_FILE_NONE, nullptr), "service menu is a valid key file");
    char *ex = g_key_file_get_string(k, "Desktop Action extract-here", "Exec", nullptr);
    check(ex && strstr(ex, "--extract-here %F"), "service menu runs --extract-here");
    g_free(ex);
    g_key_file_free(k);
    check(g_file_test(kio.c_str(), G_FILE_TEST_IS_EXECUTABLE), "service menu is executable (KF6 needs it)");
    check(read_file(beluga).find("Mine") != std::string::npos, "Beluga user action kept");
    for (const char *fm : {"dolphin", "nautilus", "nemo", "fma", "beluga"}) {
        std::string err;
        check(set_context_menu(fm, 0, true, err), std::string("remove context menu for ") + fm);
    }
    for (const auto &m : file_managers()) check(!m.installed, "context menu removed: " + m.id);
    check(read_file(beluga).find("Mine") != std::string::npos && read_file(beluga).find("7zipl") == std::string::npos,
          "Beluga: only 7zipl's actions removed");
    check(!g_file_test(path_join(g_get_user_data_dir(), "nautilus/scripts/7-Zip").c_str(), G_FILE_TEST_EXISTS),
          "Nautilus submenu folder removed");
}

void gui_tests(GtkApplication *app, const std::string &tmp)
{
    auto w = std::make_unique<MainWindow>(app);
    w->present();
    Panel &p = w->panel();
    const std::string src = path_join(tmp, "src");
    p.navigate_fs(src);
    check(!p.in_archive() && p.fs_dir() == src, "navigate to folder");
    auto rows = p.all_rows();
    check(std::any_of(rows.begin(), rows.end(), [](const Row &r) { return r.name == "sub" && r.dir; }), "folder rows");

    const std::string arc = path_join(tmp, "t.7z");
    bool opened = false, done = false;
    p.open_archive(w->window(), arc, {}, [&](bool ok) {
        opened = ok;
        done = true;
    });
    check(wait_for([&] { return done; }) && opened && p.in_archive(), "open archive in the panel");
    p.enter("sub");
    check(p.prefix() == "sub" && p.address() == arc + "/sub/", "enter a folder in the archive");
    rows = p.all_rows();
    check(std::any_of(rows.begin(), rows.end(), [](const Row &r) { return r.name == "b.txt" && r.has_size && r.size == 5; }),
          "archive folder rows");

    // Add a file into the open folder of the archive (staged through symlinks).
    w->add_files_to_archive({path_join(src, "a.txt")});
    check(wait_for([] { return running_jobs() == 0; }), "add job finished");
    check(wait_for([&] {
              auto rr = p.all_rows();
              return std::any_of(rr.begin(), rr.end(), [](const Row &r) { return r.name == "a.txt"; });
          }),
          "file added into the archive folder");
    TestUi ui;
    ArchiveListing l = open_archive(arc, ui);
    check(has_entry(l, "sub/a.txt"), "stored as sub/a.txt");

    p.go_up();
    check(p.in_archive() && p.prefix().empty(), "up to the archive root");
    p.go_up();
    check(!p.in_archive() && p.fs_dir() == tmp, "up out of the archive");

    // Typed path into a folder of an archive.
    done = false;
    p.navigate_to(w->window(), arc + "/sub/deeper");
    check(wait_for([&] { return p.in_archive(); }) && p.prefix() == "sub/deeper", "address bar path inside archive");

    // Nested archive: tar.gz -> tar -> files
    p.navigate_fs(tmp);
    done = false;
    p.open_archive(w->window(), path_join(tmp, "t.tar.gz"), {}, [&](bool ok) { done = ok; });
    check(wait_for([&] { return done; }), "open tar.gz");
    rows = p.all_rows();
    check(rows.size() == 1 && rows[0].name == "t.tar", "gz shows the tar inside");
    if (rows.size() == 1) {
        p.activate(w->window(), rows[0]);
        check(wait_for([&] { return p.depth() == 2; }), "open nested tar");
        rows = p.all_rows();
        check(std::any_of(rows.begin(), rows.end(), [](const Row &r) { return r.name == "sub"; }), "nested listing");
        p.go_up();
        check(p.depth() == 1, "up from nested archive");
    }

    // Flat view lists every file below the folder.
    p.navigate_fs(src);
    p.set_flat(true);
    rows = p.all_rows();
    check(std::any_of(rows.begin(), rows.end(), [](const Row &r) { return r.name == "sub/b.txt" && !r.dir; }) &&
              std::none_of(rows.begin(), rows.end(), [](const Row &r) { return r.dir; }),
          "flat view in a folder");
    p.set_flat(false);
    done = false;
    p.open_archive(w->window(), arc, {}, [&](bool ok) { done = ok; });
    check(wait_for([&] { return done; }), "reopen archive");
    p.set_flat(true);
    rows = p.all_rows();
    check(std::any_of(rows.begin(), rows.end(), [](const Row &r) { return r.name == "sub/deeper/c.bin"; }),
          "flat view in an archive");
    // Items of a flat view extract to a temporary folder (View / Edit / Diff).
    std::vector<Row> pick;
    for (const Row &r : rows)
        if (r.name == "sub/deeper/c.bin") pick.push_back(r);
    std::vector<std::string> got;
    p.extract_to_temp(w->window(), pick, [&](const std::vector<std::string> &f) { got = f; });
    check(wait_for([&] { return !got.empty(); }) && g_file_test(got[0].c_str(), G_FILE_TEST_IS_REGULAR) &&
              path_basename(got[0]) == "c.bin",
          "extract an item to a temporary folder");
    p.set_flat(false);

    // View modes and two panels.
    for (int m : {0, 1, 2, 3}) {
        p.set_view_mode(m);
        check(gtk_widget_get_parent(p.current_view()) != nullptr, "view mode " + std::to_string(m) + " shown");
    }
    w->set_two_panels(true);
    check(w->two_panels() && !w->other_panel().fs_dir().empty(), "two panels");
    // Copy from the archive to the other panel's folder.
    const std::string dest = path_join(tmp, "copydest");
    g_mkdir_with_parents(dest.c_str(), 0755);
    w->other_panel().navigate_fs(dest);
    p.enter("sub");
    p.select_name("b.txt");
    w->extract_selection_to(p.selected(), dest, false);
    check(wait_for([&] { return running_jobs() == 0 && g_file_test(path_join(dest, "b.txt").c_str(), G_FILE_TEST_EXISTS); }),
          "copy from an archive to the other panel");
    w->set_two_panels(false);

    // Edit (F4) inside an archive: saving the temporary copy asks to update the archive; Yes puts it back.
    {
        const std::string edited = path_join(make_temp_dir("edit"), "b.txt");
        write_file(edited, "hello");
        w->watch_edited(edited, arc, "sub", {}, "7z");
        g_usleep(1100000);  // a different modification time
        write_file(edited, "changed by an editor");
        GtkWidget *dlg = nullptr;
        check(wait_for([&] {
                  GListModel *tops = gtk_window_get_toplevels();
                  for (guint i = 0; i < g_list_model_get_n_items(tops); i++) {
                      GtkWindow *t = GTK_WINDOW(g_list_model_get_item(tops, i));
                      if (gtk_window_get_transient_for(t) == w->window() && gtk_widget_get_visible(GTK_WIDGET(t)))
                          dlg = GTK_WIDGET(t);
                      g_object_unref(t);
                  }
                  return dlg != nullptr;
              }, 10000),
              "edit-back asks to update the archive");
        if (dlg) {
            GtkWidget *yes = gtk_window_get_default_widget(GTK_WINDOW(dlg));
            check(yes != nullptr, "the question has a default (Yes) button");
            if (yes) gtk_widget_activate(yes);
            check(wait_for([&] {
                      if (running_jobs() != 0) return false;
                      TestUi u;
                      ArchiveListing al = open_archive(arc, u);
                      for (const Entry &e : al.entries)
                          if (e.path == "sub/b.txt") return e.size == 20;
                      return false;
                  }),
                  "edited file put back into the archive folder");
        }
    }
    gtk_window_destroy(w->window());
}

}  // namespace

int run_selftest(GtkApplication *app)
{
    const std::string tmp = make_temp_dir("selftest");
    if (tmp.empty()) {
        std::fprintf(stderr, "FAIL: temp dir\n");
        return 1;
    }
    engine_tests(tmp);
    integration_tests();
    gui_tests(app, tmp);
    remove_tree(tmp);
    std::fprintf(stderr, "selftest: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}

}  // namespace szl
