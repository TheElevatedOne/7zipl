// SPDX-License-Identifier: LGPL-2.1-or-later
// 7zipl: application style sheet. Square corners, solid backgrounds, and the GTK theme's named
// colours (pywal and similar style sheets define the same names as Adwaita).
#include "style.hpp"

#include <gtk/gtk.h>

#include <string>

namespace szl {

static const char *kCss = R"css(
* { border-radius: 0; }

.szl-list {
  background-color: @szl_view_bg;
  color: @szl_view_fg;
}
.szl-list > listview > row:selected,
.szl-list > listview > row:selected cell {
  background-color: @szl_accent_bg;
  color: @szl_accent_fg;
}
.szl-list > listview > row cell { padding: 1px 4px; }
.szl-list > listview > row .dim-label { opacity: 0.7; }
.szl-list .size-cell { font-feature-settings: "tnum"; }
.szl-list.inactive > listview > row:selected,
.szl-list.inactive > listview > row:selected cell {
  background-color: mix(@szl_view_bg, @szl_accent_bg, 0.45);
}

.szl-toolbar {
  padding: 2px 4px;
  background-color: @szl_window_bg;
  border-bottom: 1px solid mix(@szl_window_bg, @szl_window_fg, 0.12);
}
.szl-toolbar button { padding: 2px 8px; }
.szl-toolbar button .tool-label { font-size: 0.85em; }
.szl-address {
  padding: 3px 4px;
  background-color: @szl_window_bg;
  border-bottom: 1px solid mix(@szl_window_bg, @szl_window_fg, 0.10);
}
.szl-address entry { min-height: 0; }
.szl-statusbar {
  padding: 2px 8px;
  background-color: @szl_window_bg;
  border-top: 1px solid mix(@szl_window_bg, @szl_window_fg, 0.12);
}
.szl-statusbar label { font-size: 0.9em; }
.szl-statusbar separator { margin: 2px 8px; }
.szl-panel.active-panel { box-shadow: inset 0 2px 0 @szl_accent_bg; }
.szl-progress progressbar trough { min-height: 14px; }
.szl-progress progressbar progress { min-height: 14px; }
.szl-messages, .szl-hash { font-family: monospace; }
.szl-mono { font-family: monospace; }
.szl-warning { color: @szl_error; }
)css";

void load_style()
{
    struct Alias {
        const char *name;
        const char *candidates[4];
        const char *fallback;
    };
    static const Alias aliases[] = {
        {"szl_view_bg", {"view_bg_color", "theme_base_color", nullptr}, "#ffffff"},
        {"szl_view_fg", {"view_fg_color", "theme_text_color", nullptr}, "#000000"},
        {"szl_window_bg", {"window_bg_color", "theme_bg_color", nullptr}, "#f0f0f0"},
        {"szl_window_fg", {"window_fg_color", "theme_fg_color", nullptr}, "#000000"},
        {"szl_accent_bg", {"accent_bg_color", "theme_selected_bg_color", nullptr}, "#3584e4"},
        {"szl_accent_fg", {"accent_fg_color", "theme_selected_fg_color", nullptr}, "#ffffff"},
        {"szl_error", {"error_color", "error_bg_color", nullptr}, "#c01c28"},
    };
    GtkWidget *probe = gtk_label_new(nullptr);
    g_object_ref_sink(probe);
    GtkStyleContext *ctx = gtk_widget_get_style_context(probe);
    std::string defs;
    for (const Alias &a : aliases) {
        const char *use = nullptr;
        for (int i = 0; a.candidates[i]; ++i) {
            GdkRGBA c;
            if (gtk_style_context_lookup_color(ctx, a.candidates[i], &c)) {
                use = a.candidates[i];
                break;
            }
        }
        defs += std::string("@define-color ") + a.name + " " +
                (use ? std::string("@") + use : std::string(a.fallback)) + ";\n";
    }
    g_object_unref(probe);
    GtkCssProvider *p = gtk_css_provider_new();
    gtk_css_provider_load_from_string(p, (defs + kCss).c_str());
    gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(p),
                                               GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(p);
}

}  // namespace szl
