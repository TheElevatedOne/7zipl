// SPDX-License-Identifier: LGPL-2.1-or-later
#include "panel.hpp"

#include "gtkutil.hpp"
#include "job.hpp"
#include "settings.hpp"
#include "util.hpp"

#include <glib/gstdio.h>

#include <algorithm>
#include <set>
#include <unordered_map>

// ---------- SzlRow: a GObject holding a szl::Row for the list model ----------

G_BEGIN_DECLS
#define SZL_TYPE_ROW (szl_row_get_type())
G_DECLARE_FINAL_TYPE(SzlRow, szl_row, SZL, ROW, GObject)
G_END_DECLS

struct _SzlRow {
    GObject parent_instance;
    szl::Row *row;
};

G_DEFINE_FINAL_TYPE(SzlRow, szl_row, G_TYPE_OBJECT)

static void szl_row_finalize(GObject *o)
{
    delete SZL_ROW(o)->row;
    G_OBJECT_CLASS(szl_row_parent_class)->finalize(o);
}
static void szl_row_class_init(SzlRowClass *c) { G_OBJECT_CLASS(c)->finalize = szl_row_finalize; }
static void szl_row_init(SzlRow *r) { r->row = nullptr; }

static SzlRow *szl_row_new(szl::Row &&r)
{
    SzlRow *o = SZL_ROW(g_object_new(SZL_TYPE_ROW, nullptr));
    o->row = new szl::Row(std::move(r));
    return o;
}

