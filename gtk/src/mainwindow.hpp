// SPDX-License-Identifier: LGPL-2.1-or-later
// 7zipl: the 7-Zip File Manager window: menu bar, toolbar, address bar, file list, status bar.
#pragma once

#include "panel.hpp"

#include <gtk/gtk.h>

#include <memory>
#include <string>
#include <vector>

namespace szl {

class MainWindow {
public:
    explicit MainWindow(GtkApplication *app);
    ~MainWindow();
    GtkWindow *window() const { return GTK_WINDOW(win_); }
    Panel &panel() { return *panel_; }

    void open_path(const std::string &path);
    void present();

    // Commands (menu actions)
    void cmd_open(int mode);
    void cmd_add();
    void cmd_extract();
    void cmd_test();
    void cmd_copy_move(bool move);
    void cmd_delete(bool permanently);
    void cmd_rename();
    void cmd_new_folder();
    void cmd_new_file();
    void cmd_properties();
    void cmd_hash(const std::string &method);
    void cmd_options();
    void cmd_select_mask(bool select);
    void cmd_add_favorite(int i);
    void cmd_goto_favorite(int i);
    void add_files_to_archive(const std::vector<std::string> &files);
    void copy_files_here(const std::vector<std::string> &files);

    void update_status();
    void update_address();
    void apply_view_settings();
    void rebuild_favorites_menu();

private:
    void build_actions();
    GMenuModel *build_menu();
    GtkWidget *build_toolbar();
    void after_change();  // refresh after an operation

    GtkWidget *win_;
    GtkWidget *toolbar_, *address_, *up_, *status_, *status_bar_;
    std::unique_ptr<Panel> panel_;
    GMenu *favorites_menu_ = nullptr;
};

}  // namespace szl
