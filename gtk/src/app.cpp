// SPDX-License-Identifier: LGPL-2.1-or-later
#include "app.hpp"

#include "compressdialog.hpp"
#include "dialogs.hpp"
#include "engine.hpp"
#include "extractdialog.hpp"
#include "gtkutil.hpp"
#include "job.hpp"
#include "mainwindow.hpp"
#include "settings.hpp"
#include "style.hpp"
#include "util.hpp"

#include <memory>

namespace szl {

int run_selftest(GtkApplication *app);

namespace {

struct AppState {
    AppOptions opts;
    std::vector<std::unique_ptr<MainWindow>> windows;
    int exit_code = 0;
};

// Renders the window to a PNG (README screenshots, visual checks without a screen).
void save_screenshot(GtkWidget *w, const std::string &file)
{
    const int width = gtk_widget_get_width(w), height = gtk_widget_get_height(w);
    if (width <= 0 || height <= 0) return;
    GdkPaintable *p = gtk_widget_paintable_new(w);
    GtkSnapshot *s = gtk_snapshot_new();
    gdk_paintable_snapshot(p, s, width, height);
    GskRenderNode *node = gtk_snapshot_free_to_node(s);
    if (node) {
        GskRenderer *r = gtk_native_get_renderer(GTK_NATIVE(w));
        graphene_rect_t rect = GRAPHENE_RECT_INIT(0, 0, (float)width, (float)height);
        GdkTexture *t = gsk_renderer_render_texture(r, node, &rect);
        gdk_texture_save_to_png(t, file.c_str());
        g_object_unref(t);
        gsk_render_node_unref(node);
    }
    g_object_unref(p);
}

std::string common_dir(const std::vector<std::string> &files)
{
    return files.empty() ? home_dir() : path_dirname(files[0]);
}

void start_extract(const ExtractChoice &c, const std::vector<std::string> &archives)
{
    JobSpec spec;
    spec.title = "Extracting";
    spec.archive = archives.size() == 1 ? archives[0] : std::string();
    const auto args = extract_args(c, archives);
    start_job(nullptr, spec, [args](Ui &ui) { return run(args, ui); });
}

void start_compress(const CompressRequest &req)
{
    JobSpec spec;
    spec.title = "Compressing";
    spec.archive = req.archive;
    spec.show_ratio = true;
    const auto args = req.args;
    start_job(nullptr, spec, [args](Ui &ui) { return run(args, ui); });
}

void activate(GtkApplication *app, AppState *st)
{
    load_style();
    const AppOptions &o = st->opts;
    const auto &files = o.files;
    switch (o.mode) {
    case Mode::SelfTest:
        st->exit_code = run_selftest(app);
        return;
    case Mode::Browse: {
        auto w = std::make_unique<MainWindow>(app);
        w->open_path(files.empty() ? std::string() : files[0]);
        w->present();
        if (!o.screenshot.empty()) {
            struct S {
                GtkWidget *w;
                std::string file;
                GtkApplication *app;
            };
            auto *s = new S{GTK_WIDGET(w->window()), o.screenshot, app};
            g_timeout_add(1500, [](gpointer p) -> gboolean {
                auto *s = static_cast<S *>(p);
                save_screenshot(s->w, s->file);
                g_application_quit(G_APPLICATION(s->app));
                delete s;
                return G_SOURCE_REMOVE;
            }, s);
        }
        st->windows.push_back(std::move(w));
        return;
    }
    case Mode::Extract:
        extract_dialog(nullptr, common_dir(files), files, false,
                       [files](const ExtractChoice &c) { start_extract(c, files); });
        return;
    case Mode::ExtractHere:
    case Mode::ExtractToFolder: {
        ExtractChoice c;
        c.overwrite = settings().extract.overwrite;
        c.out_dir = common_dir(files);
        if (o.mode == Mode::ExtractToFolder) {
            c.out_dir = path_join(c.out_dir, files.size() == 1 ? strip_archive_ext(path_basename(files[0])) : "*");
            c.elim_dup = true;
        } else {
            c.elim_dup = false;
        }
        start_extract(c, files);
        return;
    }
    case Mode::Compress:
        compress_dialog(nullptr, common_dir(files), files, {}, [](const CompressRequest &r) { start_compress(r); });
        return;
    case Mode::CompressTo: {
        std::string stem = files.size() == 1 ? path_basename(files[0]) : path_basename(common_dir(files));
        if (files.size() == 1 && !g_file_test(files[0].c_str(), G_FILE_TEST_IS_DIR)) {
            const auto dot = stem.rfind('.');
            if (dot != std::string::npos && dot > 0) stem = stem.substr(0, dot);
        }
        const std::string ext = o.format == "gzip" ? "gz" : o.format == "bzip2" ? "bz2" : o.format;
        const std::string archive = path_join(common_dir(files), stem + "." + ext);
        start_compress(quick_compress(archive, files, o.format));
        return;
    }
    case Mode::Test: {
        JobSpec spec;
        spec.title = "Testing";
        spec.report_success = true;
        spec.archive = files.size() == 1 ? files[0] : std::string();
        ExtractChoice c;
        const auto args = extract_args(c, files, true);
        start_job(nullptr, spec, [args](Ui &ui) { return run(args, ui); });
        return;
    }
    case Mode::Hash: {
        JobSpec spec;
        spec.title = "Checksum calculating";
        std::vector<std::string> args = {"h", "-scrc" + o.hash, "-spd", "--"};
        for (const auto &f : files) args.push_back(f);
        start_job(nullptr, spec, [args](Ui &ui) { return run(args, ui); }, [](const Result &r) {
            if (!r.cancelled && !r.hash.methods.empty()) hash_window(nullptr, r.hash);
        });
        return;
    }
    }
}

}  // namespace

int run_app(const char *argv0, const AppOptions &opts)
{
    use_window_class();
    init(argv0);
    settings().load();
    AppState st;
    st.opts = opts;
    GtkApplication *app = gtk_application_new(SZL_APP_ID, G_APPLICATION_NON_UNIQUE);
    g_signal_connect(app, "activate", G_CALLBACK(activate), &st);
    const char *argv[] = {argv0, nullptr};
    int status = g_application_run(G_APPLICATION(app), 1, const_cast<char **>(argv));
    st.windows.clear();
    g_object_unref(app);
    settings().save();
    cleanup_temp_dirs();
    return opts.mode == Mode::SelfTest ? st.exit_code : status;
}

}  // namespace szl