namespace szl {

namespace {

enum Col {
    C_NAME,
    C_SIZE,
    C_PACKED,
    C_MTIME,
    C_CTIME,
    C_ATIME,
    C_ATTRIB,
    C_ENCRYPTED,
    C_METHOD,
    C_CRC,
    C_COMMENT,
    C_FOLDERS,
    C_FILES,
    C_USER,
    C_GROUP,
    C_LINK,
    C_TYPE,  // the extension, for View > Arrange by Type (not shown)
    C_COUNT
};
const char *const kColTitles[C_COUNT] = {"Name",   "Size",     "Packed Size", "Modified", "Created", "Accessed",
                                         "Attributes", "Encrypted", "Method", "CRC", "Comment", "Folders",
                                         "Files", "User", "Group", "Link", "Type"};

const Row *row_of(gpointer item) { return item ? SZL_ROW(item)->row : nullptr; }

// Extensions 7-Zip can open that people usually want to open with their own application.
bool prefers_external(const std::string &name)
{
    static const char *const exts[] = {"docx", "xlsx", "pptx", "odt", "ods", "odp", "odg", "epub", "flv",
                                       "swf",  "chm",  "exe",  "dll", "sys", "jar", "apk", "ipa", "xpi",
                                       "appx", "msi",  "msix", "mslz", "ppmd", "mub", "dmg", "img", "vhd",
                                       "vhdx", "vmdk", "qcow", "qcow2", "elf", "macho", "uefif", "hfs",
                                       "fat", "ntfs", "mbr", "gpt", "ext", "ext2", "ext3", "ext4", "sqfs"};
    const auto dot = name.rfind('.');
    if (dot == std::string::npos) return false;
    std::string ext = name.substr(dot + 1);
    for (auto &c : ext) c = (char)g_ascii_tolower(c);
    for (const char *e : exts)
        if (ext == e) return true;
    return false;
}

std::string mode_string(guint32 mode, bool dir, bool link)
{
    std::string s(10, '-');
    s[0] = link ? 'l' : dir ? 'd' : '-';
    const char *rwx = "rwxrwxrwx";
    for (int i = 0; i < 9; i++)
        if (mode & (1u << (8 - i))) s[i + 1] = rwx[i];
    if (mode & 04000) s[3] = (mode & 0100) ? 's' : 'S';
    if (mode & 02000) s[6] = (mode & 010) ? 's' : 'S';
    if (mode & 01000) s[9] = (mode & 01) ? 't' : 'T';
    return s;
}

std::string col_text(const Row &r, int col)
{
    if (r.up) return col == C_NAME ? ".." : "";
    switch (col) {
    case C_NAME: return r.name;
    case C_SIZE: return r.has_size ? format_number(r.size) : "";
    case C_PACKED: return r.has_packed ? format_number(r.packed) : "";
    case C_MTIME: return r.has_mtime ? format_time(r.mtime) : "";
    case C_CTIME: return r.has_ctime ? format_time(r.ctime) : "";
    case C_ATIME: return r.has_atime ? format_time(r.atime) : "";
    case C_ATTRIB: return r.attrib;
    case C_ENCRYPTED: return r.encrypted ? "+" : (r.dir ? "" : "-");
    case C_METHOD: return r.method;
    case C_CRC: {
        if (!r.has_crc) return "";
        char b[16];
        g_snprintf(b, sizeof b, "%08X", r.crc);
        return b;
    }
    case C_COMMENT: return r.comment;
    case C_FOLDERS: return r.dir ? format_number(r.num_folders) : "";
    case C_FILES: return r.dir ? format_number(r.num_files) : "";
    case C_USER: return r.user;
    case C_GROUP: return r.group;
    case C_LINK: return r.link;
    case C_TYPE: {
        if (r.dir) return {};
        const auto dot = r.name.rfind('.');
        return dot == std::string::npos || dot == 0 ? std::string() : r.name.substr(dot + 1);
    }
    default: return "";
    }
}

int cmp_u64(uint64_t a, uint64_t b) { return a < b ? -1 : a > b ? 1 : 0; }
int cmp_str(const std::string &a, const std::string &b)
{
    return g_utf8_collate(a.c_str(), b.c_str());
}
int cmp_name(const std::string &a, const std::string &b)
{
    char *ka = g_utf8_collate_key_for_filename(a.c_str(), -1);
    char *kb = g_utf8_collate_key_for_filename(b.c_str(), -1);
    int r = strcmp(ka, kb);
    g_free(ka);
    g_free(kb);
    return r;
}

int compare_rows(const Row &a, const Row &b, int col)
{
    int r = 0;
    switch (col) {
    case C_SIZE: r = cmp_u64(a.size, b.size); break;
    case C_PACKED: r = cmp_u64(a.packed, b.packed); break;
    case C_MTIME: r = cmp_u64((uint64_t)a.mtime, (uint64_t)b.mtime); break;
    case C_CTIME: r = cmp_u64((uint64_t)a.ctime, (uint64_t)b.ctime); break;
    case C_ATIME: r = cmp_u64((uint64_t)a.atime, (uint64_t)b.atime); break;
    case C_CRC: r = cmp_u64(a.crc, b.crc); break;
    case C_FOLDERS: r = cmp_u64(a.num_folders, b.num_folders); break;
    case C_FILES: r = cmp_u64(a.num_files, b.num_files); break;
    case C_NAME: break;
    default: r = cmp_str(col_text(a, col), col_text(b, col)); break;
    }
    if (r == 0) r = cmp_name(a.name, b.name);
    return r;
}

GIcon *icon_for(const Row &r)
{
    static std::unordered_map<std::string, GIcon *> cache;
    if (r.up) return g_themed_icon_new("go-up");
    if (r.dir) return g_themed_icon_new("folder");
    if (!szl::settings().real_icons) return g_themed_icon_new("text-x-generic");
    std::string ct = r.content_type;
    if (ct.empty()) {
        char *g = g_content_type_guess(r.name.c_str(), nullptr, 0, nullptr);
        ct = g ? g : "application/octet-stream";
        g_free(g);
    }
    auto it = cache.find(ct);
    if (it == cache.end()) it = cache.emplace(ct, g_content_type_get_icon(ct.c_str())).first;
    return G_ICON(g_object_ref(it->second));
}

void setup_cell(GtkSignalListItemFactory *, GtkListItem *li, gpointer data)
{
    const int col = GPOINTER_TO_INT(data);
    if (col == C_NAME) {
        GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        GtkWidget *img = gtk_image_new();
        GtkWidget *l = gtk_label_new(nullptr);
        gtk_label_set_xalign(GTK_LABEL(l), 0);
        gtk_label_set_ellipsize(GTK_LABEL(l), PANGO_ELLIPSIZE_END);
        gtk_box_append(GTK_BOX(box), img);
        gtk_box_append(GTK_BOX(box), l);
        gtk_list_item_set_child(li, box);
    } else {
        GtkWidget *l = gtk_label_new(nullptr);
        const bool num = col == C_SIZE || col == C_PACKED || col == C_FOLDERS || col == C_FILES;
        gtk_label_set_xalign(GTK_LABEL(l), num ? 1.0f : 0.0f);
        gtk_label_set_ellipsize(GTK_LABEL(l), PANGO_ELLIPSIZE_END);
        if (num) gtk_widget_add_css_class(l, "size-cell");
        if (col == C_CRC || col == C_ATTRIB) gtk_widget_add_css_class(l, "szl-mono");
        gtk_list_item_set_child(li, l);
    }
}

void bind_cell(GtkSignalListItemFactory *, GtkListItem *li, gpointer data)
{
    const int col = GPOINTER_TO_INT(data);
    const Row *r = row_of(gtk_list_item_get_item(li));
    if (!r) return;
    GtkWidget *child = gtk_list_item_get_child(li);
    if (col == C_NAME) {
        GtkWidget *img = gtk_widget_get_first_child(child);
        GtkWidget *l = gtk_widget_get_next_sibling(img);
        GIcon *ic = icon_for(*r);
        gtk_image_set_from_gicon(GTK_IMAGE(img), ic);
        g_object_unref(ic);
        gtk_label_set_text(GTK_LABEL(l), r->up ? ".." : r->name.c_str());
        gtk_widget_set_opacity(img, r->hidden ? 0.55 : 1.0);
    } else {
        gtk_label_set_text(GTK_LABEL(child), col_text(*r, col).c_str());
    }
}

// Large Icons (icon above the name), Small Icons and List (icon left of the name).
void setup_tile(GtkSignalListItemFactory *, GtkListItem *li, gpointer data)
{
    const bool large = GPOINTER_TO_INT(data) == 0;
    GtkWidget *box = gtk_box_new(large ? GTK_ORIENTATION_VERTICAL : GTK_ORIENTATION_HORIZONTAL, large ? 2 : 6);
    GtkWidget *img = gtk_image_new();
    gtk_image_set_pixel_size(GTK_IMAGE(img), large ? 48 : 16);
    GtkWidget *l = gtk_label_new(nullptr);
    if (large) {
        gtk_label_set_wrap(GTK_LABEL(l), TRUE);
        gtk_label_set_wrap_mode(GTK_LABEL(l), PANGO_WRAP_WORD_CHAR);
        gtk_label_set_lines(GTK_LABEL(l), 2);
        gtk_label_set_justify(GTK_LABEL(l), GTK_JUSTIFY_CENTER);
        gtk_label_set_width_chars(GTK_LABEL(l), 12);
        gtk_label_set_max_width_chars(GTK_LABEL(l), 12);
    } else {
        gtk_label_set_xalign(GTK_LABEL(l), 0);
        gtk_label_set_width_chars(GTK_LABEL(l), 24);
        gtk_label_set_max_width_chars(GTK_LABEL(l), 24);
    }
    gtk_label_set_ellipsize(GTK_LABEL(l), PANGO_ELLIPSIZE_END);
    gtk_box_append(GTK_BOX(box), img);
    gtk_box_append(GTK_BOX(box), l);
    gtk_list_item_set_child(li, box);
}

void bind_tile(GtkSignalListItemFactory *, GtkListItem *li, gpointer)
{
    const Row *r = row_of(gtk_list_item_get_item(li));
    if (!r) return;
    GtkWidget *box = gtk_list_item_get_child(li);
    GtkWidget *img = gtk_widget_get_first_child(box);
    GtkWidget *l = gtk_widget_get_next_sibling(img);
    GIcon *ic = icon_for(*r);
    gtk_image_set_from_gicon(GTK_IMAGE(img), ic);
    g_object_unref(ic);
    gtk_label_set_text(GTK_LABEL(l), r->up ? ".." : r->name.c_str());
    gtk_widget_set_opacity(img, r->hidden ? 0.55 : 1.0);
    gtk_widget_set_tooltip_text(box, r->up ? nullptr : r->name.c_str());
}

int sort_func(gconstpointer a, gconstpointer b, gpointer data)
{
    const Row *ra = row_of((gpointer)a), *rb = row_of((gpointer)b);
    return compare_rows(*ra, *rb, GPOINTER_TO_INT(data));
}

// ".." first and folders before files, whatever the column's sort direction.
int fixed_order(gconstpointer a, gconstpointer b, gpointer)
{
    const Row *ra = row_of((gpointer)a), *rb = row_of((gpointer)b);
    if (ra->up != rb->up) return ra->up ? -1 : 1;
    if (ra->dir != rb->dir) return ra->dir ? -1 : 1;
    return 0;
}

}  // namespace

// ---------- ArcLevel ----------

int ArcLevel::find(const std::string &prefix) const
{
    if (nodes.empty()) return -1;
    size_t cur = 0;
    if (prefix.empty()) return 0;
    for (const auto &part : split(prefix, '/')) {
        if (part.empty()) continue;
        auto it = nodes[cur].dirs.find(part);
        if (it == nodes[cur].dirs.end()) return -1;
        cur = it->second;
    }
    return (int)cur;
}

void ArcLevel::build(const ArchiveListing &l)
{
    nodes.clear();
    nodes.emplace_back();
    nodes[0].row.dir = true;
    for (const Entry &e : l.entries) {
        std::vector<std::string> parts;
        for (auto &p : split(e.path, '/'))
            if (!p.empty() && p != ".") parts.push_back(p);
        if (parts.empty()) continue;
        size_t cur = 0;
        std::string full;
        for (size_t i = 0; i + 1 < parts.size(); i++) {
            full = full.empty() ? parts[i] : full + "/" + parts[i];
            auto it = nodes[cur].dirs.find(parts[i]);
            if (it == nodes[cur].dirs.end()) {
                Node n;
                n.row.name = parts[i];
                n.row.full = full;
                n.row.dir = true;
                nodes.push_back(std::move(n));
                const size_t idx = nodes.size() - 1;
                nodes[cur].dirs[parts[i]] = idx;
                nodes[cur].children.push_back(idx);
                cur = idx;
            } else {
                cur = it->second;
            }
        }
        full = full.empty() ? parts.back() : full + "/" + parts.back();
        Row r;
        r.name = parts.back();
        r.full = full;
        r.dir = e.dir;
        r.index = e.index;
        r.has_size = e.has_size;
        r.size = e.size;
        r.has_packed = e.has_packed;
        r.packed = e.packed;
        r.has_mtime = e.has_mtime;
        r.mtime = e.mtime;
        r.has_ctime = e.has_ctime;
        r.ctime = e.ctime;
        r.has_atime = e.has_atime;
        r.atime = e.atime;
        r.has_crc = e.has_crc;
        r.crc = e.crc;
        r.encrypted = e.encrypted;
        r.attrib = e.attrib;
        r.method = e.method;
        r.comment = e.comment;
        r.user = e.user;
        r.group = e.group;
        r.link = e.link;
        r.is_link = !e.link.empty();
        r.hidden = !r.name.empty() && r.name[0] == '.';
        if (e.dir) {
            auto it = nodes[cur].dirs.find(r.name);
            if (it != nodes[cur].dirs.end()) {  // folder already created by a child: fill in its details
                Row &d = nodes[it->second].row;
                const std::string name = d.name;
                d = r;
                d.name = name;
                continue;
            }
            Node n;
            n.row = r;
            nodes.push_back(std::move(n));
            const size_t idx = nodes.size() - 1;
            nodes[cur].dirs[r.name] = idx;
            nodes[cur].children.push_back(idx);
        } else {
            Node n;
            n.row = r;
            nodes.push_back(std::move(n));
            nodes[cur].children.push_back(nodes.size() - 1);
        }
    }
    // Folder totals (7-Zip FM shows the size of a folder's contents). Children come after parents,
    // so a reverse pass sums bottom-up.
    for (size_t i = nodes.size(); i-- > 0;) {
        Node &n = nodes[i];
        if (!n.row.dir) continue;
        uint64_t size = 0, packed = 0, files = 0, folders = 0;
        bool any_packed = false;
        for (size_t c : n.children) {
            const Row &cr = nodes[c].row;
            size += cr.size;
            packed += cr.packed;
            any_packed |= cr.has_packed;
            if (cr.dir) {
                folders += 1 + cr.num_folders;
                files += cr.num_files;
            } else {
                files++;
            }
        }
        n.row.size = size;
        n.row.has_size = true;
        n.row.packed = packed;
        n.row.has_packed = any_packed;
        n.row.num_files = files;
        n.row.num_folders = folders;
    }
}

// ---------- Panel ----------

Panel::Panel()
{
    show_hidden_ = settings().show_hidden;
    store_ = g_list_store_new(SZL_TYPE_ROW);
    view_ = gtk_column_view_new(nullptr);
    gtk_widget_add_css_class(view_, "szl-list");
    build_columns();

    GtkSorter *fixed = GTK_SORTER(gtk_custom_sorter_new(fixed_order, nullptr, nullptr));
    GtkMultiSorter *ms = gtk_multi_sorter_new();
    gtk_multi_sorter_append(ms, fixed);
    GtkSorter *cvs = gtk_column_view_get_sorter(GTK_COLUMN_VIEW(view_));
    gtk_multi_sorter_append(ms, GTK_SORTER(g_object_ref(cvs)));
    sorted_ = gtk_sort_list_model_new(G_LIST_MODEL(g_object_ref(store_)), GTK_SORTER(ms));
    sel_ = gtk_multi_selection_new(G_LIST_MODEL(g_object_ref(sorted_)));
    gtk_column_view_set_model(GTK_COLUMN_VIEW(view_), GTK_SELECTION_MODEL(sel_));
    gtk_column_view_set_reorderable(GTK_COLUMN_VIEW(view_), TRUE);

    grid_ = gtk_grid_view_new(nullptr, nullptr);
    g_object_ref_sink(grid_);
    gtk_widget_add_css_class(grid_, "szl-list");
    gtk_grid_view_set_max_columns(GTK_GRID_VIEW(grid_), 64);
    g_signal_connect(grid_, "activate", CB([](GtkGridView *, guint pos, Panel *p) {
                         SzlRow *o = SZL_ROW(g_list_model_get_item(G_LIST_MODEL(p->sorted_), pos));
                         if (!o) return;
                         Row r = *o->row;
                         g_object_unref(o);
                         p->activate(GTK_WINDOW(gtk_widget_get_root(p->scroller_)), r);
                     }),
                     this);
    for (GtkWidget *v : {view_, grid_}) {
        GtkEventController *fc = gtk_event_controller_focus_new();
        g_signal_connect_swapped(fc, "enter", CB([](Panel *p) {
                                     if (p->on_focus) p->on_focus();
                                 }),
                                 this);
        gtk_widget_add_controller(v, fc);
    }
    g_signal_connect(view_, "activate", CB([](GtkColumnView *, guint pos, Panel *p) {
                         SzlRow *o = SZL_ROW(g_list_model_get_item(G_LIST_MODEL(p->sorted_), pos));
                         if (!o) return;
                         Row r = *o->row;
                         g_object_unref(o);
                         GtkWindow *win = GTK_WINDOW(gtk_widget_get_root(p->view_));
                         p->activate(win, r);
                     }),
                     this);
    g_signal_connect(sel_, "selection-changed", CB([](GtkSelectionModel *, guint, guint, Panel *p) {
                         if (p->on_selection_changed) p->on_selection_changed();
                     }),
                     this);

    // Files dropped on the list: copied into the folder, or added to the archive.
    for (GtkWidget *target : {view_, grid_}) {
    GtkDropTarget *drop = gtk_drop_target_new(GDK_TYPE_FILE_LIST, GDK_ACTION_COPY);
    g_signal_connect(drop, "drop", CB([](GtkDropTarget *, const GValue *v, double, double, Panel *p) -> gboolean {
                         if (!G_VALUE_HOLDS(v, GDK_TYPE_FILE_LIST)) return FALSE;
                         std::vector<std::string> paths;
                         for (GSList *l = gdk_file_list_get_files((GdkFileList *)g_value_get_boxed(v)); l; l = l->next) {
                             char *path = g_file_get_path(G_FILE(l->data));
                             if (path) paths.emplace_back(path);
                             g_free(path);
                         }
                         if (paths.empty() || !p->on_drop_files) return FALSE;
                         p->on_drop_files(paths);
                         return TRUE;
                     }),
                     this);
    gtk_widget_add_controller(target, GTK_EVENT_CONTROLLER(drop));

    // Dragging files out of a folder on disk.
    GtkDragSource *drag = gtk_drag_source_new();
    gtk_drag_source_set_actions(drag, GDK_ACTION_COPY);
    g_signal_connect(drag, "prepare", CB([](GtkDragSource *, double, double, Panel *p) -> GdkContentProvider * {
                         if (p->in_archive()) return nullptr;
                         GSList *files = nullptr;
                         for (const Row &r : p->selected()) files = g_slist_append(files, g_file_new_for_path(r.full.c_str()));
                         if (!files) return nullptr;
                         GdkFileList *fl = gdk_file_list_new_from_list(files);
                         g_slist_free_full(files, g_object_unref);
                         GdkContentProvider *cp = gdk_content_provider_new_typed(GDK_TYPE_FILE_LIST, fl);
                         g_boxed_free(GDK_TYPE_FILE_LIST, fl);
                         return cp;
                     }),
                     this);
    gtk_widget_add_controller(target, GTK_EVENT_CONTROLLER(drag));
    }

    // Backspace goes up, like 7-Zip FM.
    for (GtkWidget *target : {view_, grid_}) {
    GtkEventController *keys = gtk_event_controller_key_new();
    g_signal_connect(keys, "key-pressed", CB([](GtkEventControllerKey *, guint key, guint, GdkModifierType mods, Panel *p) -> gboolean {
                         if (key == GDK_KEY_BackSpace && !(mods & (GDK_CONTROL_MASK | GDK_ALT_MASK))) {
                             p->go_up();
                             return TRUE;
                         }
                         return FALSE;
                     }),
                     this);
    gtk_widget_add_controller(target, keys);
    }

    g_object_ref_sink(view_);
    scroller_ = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(scroller_, TRUE);
    gtk_widget_set_hexpand(scroller_, TRUE);
    root_ = scroller_;
    g_object_ref(root_);
    set_view_mode(settings().view_mode);
    apply_view_settings();
}

Panel::~Panel()
{
    if (monitor_timer_) g_source_remove(monitor_timer_);
    if (monitor_) {
        g_file_monitor_cancel(monitor_);
        g_object_unref(monitor_);
    }
    g_object_unref(view_);
    g_object_unref(grid_);
    g_object_unref(sel_);
    g_object_unref(sorted_);
    g_object_unref(store_);
    g_object_unref(root_);
}

void Panel::build_columns()
{
    static const int widths[C_COUNT] = {260, 100, 100, 150, 150, 150, 90, 70, 110, 80, 120, 70, 70, 80, 80, 140, 60};
    for (int c = 0; c < C_COUNT; c++) {
        GtkListItemFactory *f = gtk_signal_list_item_factory_new();
        g_signal_connect(f, "setup", G_CALLBACK(setup_cell), GINT_TO_POINTER(c));
        g_signal_connect(f, "bind", G_CALLBACK(bind_cell), GINT_TO_POINTER(c));
        GtkColumnViewColumn *col = gtk_column_view_column_new(kColTitles[c], f);
        gtk_column_view_column_set_resizable(col, TRUE);
        gtk_column_view_column_set_fixed_width(col, widths[c]);
        if (c == C_NAME) gtk_column_view_column_set_expand(col, TRUE);
        GtkSorter *s = GTK_SORTER(gtk_custom_sorter_new(sort_func, GINT_TO_POINTER(c), nullptr));
        gtk_column_view_column_set_sorter(col, s);
        g_object_unref(s);
        gtk_column_view_append_column(GTK_COLUMN_VIEW(view_), col);
        cols_.push_back(col);
        g_object_unref(col);
    }
    const Settings &st = settings();
    const int sc = st.sort_column >= 0 && st.sort_column < C_COUNT ? st.sort_column : 0;
    gtk_column_view_sort_by_column(GTK_COLUMN_VIEW(view_), cols_[sc],
                                   st.sort_descending ? GTK_SORT_DESCENDING : GTK_SORT_ASCENDING);
    GtkSorter *cvs = gtk_column_view_get_sorter(GTK_COLUMN_VIEW(view_));
    g_signal_connect(cvs, "changed", CB([](GtkSorter *s, GtkSorterChange, Panel *p) {
                         GtkColumnViewColumn *c = gtk_column_view_sorter_get_primary_sort_column(GTK_COLUMN_VIEW_SORTER(s));
                         for (size_t i = 0; i < p->cols_.size(); i++)
                             if (p->cols_[i] == c) settings().sort_column = (int)i;
                         settings().sort_descending =
                             gtk_column_view_sorter_get_primary_sort_order(GTK_COLUMN_VIEW_SORTER(s)) == GTK_SORT_DESCENDING;
                     }),
                     this);
}

void Panel::apply_view_settings()
{
    const Settings &st = settings();
    gtk_column_view_set_show_row_separators(GTK_COLUMN_VIEW(view_), st.show_grid);
    gtk_column_view_set_show_column_separators(GTK_COLUMN_VIEW(view_), st.show_grid);
    gtk_column_view_set_single_click_activate(GTK_COLUMN_VIEW(view_), st.single_click);
}

void Panel::update_column_visibility()
{
    bool has[C_COUNT] = {};
    has[C_NAME] = true;
    const guint n = g_list_model_get_n_items(G_LIST_MODEL(store_));
    for (guint i = 0; i < n; i++) {
        SzlRow *o = SZL_ROW(g_list_model_get_item(G_LIST_MODEL(store_), i));
        const Row &r = *o->row;
        if (!r.up) {
            has[C_SIZE] |= r.has_size;
            has[C_PACKED] |= r.has_packed;
            has[C_MTIME] |= r.has_mtime;
            has[C_CTIME] |= r.has_ctime;
            has[C_ATIME] |= r.has_atime && in_archive();
            has[C_ATTRIB] |= !r.attrib.empty();
            has[C_ENCRYPTED] |= r.encrypted;
            has[C_METHOD] |= !r.method.empty();
            has[C_CRC] |= r.has_crc;
            has[C_COMMENT] |= !r.comment.empty();
            has[C_USER] |= !r.user.empty() && in_archive();
            has[C_GROUP] |= !r.group.empty() && in_archive();
            has[C_LINK] |= !r.link.empty();
        }
        g_object_unref(o);
    }
    if (in_archive()) {
        has[C_SIZE] = has[C_PACKED] = has[C_MTIME] = true;
        has[C_ENCRYPTED] = has[C_ENCRYPTED] || arc().format == "7z" || arc().format == "zip";
        has[C_FOLDERS] = has[C_FILES] = true;
    } else {
        has[C_SIZE] = has[C_MTIME] = has[C_CTIME] = has[C_ATTRIB] = true;
    }
    has[C_TYPE] = false;
    for (int c = 0; c < C_COUNT; c++) gtk_column_view_column_set_visible(cols_[c], has[c]);
}

void Panel::set_rows(std::vector<Row> rows, const std::string &select)
{
    std::vector<gpointer> objs;
    objs.reserve(rows.size() + 1);
    const bool dotdot = settings().show_dotdot && (in_archive() || fs_dir_ != "/");
    if (dotdot) {
        Row up;
        up.up = true;
        up.dir = true;
        up.name = "..";
        objs.push_back(szl_row_new(std::move(up)));
    }
    for (auto &r : rows) {
        if (r.hidden && !show_hidden_) continue;
        objs.push_back(szl_row_new(std::move(r)));
    }
    g_list_store_splice(store_, 0, g_list_model_get_n_items(G_LIST_MODEL(store_)), objs.data(), objs.size());
    for (gpointer o : objs) g_object_unref(o);
    update_column_visibility();
    if (!select.empty()) {
        select_name(select);
    } else if (g_list_model_get_n_items(G_LIST_MODEL(sorted_)) > 0) {
        scroll_to(0, GTK_LIST_SCROLL_FOCUS);
        gtk_selection_model_unselect_all(GTK_SELECTION_MODEL(sel_));
    }
    if (on_location_changed) on_location_changed();
    if (on_selection_changed) on_selection_changed();
}

void Panel::collect_flat(std::vector<Row> &rows, const std::string &dir, const std::string &rel, int depth)
{
    if (depth > 64 || rows.size() > 200000) return;
    GDir *d = g_dir_open(dir.c_str(), 0, nullptr);
    if (!d) return;
    const char *n;
    while ((n = g_dir_read_name(d))) {
        const std::string full = path_join(dir, n), name = rel.empty() ? std::string(n) : rel + "/" + n;
        GStatBuf st;
        if (g_lstat(full.c_str(), &st) != 0) continue;
        if (!show_hidden_ && n[0] == '.') continue;
        if (S_ISDIR(st.st_mode)) {
            collect_flat(rows, full, name, depth + 1);
            continue;
        }
        Row r;
        r.name = name;
        r.full = full;
        r.has_size = true;
        r.size = (uint64_t)st.st_size;
        r.has_mtime = true;
        r.mtime = st.st_mtime;
        r.attrib = mode_string(st.st_mode, false, S_ISLNK(st.st_mode));
        r.is_link = S_ISLNK(st.st_mode);
        rows.push_back(std::move(r));
    }
    g_dir_close(d);
}

void Panel::fill_fs()
{
    std::vector<Row> rows;
    if (flat_) {
        collect_flat(rows, fs_dir_, {}, 0);
        set_rows(std::move(rows), {});
        watch();
        return;
    }
    GFile *dir = g_file_new_for_path(fs_dir_.c_str());
    GError *err = nullptr;
    GFileEnumerator *en = g_file_enumerate_children(
        dir,
        G_FILE_ATTRIBUTE_STANDARD_NAME "," G_FILE_ATTRIBUTE_STANDARD_TYPE "," G_FILE_ATTRIBUTE_STANDARD_SIZE
        "," G_FILE_ATTRIBUTE_STANDARD_IS_HIDDEN "," G_FILE_ATTRIBUTE_STANDARD_IS_SYMLINK
        "," G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE "," G_FILE_ATTRIBUTE_STANDARD_SYMLINK_TARGET
        "," G_FILE_ATTRIBUTE_TIME_MODIFIED "," G_FILE_ATTRIBUTE_TIME_CREATED "," G_FILE_ATTRIBUTE_TIME_ACCESS
        "," G_FILE_ATTRIBUTE_UNIX_MODE "," G_FILE_ATTRIBUTE_OWNER_USER "," G_FILE_ATTRIBUTE_OWNER_GROUP,
        G_FILE_QUERY_INFO_NONE, nullptr, &err);
    if (en) {
        GFileInfo *fi;
        while ((fi = g_file_enumerator_next_file(en, nullptr, nullptr))) {
            Row r;
            r.name = g_file_info_get_name(fi);
            r.full = path_join(fs_dir_, r.name);
            r.dir = g_file_info_get_file_type(fi) == G_FILE_TYPE_DIRECTORY;
            r.hidden = g_file_info_get_is_hidden(fi);
            r.is_link = g_file_info_get_is_symlink(fi);
            if (!r.dir) {
                r.has_size = true;
                r.size = (uint64_t)g_file_info_get_size(fi);
            }
            if (g_file_info_has_attribute(fi, G_FILE_ATTRIBUTE_TIME_MODIFIED)) {
                r.has_mtime = true;
                r.mtime = (int64_t)g_file_info_get_attribute_uint64(fi, G_FILE_ATTRIBUTE_TIME_MODIFIED);
            }
            if (g_file_info_has_attribute(fi, G_FILE_ATTRIBUTE_TIME_CREATED)) {
                r.has_ctime = true;
                r.ctime = (int64_t)g_file_info_get_attribute_uint64(fi, G_FILE_ATTRIBUTE_TIME_CREATED);
            }
            if (g_file_info_has_attribute(fi, G_FILE_ATTRIBUTE_TIME_ACCESS)) {
                r.has_atime = true;
                r.atime = (int64_t)g_file_info_get_attribute_uint64(fi, G_FILE_ATTRIBUTE_TIME_ACCESS);
            }
            if (g_file_info_has_attribute(fi, G_FILE_ATTRIBUTE_UNIX_MODE))
                r.attrib = mode_string(g_file_info_get_attribute_uint32(fi, G_FILE_ATTRIBUTE_UNIX_MODE), r.dir, r.is_link);
            const char *ct = g_file_info_get_content_type(fi);
            if (ct) r.content_type = ct;
            if (r.is_link) {
                const char *t = g_file_info_get_symlink_target(fi);
                if (t) r.link = t;
            }
            const char *u = g_file_info_get_attribute_string(fi, G_FILE_ATTRIBUTE_OWNER_USER);
            if (u) r.user = u;
            const char *g = g_file_info_get_attribute_string(fi, G_FILE_ATTRIBUTE_OWNER_GROUP);
            if (g) r.group = g;
            rows.push_back(std::move(r));
            g_object_unref(fi);
        }
        g_object_unref(en);
    } else if (err) {
        g_error_free(err);
    }
    g_object_unref(dir);
    set_rows(std::move(rows), {});
    watch();
}

void Panel::fill_archive()
{
    std::vector<Row> rows;
    ArcLevel &a = arc();
    int idx = a.find(prefix_);
    if (idx < 0) {
        prefix_.clear();
        idx = 0;
    }
    if (flat_) {
        // Every file below the folder, named by its path from here.
        std::vector<std::pair<size_t, std::string>> stack = {{(size_t)idx, std::string()}};
        while (!stack.empty()) {
            auto [n, rel] = stack.back();
            stack.pop_back();
            for (size_t c : a.nodes[n].children) {
                const Row &cr = a.nodes[c].row;
                const std::string name = rel.empty() ? cr.name : rel + "/" + cr.name;
                if (cr.dir) {
                    stack.emplace_back(c, name);
                } else {
                    Row r = cr;
                    r.name = name;
                    rows.push_back(std::move(r));
                }
            }
        }
    } else {
        for (size_t c : a.nodes[(size_t)idx].children) rows.push_back(a.nodes[c].row);
    }
    set_rows(std::move(rows), {});
    watch();
}

void Panel::navigate_fs(const std::string &dir, const std::string &select)
{
    std::string d = dir.empty() ? home_dir() : dir;
    while (d.size() > 1 && d.back() == '/') d.pop_back();
    while (!g_file_test(d.c_str(), G_FILE_TEST_IS_DIR) && d != "/") d = path_dirname(d);
    levels_.clear();
    prefix_.clear();
    fs_dir_ = d;
    settings().last_folder = d;
    fill_fs();
    if (!select.empty()) select_name(select);
}

void Panel::open_archive(GtkWindow *parent, const std::string &file, const std::string &inner,
                         std::function<void(bool)> done, const std::string &type)
{
    open_level(parent, file, file, inner, false, {}, std::move(done), type);
}

void Panel::open_level(GtkWindow *parent, const std::string &file, const std::string &virtual_path,
                       const std::string &inner, bool nested, const std::string &name_in_parent,
                       std::function<void(bool)> done, const std::string &type)
{
    if (busy_) return;
    busy_ = true;
    auto listing = std::make_shared<ArchiveListing>();
    JobSpec spec;
    spec.title = "Opening";
    spec.archive = file;
    spec.show_delay_ms = 500;
    spec.silent = true;
    if (nested && !levels_.empty()) spec.password = arc().password;
    const std::string password = spec.password;
    start_job(
        parent, spec,
        [file, listing, password, type](Ui &ui) {
            *listing = szl::open_archive(file, ui, password, type);
            Result r;
            r.ok = listing->ok;
            r.cancelled = listing->cancelled;
            return r;
        },
        [this, parent, file, virtual_path, inner, nested, name_in_parent, listing, done](const Result &) {
            busy_ = false;
            if (!listing->ok) {
                if (!listing->cancelled && !done)
                    show_message(parent, "7zipl", listing->error.empty() ? "Cannot open the file as archive" : listing->error, true);
                if (done) done(false);
                return;
            }
            auto lvl = std::make_unique<ArcLevel>();
            lvl->file = file;
            lvl->virtual_path = virtual_path;
            lvl->format = listing->format;
            lvl->can_update = listing->can_update && !nested;
            lvl->password = listing->password;
            lvl->props = listing->props;
            lvl->build(*listing);
            if (nested) {
                lvl->parent_prefix = prefix_;
                lvl->name_in_parent = name_in_parent;
            } else {
                levels_.clear();
                fs_dir_ = path_dirname(file);
                settings().last_folder = fs_dir_;
            }
            levels_.push_back(std::move(lvl));
            std::string in = inner;
            while (!in.empty() && in.back() == '/') in.pop_back();
            while (!in.empty() && in.front() == '/') in.erase(0, 1);
            prefix_ = arc().find(in) >= 0 ? in : std::string();
            fill_archive();
            if (done) done(true);
        });
}

void Panel::reload_archive(GtkWindow *parent)
{
    if (!in_archive() || busy_) return;
    busy_ = true;
    auto listing = std::make_shared<ArchiveListing>();
    const std::string file = arc().file, password = arc().password;
    JobSpec spec;
    spec.title = "Opening";
    spec.archive = file;
    spec.show_delay_ms = 500;
    spec.silent = true;
    spec.password = password;
    start_job(
        parent, spec,
        [file, listing, password](Ui &ui) {
            *listing = szl::open_archive(file, ui, password);
            Result r;
            r.ok = listing->ok;
            return r;
        },
        [this, parent, file, listing](const Result &) {
            busy_ = false;
            if (!in_archive() || arc().file != file) return;
            if (!listing->ok) {
                // The archive is gone or broken now: back to its folder.
                if (levels_.size() == 1 && !g_file_test(file.c_str(), G_FILE_TEST_EXISTS)) {
                    navigate_fs(fs_dir_);
                    return;
                }
                show_message(parent, "7zipl", listing->error, true);
                return;
            }
            std::vector<std::string> keep;
            for (const Row &r : selected()) keep.push_back(r.name);
            ArcLevel &a = arc();
            a.format = listing->format;
            a.props = listing->props;
            if (!listing->password.empty()) a.password = listing->password;
            a.build(*listing);
            while (!prefix_.empty() && a.find(prefix_) < 0) prefix_ = prefix_.find('/') == std::string::npos ? "" : path_dirname(prefix_);
            fill_archive();
            for (const auto &n : keep) select_name(n);
        });
}

void Panel::navigate_to(GtkWindow *parent, const std::string &path_in)
{
    std::string path = trim(path_in);
    if (path.empty()) return;
    if (path[0] == '~') path = home_dir() + path.substr(1);
    if (g_str_has_prefix(path.c_str(), "file://")) {
        char *p = g_filename_from_uri(path.c_str(), nullptr, nullptr);
        if (p) {
            path = p;
            g_free(p);
        }
    }
    if (path[0] != '/') path = path_join(in_archive() ? fs_dir_ : fs_dir_, path);
    while (path.size() > 1 && path.back() == '/') path.pop_back();
    if (g_file_test(path.c_str(), G_FILE_TEST_IS_DIR)) {
        navigate_fs(path);
        return;
    }
    if (g_file_test(path.c_str(), G_FILE_TEST_IS_REGULAR)) {
        open_archive(parent, path);
        return;
    }
    // A folder inside an archive: /path/a.7z/docs/img
    std::string file = path, inner;
    while (file.size() > 1) {
        const std::string base = path_basename(file);
        inner = inner.empty() ? base : base + "/" + inner;
        file = path_dirname(file);
        if (g_file_test(file.c_str(), G_FILE_TEST_IS_REGULAR)) {
            open_archive(parent, file, inner);
            return;
        }
        if (g_file_test(file.c_str(), G_FILE_TEST_IS_DIR)) break;
    }
    show_message(parent, "7zipl", "The system cannot find the path specified:\n" + path, true);
}

void Panel::go_up()
{
    if (busy_) return;
    if (in_archive()) {
        if (!prefix_.empty()) {
            const std::string name = path_basename(prefix_);
            prefix_ = prefix_.find('/') == std::string::npos ? "" : path_dirname(prefix_);
            fill_archive();
            select_name(name);
            return;
        }
        auto &top = levels_.back();
        const std::string parent_prefix = top->parent_prefix, name = top->name_in_parent;
        const std::string file = top->file;
        if (levels_.size() > 1) {
            levels_.pop_back();
            prefix_ = parent_prefix;
            fill_archive();
            select_name(name);
            return;
        }
        navigate_fs(path_dirname(file), path_basename(file));
        return;
    }
    if (fs_dir_ == "/") return;
    navigate_fs(path_dirname(fs_dir_), path_basename(fs_dir_));
}

void Panel::go_root()
{
    if (in_archive()) {
        prefix_.clear();
        fill_archive();
    } else {
        navigate_fs("/");
    }
}

void Panel::refresh(GtkWindow *parent)
{
    if (in_archive()) {
        reload_archive(parent);
        return;
    }
    std::vector<std::string> keep;
    for (const Row &r : selected()) keep.push_back(r.name);
    if (!g_file_test(fs_dir_.c_str(), G_FILE_TEST_IS_DIR)) {
        navigate_fs(fs_dir_);
        return;
    }
    fill_fs();
    for (const auto &n : keep) select_name(n);
}

void Panel::enter(const std::string &name)
{
    if (in_archive()) {
        prefix_ = prefix_.empty() ? name : prefix_ + "/" + name;
        fill_archive();
    } else {
        navigate_fs(path_join(fs_dir_, name));
    }
}

void Panel::activate(GtkWindow *parent, const Row &r, int mode)
{
    const bool inside_always = mode == 1;
    if (busy_) return;
    if (r.up) {
        go_up();
        return;
    }
    if (r.dir) {
        enter(r.name);
        return;
    }
    if (in_archive()) {
        open_archive_item(parent, r, mode);
        return;
    }
    if (mode != 2 && (inside_always || (looks_like_archive(r.name) && !prefers_external(r.name)))) {
        const bool must = inside_always;
        const Row copy = r;
        open_archive(parent, r.full, {}, [this, parent, copy, must](bool ok) {
            if (ok) return;
            if (must)
                show_message(parent, "7zipl", "Cannot open the file as archive:\n" + copy.full, true);
            else if (on_open_file)
                on_open_file(copy);
        });
        return;
    }
    if (on_open_file) on_open_file(r);
}

void Panel::open_archive_item(GtkWindow *parent, const Row &r, int mode)
{
    const bool inside = mode == 1;
    const std::string tmp = make_temp_dir("open");
    if (tmp.empty()) return;
    ExtractRequest req;
    req.archive = arc().file;
    req.out_dir = tmp;
    req.items = {r.full};
    req.strip_prefix = r.full.find('/') == std::string::npos ? std::string() : path_dirname(r.full);
    req.overwrite = 1;
    req.password = arc().password;
    JobSpec spec;
    spec.title = "Extracting";
    spec.archive = arc().file;
    spec.show_delay_ms = 500;
    spec.password = arc().password;
    const std::string out = path_join(tmp, r.name);
    const std::string virt = arc().virtual_path + "/" + r.full;
    const Row copy = r;
    start_job(
        parent, spec, [req](Ui &ui) { return extract_items(req, ui); },
        [this, parent, out, virt, copy, inside, mode](const Result &res) {
            if (!res.ok || !g_file_test(out.c_str(), G_FILE_TEST_EXISTS)) return;
            const bool try_inside =
                mode != 2 && (inside || (looks_like_archive(copy.name) && !prefers_external(copy.name)));
            if (!try_inside) {
                launch_file(parent, out);
                return;
            }
            open_level(parent, out, virt, {}, true, copy.name, [this, parent, out, inside](bool ok) {
                if (ok) return;
                if (inside)
                    show_message(parent, "7zipl", "Cannot open the file as archive", true);
                else
                    launch_file(parent, out);
            });
        });
}

std::string Panel::address() const
{
    if (!in_archive()) return fs_dir_ == "/" ? "/" : fs_dir_ + "/";
    std::string a = levels_.back()->virtual_path + "/";
    if (!prefix_.empty()) a += prefix_ + "/";
    return a;
}

std::vector<Row> Panel::selected() const
{
    std::vector<Row> out;
    GtkBitset *bs = gtk_selection_model_get_selection(GTK_SELECTION_MODEL(sel_));
    GtkBitsetIter it;
    guint pos;
    if (gtk_bitset_iter_init_first(&it, bs, &pos)) {
        do {
            SzlRow *o = SZL_ROW(g_list_model_get_item(G_LIST_MODEL(sorted_), pos));
            if (o) {
                if (!o->row->up) out.push_back(*o->row);
                g_object_unref(o);
            }
        } while (gtk_bitset_iter_next(&it, &pos));
    }
    gtk_bitset_unref(bs);
    return out;
}

std::vector<Row> Panel::all_rows() const
{
    std::vector<Row> out;
    const guint n = g_list_model_get_n_items(G_LIST_MODEL(sorted_));
    for (guint i = 0; i < n; i++) {
        SzlRow *o = SZL_ROW(g_list_model_get_item(G_LIST_MODEL(sorted_), i));
        if (!o->row->up) out.push_back(*o->row);
        g_object_unref(o);
    }
    return out;
}

std::unique_ptr<Row> Panel::focused() const
{
    auto s = selected();
    if (s.empty()) return nullptr;
    return std::make_unique<Row>(s.front());
}

void Panel::select_all()
{
    gtk_selection_model_select_all(GTK_SELECTION_MODEL(sel_));
    // Not the ".." row.
    const guint n = g_list_model_get_n_items(G_LIST_MODEL(sorted_));
    for (guint i = 0; i < n; i++) {
        SzlRow *o = SZL_ROW(g_list_model_get_item(G_LIST_MODEL(sorted_), i));
        const bool up = o->row->up;
        g_object_unref(o);
        if (up) gtk_selection_model_unselect_item(GTK_SELECTION_MODEL(sel_), i);
    }
}

void Panel::select_none() { gtk_selection_model_unselect_all(GTK_SELECTION_MODEL(sel_)); }

void Panel::invert_selection()
{
    const guint n = g_list_model_get_n_items(G_LIST_MODEL(sorted_));
    GtkBitset *sel = gtk_bitset_new_empty();
    GtkBitset *mask = gtk_bitset_new_range(0, n);
    for (guint i = 0; i < n; i++) {
        SzlRow *o = SZL_ROW(g_list_model_get_item(G_LIST_MODEL(sorted_), i));
        const bool up = o->row->up;
        g_object_unref(o);
        if (!up && !gtk_selection_model_is_selected(GTK_SELECTION_MODEL(sel_), i)) gtk_bitset_add(sel, i);
    }
    gtk_selection_model_set_selection(GTK_SELECTION_MODEL(sel_), sel, mask);
    gtk_bitset_unref(sel);
    gtk_bitset_unref(mask);
}

void Panel::select_mask(const std::string &mask, bool select)
{
    const guint n = g_list_model_get_n_items(G_LIST_MODEL(sorted_));
    for (guint i = 0; i < n; i++) {
        SzlRow *o = SZL_ROW(g_list_model_get_item(G_LIST_MODEL(sorted_), i));
        const bool match = !o->row->up && mask_match(mask, o->row->name);
        g_object_unref(o);
        if (!match) continue;
        if (select)
            gtk_selection_model_select_item(GTK_SELECTION_MODEL(sel_), i, FALSE);
        else
            gtk_selection_model_unselect_item(GTK_SELECTION_MODEL(sel_), i);
    }
}

void Panel::select_name(const std::string &name)
{
    const guint n = g_list_model_get_n_items(G_LIST_MODEL(sorted_));
    for (guint i = 0; i < n; i++) {
        SzlRow *o = SZL_ROW(g_list_model_get_item(G_LIST_MODEL(sorted_), i));
        const bool eq = !o->row->up && o->row->name == name;
        g_object_unref(o);
        if (eq) {
            scroll_to(i, (GtkListScrollFlags)(GTK_LIST_SCROLL_FOCUS | GTK_LIST_SCROLL_SELECT));
            return;
        }
    }
}

void Panel::focus() { gtk_widget_grab_focus(current_view()); }

std::string Panel::status_text() const
{
    const auto sel = selected();
    const guint total = g_list_model_get_n_items(G_LIST_MODEL(store_));
    const bool has_up = total > 0 && [&] {
        SzlRow *o = SZL_ROW(g_list_model_get_item(G_LIST_MODEL(store_), 0));
        const bool up = o->row->up;
        g_object_unref(o);
        return up;
    }();
    std::string s = format_number(sel.size()) + " / " + format_number(total - (has_up ? 1 : 0)) + " object(s) selected";
    uint64_t size = 0, packed = 0;
    bool any_packed = false;
    for (const Row &r : sel) {
        size += r.size;
        packed += r.packed;
        any_packed |= r.has_packed;
    }
    if (!sel.empty()) {
        s += "    " + format_number(size) + " bytes";
        if (any_packed) s += "    packed " + format_number(packed);
        if (sel.size() == 1 && sel[0].has_mtime) s += "    " + format_time(sel[0].mtime);
    }
    return s;
}

void Panel::set_show_hidden(bool v)
{
    show_hidden_ = v;
}

}  // namespace szl

