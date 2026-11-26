// SPDX-License-Identifier: LGPL-2.1-or-later
// 7zipl: 7-Zip File Manager for Linux with a GTK 4 front-end.
#include "app.hpp"
#include "engine.hpp"
#include "settings.hpp"
#include "util.hpp"

#include <gtk/gtk.h>

#include <cstdio>
#include <cstring>

int main(int argc, char **argv)
{
    gboolean version = FALSE, selftest = FALSE, extract = FALSE, extract_here = FALSE, extract_folder = FALSE;
    gboolean compress = FALSE, test = FALSE;
    char *compress_to = nullptr, *hash = nullptr, *profile = nullptr, *screenshot = nullptr;
    char **rest = nullptr;
    const GOptionEntry entries[] = {
        {"extract", 'x', 0, G_OPTION_ARG_NONE, &extract, "Show the Extract dialog for the archives", nullptr},
        {"extract-here", 0, 0, G_OPTION_ARG_NONE, &extract_here, "Extract the archives into their folder", nullptr},
        {"extract-to-folder", 0, 0, G_OPTION_ARG_NONE, &extract_folder,
         "Extract each archive into a folder named after it", nullptr},
        {"compress", 'a', 0, G_OPTION_ARG_NONE, &compress, "Show the Add to Archive dialog for the files", nullptr},
        {"compress-to", 0, 0, G_OPTION_ARG_STRING, &compress_to, "Add the files to name.FORMAT without a dialog",
         "FORMAT"},
        {"test", 't', 0, G_OPTION_ARG_NONE, &test, "Test the archives", nullptr},
        {"hash", 0, 0, G_OPTION_ARG_STRING, &hash, "Calculate checksums (CRC32, SHA256, ..., or *)", "METHOD"},
        {"profile", 0, 0, G_OPTION_ARG_FILENAME, &profile, "Use DIR for settings instead of ~/.config/7zipl", "DIR"},
        {"screenshot", 0, 0, G_OPTION_ARG_FILENAME, &screenshot, "Save a PNG of the window and quit", "FILE"},
        {"selftest", 0, 0, G_OPTION_ARG_NONE, &selftest, "Run the built-in tests and exit", nullptr},
        {"version", 'V', 0, G_OPTION_ARG_NONE, &version, "Show the version and exit", nullptr},
        {G_OPTION_REMAINING, 0, 0, G_OPTION_ARG_FILENAME_ARRAY, &rest, nullptr, "[FOLDER|ARCHIVE|FILE...]"},
        {nullptr, 0, 0, G_OPTION_ARG_NONE, nullptr, nullptr, nullptr},
    };
    GOptionContext *ctx = g_option_context_new("- 7-Zip File Manager");
    g_option_context_set_summary(ctx,
                                 "Browse folders and archives, create, extract, test and update archives.\n"
                                 "Without options, opens FOLDER or ARCHIVE (or a folder inside an archive).");
    g_option_context_add_main_entries(ctx, entries, nullptr);
    GError *err = nullptr;
    if (!g_option_context_parse(ctx, &argc, &argv, &err)) {
        std::fprintf(stderr, "7zipl: %s\nTry '7zipl --help'.\n", err->message);
        g_error_free(err);
        g_option_context_free(ctx);
        return 2;
    }
    g_option_context_free(ctx);

    if (version) {
        szl::init(argv[0]);
        std::printf("7zipl %s (7-Zip engine %s)\n", SZL_VERSION, szl::version().c_str());
        return 0;
    }
    if (profile) szl::set_config_dir(profile);

    szl::AppOptions o;
    for (char **p = rest; p && *p; p++) {
        GFile *f = g_file_new_for_commandline_arg(*p);
        char *path = g_file_get_path(f);
        o.files.emplace_back(path ? path : *p);
        g_free(path);
        g_object_unref(f);
    }
    g_strfreev(rest);
    if (screenshot) o.screenshot = screenshot;

    int modes = 0;
    if (selftest) o.mode = szl::Mode::SelfTest, modes++;
    if (extract) o.mode = szl::Mode::Extract, modes++;
    if (extract_here) o.mode = szl::Mode::ExtractHere, modes++;
    if (extract_folder) o.mode = szl::Mode::ExtractToFolder, modes++;
    if (compress) o.mode = szl::Mode::Compress, modes++;
    if (compress_to) {
        o.mode = szl::Mode::CompressTo;
        o.format = compress_to;
        modes++;
    }
    if (test) o.mode = szl::Mode::Test, modes++;
    if (hash) {
        o.mode = szl::Mode::Hash;
        o.hash = hash;
        modes++;
    }
    g_free(compress_to);
    g_free(hash);
    g_free(profile);
    g_free(screenshot);
    if (modes > 1) {
        std::fprintf(stderr, "7zipl: use only one of the operation options\n");
        return 2;
    }
    if (o.mode != szl::Mode::Browse && o.mode != szl::Mode::SelfTest && o.files.empty()) {
        std::fprintf(stderr, "7zipl: no files given\n");
        return 2;
    }
    if (o.mode == szl::Mode::SelfTest && !gtk_init_check()) {
        std::fprintf(stderr, "7zipl: no display, self-test skipped\n");
        return 77;
    }
    return szl::run_app(argv[0], o);
}
