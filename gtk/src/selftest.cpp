// SPDX-License-Identifier: LGPL-2.1-or-later
// 7zipl --selftest: checks the engine glue (create, list, extract, test, rename, delete, hash,
// encryption) and drives the main window through an archive. Needs a display (Broadway works);
// exits 77 without one.
#include "engine.hpp"
#include "gtkutil.hpp"
#include "job.hpp"
#include "mainwindow.hpp"
#include "util.hpp"

#include <glib/gstdio.h>

#include <algorithm>
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
    void message(Level, const std::string &) override {}
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
    gui_tests(app, tmp);
    remove_tree(tmp);
    std::fprintf(stderr, "selftest: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}

}  // namespace szl