namespace szl {

void Panel::scroll_to(guint pos, GtkListScrollFlags flags)
{
    if (view_mode_ == 3)
        gtk_column_view_scroll_to(GTK_COLUMN_VIEW(view_), pos, nullptr, flags, nullptr);
    else
        gtk_grid_view_scroll_to(GTK_GRID_VIEW(grid_), pos, flags, nullptr);
}

void Panel::set_view_mode(int mode)
{
    view_mode_ = mode < 0 || mode > 3 ? 3 : mode;
    if (view_mode_ == 3) {
        gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller_), view_);
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller_), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
        gtk_column_view_set_model(GTK_COLUMN_VIEW(view_), GTK_SELECTION_MODEL(sel_));
        gtk_grid_view_set_model(GTK_GRID_VIEW(grid_), nullptr);
        return;
    }
    GtkListItemFactory *f = gtk_signal_list_item_factory_new();
    g_signal_connect(f, "setup", G_CALLBACK(setup_tile), GINT_TO_POINTER(view_mode_));
    g_signal_connect(f, "bind", G_CALLBACK(bind_tile), nullptr);
    gtk_grid_view_set_factory(GTK_GRID_VIEW(grid_), f);
    g_object_unref(f);
    gtk_column_view_set_model(GTK_COLUMN_VIEW(view_), nullptr);
    gtk_grid_view_set_model(GTK_GRID_VIEW(grid_), GTK_SELECTION_MODEL(sel_));
    gtk_grid_view_set_min_columns(GTK_GRID_VIEW(grid_), 1);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller_), grid_);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller_), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
}

