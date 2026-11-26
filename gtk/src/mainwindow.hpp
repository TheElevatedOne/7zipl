// SPDX-License-Identifier: LGPL-2.1-or-later
// 7zipl: the 7-Zip File Manager window: menu bar, toolbar, one or two panels (F9) each with its own
// address bar, and the status bar.
#pragma once

#include "panel.hpp"

#include <gtk/gtk.h>

#include <memory>
#include <string>
#include <vector>

namespace szl {

class Ui;
struct Result;
// File > Split file: `src` into `dir`/`name`.001, .002, ... of `volume` bytes each.
Result split_file(Ui &ui, const std::string &src, const std::string &dir, const std::string &name, uint64_t volume);
// File > Combine files: `parts` in order into `out`.
Result combine_files(Ui &ui, const std::vector<std::string> &parts, const std::string &out, uint64_t total);

class MainWindow {
public:
    explicit MainWindow(GtkApplication *app);
    ~MainWindow();
    GtkWindow *window() const { return GTK_WINDOW(win_); }
    Panel &panel() { return *panels_[active_].panel; }        // the active panel
    Panel &other_panel() { return *panels_[1 - active_].panel; }
    bool two_panels() const { return two_; }

    void open_path(const std::string &path);
    void present();

    // File
    void cmd_open(int mode);              // 0 Open, 1 Inside, 2 Outside
    void cmd_open_inside_type(const std::string &type);
    void cmd_view(bool edit);             // F3 / F4
    void cmd_copy_move(bool move);
    void cmd_delete(bool permanently);
    void cmd_rename();
    void cmd_split();
    void cmd_combine();
    void cmd_properties();
    void cmd_hash(const std::string &method);
    void cmd_diff();
    void cmd_new_folder();
    void cmd_new_file();
    void cmd_link();
    // Archive toolbar
    void cmd_add();
    void cmd_extract();
    void cmd_test();
    // Edit
    void cmd_select_mask(bool select);
    // View
    void set_two_panels(bool on);
    void switch_panel();
    void cmd_folders_history();
    // Favorites
    void cmd_add_favorite(int i);
    void cmd_goto_favorite(int i);
    // Tools
    void cmd_options(int page = 0);
    void cmd_benchmark();
    void cmd_delete_temp();
    void cmd_help();

    // Adds files to the archive open in `p` (into its current folder).
    void add_files_to_archive(Panel &p, const std::vector<std::string> &files, bool move = false);
    void copy_files_to(const std::vector<std::string> &files, const std::string &dest, bool move);
    // Extracts archive items of the active panel to `dir` (Copy To / Move To from an archive).
    void extract_selection_to(const std::vector<Row> &sel, const std::string &dir, bool move);
    void add_files_to_archive(const std::vector<std::string> &files) { add_files_to_archive(panel(), files); }

    void refresh_all();           // refresh both panels after an operation
    // Edit (F4) inside an archive: asks to put `file` back into `archive` (folder `prefix`) when it changes.
    void watch_edited(const std::string &file, const std::string &archive, const std::string &prefix,
                      const std::string &password, const std::string &format);
    void update_status();
    void update_address(int i);
    void apply_view_settings();
    void rebuild_favorites_menu();

private:
    struct Side {
        std::unique_ptr<Panel> panel;
        GtkWidget *box, *address, *up;
    };
    void build_side(int i);
    void build_actions();
    GMenuModel *build_menu();
    GtkWidget *build_toolbar();
    void set_active(int i);
    void after_change();          // refresh the panels after an operation
    void launch_with(const std::string &command, const std::vector<std::string> &files, const char *fallback_type);

    GtkWidget *win_;
    GtkWidget *toolbar_, *paned_, *status_, *status_bar_;
    Side panels_[2];
    int active_ = 0;
    bool two_ = false;
    GMenu *favorites_menu_ = nullptr;
    std::vector<GFileMonitor *> edit_monitors_;
};

}  // namespace szl
