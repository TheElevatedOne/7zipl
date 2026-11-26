// SPDX-License-Identifier: LGPL-2.1-or-later
// 7zipl: the file list of 7-Zip File Manager. Shows a folder on disk or a folder inside an archive
// (archives inside archives too), with 7-Zip's columns.
#pragma once

#include "engine.hpp"

#include <gtk/gtk.h>

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace szl {

struct Row {
    std::string name;
    std::string full;        // FS: absolute path; archive: path inside the archive
    bool dir = false;
    bool up = false;         // the ".." row
    bool hidden = false;
    bool has_size = false, has_packed = false, has_mtime = false, has_ctime = false, has_atime = false;
    bool has_crc = false, encrypted = false, is_link = false;
    uint64_t size = 0, packed = 0;
    int64_t mtime = 0, ctime = 0, atime = 0;
    uint32_t crc = 0;
    uint64_t num_files = 0, num_folders = 0;  // folders in archives
    std::string attrib, method, comment, user, group, link;
    std::string content_type;
    int64_t index = -1;      // archive item index (-1: implicit folder)
};

// One opened archive (a level of nesting).
struct ArcLevel {
    std::string file;          // the archive on disk (a temporary copy for nested archives)
    std::string virtual_path;  // what the address bar shows, e.g. /home/me/a.7z/docs/b.tar
    std::string format;
    bool can_update = false;
    std::string password;
    std::string parent_prefix, name_in_parent;  // where a nested archive sits in its parent
    std::vector<std::pair<std::string, std::string>> props;
    struct Node {
        Row row;
        std::map<std::string, size_t> dirs;  // child folders by name
        std::vector<size_t> children;
    };
    std::vector<Node> nodes;   // nodes[0] = root
    int find(const std::string &prefix) const;  // node index of a folder path, -1 if missing
    void build(const ArchiveListing &l);
};

class Panel {
public:
    Panel();
    ~Panel();
    GtkWidget *widget() const { return root_; }
    GtkWidget *list() const { return view_; }

    // Callbacks set by the main window.
    std::function<void()> on_location_changed;
    std::function<void()> on_selection_changed;
    std::function<void(const std::vector<std::string> &)> on_drop_files;  // files dropped on the list
    std::function<void(const Row &)> on_open_file;  // activate on a file that isn't opened inside
    std::function<void()> on_focus;                  // the list got the keyboard focus (active panel)

    // Navigation
    void navigate_fs(const std::string &dir, const std::string &select = {});
    // `type`: "" by signature, "*" every handler (Open Inside *), "#" parser mode (Open Inside #)
    void open_archive(GtkWindow *parent, const std::string &file, const std::string &inner = {},
                      std::function<void(bool)> done = nullptr, const std::string &type = {});
    // Navigates to a typed path: a folder, an archive, or a folder inside an archive.
    void navigate_to(GtkWindow *parent, const std::string &path);
    void go_up();
    void go_root();
    void refresh(GtkWindow *parent);
    void enter(const std::string &name);   // a subfolder of the current folder
    // mode: 0 = like 7-Zip (archives open inside, other files with their application), 1 = inside, 2 = outside
    void activate(GtkWindow *parent, const Row &r, int mode = 0);
    // Re-reads the innermost archive after it was changed (add/delete/rename), keeping the folder.
    void reload_archive(GtkWindow *parent);

    // State
    bool in_archive() const { return !levels_.empty(); }
    ArcLevel &arc() { return *levels_.back(); }
    const std::string &prefix() const { return prefix_; }
    const std::string &fs_dir() const { return fs_dir_; }
    std::string address() const;
    int depth() const { return (int)levels_.size(); }

    // Selection
    std::vector<Row> selected() const;   // without ".."
    std::vector<Row> all_rows() const;
    std::unique_ptr<Row> focused() const;
    void select_all();
    void select_none();
    void invert_selection();
    void select_mask(const std::string &mask, bool select);
    void select_name(const std::string &name);
    void select_by_type(bool select);   // items with the focused item's extension
    void focus();
    bool has_focus() const;
    GtkWidget *current_view() const { return view_mode_ == 3 ? view_ : grid_; }
    std::vector<GtkWidget *> views() const { return {view_, grid_}; }

    // View
    void set_view_mode(int mode);       // 0 large icons, 1 small icons, 2 list, 3 details
    void sort_by(int column);           // -1 = unsorted (archive / folder order)
    void set_flat(bool flat);
    bool flat() const { return flat_; }
    void set_active(bool active);

    // Extracts archive items to a new temporary folder, then calls `done` with their paths (main thread).
    void extract_to_temp(GtkWindow *parent, const std::vector<Row> &rows,
                         std::function<void(const std::vector<std::string> &)> done);
    std::string status_text() const;

    void set_show_hidden(bool v);
    void apply_view_settings();

private:
    void fill_fs();
    void fill_archive();
    void set_rows(std::vector<Row> rows, const std::string &select);
    void build_columns();
    void update_column_visibility();
    void open_archive_item(GtkWindow *parent, const Row &r, int mode);
    void open_level(GtkWindow *parent, const std::string &file, const std::string &virtual_path,
                    const std::string &inner, bool nested, const std::string &name_in_parent,
                    std::function<void(bool)> done, const std::string &type = {});
    void watch();
    void scroll_to(guint pos, GtkListScrollFlags flags);                       // auto refresh: monitor the folder or the archive file
    void collect_flat(std::vector<Row> &rows, const std::string &dir, const std::string &rel, int depth);

    GtkWidget *root_ = nullptr;
    GtkWidget *view_ = nullptr;
    GtkWidget *grid_ = nullptr;
    GtkWidget *scroller_ = nullptr;
    GFileMonitor *monitor_ = nullptr;
    guint monitor_timer_ = 0;
    int view_mode_ = 3;
    bool flat_ = false;
    GListStore *store_ = nullptr;
    GtkSortListModel *sorted_ = nullptr;
    GtkMultiSelection *sel_ = nullptr;
    std::vector<GtkColumnViewColumn *> cols_;
    std::string fs_dir_;
    std::vector<std::unique_ptr<ArcLevel>> levels_;
    std::string prefix_;
    bool show_hidden_ = false;
    bool busy_ = false;
};

}  // namespace szl