void Panel::sort_by(int column)
{
    if (column < 0 || column >= (int)cols_.size()) {
        gtk_column_view_sort_by_column(GTK_COLUMN_VIEW(view_), nullptr, GTK_SORT_ASCENDING);
        settings().sort_column = -1;
        return;
    }
    gtk_column_view_sort_by_column(GTK_COLUMN_VIEW(view_), cols_[(size_t)column], GTK_SORT_ASCENDING);
}

void Panel::set_flat(bool flat)
{
    if (flat == flat_) return;
    flat_ = flat;
    if (in_archive())
        fill_archive();
    else
        fill_fs();
}

void Panel::set_active(bool active)
{
    for (GtkWidget *v : {view_, grid_}) {
        if (active)
            gtk_widget_remove_css_class(v, "inactive");
        else
            gtk_widget_add_css_class(v, "inactive");
    }
}

bool Panel::has_focus() const
{
    GtkRoot *root = gtk_widget_get_root(scroller_);
    if (!root) return false;
    GtkWidget *f = gtk_root_get_focus(root);
    return f && (f == view_ || f == grid_ || gtk_widget_is_ancestor(f, scroller_));
}

void Panel::select_by_type(bool select)
{
    auto f = focused();
    if (!f) return;
    const auto dot = f->name.rfind('.');
    const std::string mask = f->dir ? std::string() : (dot == std::string::npos || dot == 0 ? std::string() : "*" + f->name.substr(dot));
    const bool dir = f->dir;
    const guint n = g_list_model_get_n_items(G_LIST_MODEL(sorted_));
    for (guint i = 0; i < n; i++) {
        SzlRow *o = SZL_ROW(g_list_model_get_item(G_LIST_MODEL(sorted_), i));
        const Row &r = *o->row;
        bool match = false;
        if (!r.up) {
            if (dir) match = r.dir;
            else if (mask.empty()) match = !r.dir && (r.name.rfind('.') == std::string::npos || r.name.rfind('.') == 0);
            else match = !r.dir && mask_match(mask, r.name);
        }
        g_object_unref(o);
        if (!match) continue;
        if (select)
            gtk_selection_model_select_item(GTK_SELECTION_MODEL(sel_), i, FALSE);
        else
            gtk_selection_model_unselect_item(GTK_SELECTION_MODEL(sel_), i);
    }
}

void Panel::watch()
{
    if (monitor_timer_) {
        g_source_remove(monitor_timer_);
        monitor_timer_ = 0;
    }
    if (monitor_) {
        g_file_monitor_cancel(monitor_);
        g_object_unref(monitor_);
        monitor_ = nullptr;
    }
    if (!settings().auto_refresh || flat_) return;
    // A folder: its entries. An archive: the archive file (changed by another program).
    const bool arch = in_archive();
    if (arch && levels_.size() > 1) return;  // nested archives are temporary copies
    GFile *f = g_file_new_for_path(arch ? arc().file.c_str() : fs_dir_.c_str());
    monitor_ = arch ? g_file_monitor_file(f, G_FILE_MONITOR_WATCH_MOVES, nullptr, nullptr)
                    : g_file_monitor_directory(f, G_FILE_MONITOR_WATCH_MOVES, nullptr, nullptr);
    g_object_unref(f);
    if (!monitor_) return;
    g_signal_connect(monitor_, "changed", CB([](GFileMonitor *, GFile *, GFile *, GFileMonitorEvent ev, Panel *p) {
                         if (ev == G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT && !p->in_archive()) return;
                         if (p->monitor_timer_) g_source_remove(p->monitor_timer_);
                         // Coalesce bursts (a program writing many files) into one refresh.
                         p->monitor_timer_ = g_timeout_add(400, [](gpointer pp) -> gboolean {
                             auto *p = static_cast<Panel *>(pp);
                             p->monitor_timer_ = 0;
                             if (!p->busy_ && running_jobs() == 0)
                                 p->refresh(GTK_WINDOW(gtk_widget_get_root(p->scroller_)));
                             return G_SOURCE_REMOVE;
                         }, p);
                     }),
                     this);
}

void Panel::extract_to_temp(GtkWindow *parent, const std::vector<Row> &rows,
                            std::function<void(const std::vector<std::string> &)> done)
{
    if (!in_archive() || rows.empty()) return;
    const std::string tmp = make_temp_dir("open");
    if (tmp.empty()) return;
    ExtractRequest req;
    req.archive = arc().file;
    req.out_dir = tmp;
    req.overwrite = 1;
    req.path_mode = 1;  // no paths: flat view names may contain folders
    req.password = arc().password;
    std::vector<std::string> out;
    for (const Row &r : rows) {
        req.items.push_back(r.full);
        out.push_back(path_join(tmp, path_basename(r.full)));
    }
    JobSpec spec;
    spec.title = "Extracting";
    spec.archive = arc().file;
    spec.show_delay_ms = 500;
    spec.password = arc().password;
    start_job(parent, spec, [req](Ui &ui) { return extract_items(req, ui); },
              [out, done](const Result &r) {
                  if (r.ok) done(out);
              });
}

}  // namespace szl
