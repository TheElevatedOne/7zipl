// SPDX-License-Identifier: LGPL-2.1-or-later
// 7zipl: helpers for GTK code.
#pragma once

#include <gtk/gtk.h>

#include <functional>
#include <string>
#include <vector>

namespace szl {

// The window class the compositor sees (Hyprland's `class`, X11's WM_CLASS) instead of the application ID.
inline constexpr const char *kWindowClass = "7zipl";
void use_window_class();

// G_CALLBACK for captureless lambdas (a macro would split the lambda at its commas).
template <typename F>
inline GCallback CB(F f)
{
    return reinterpret_cast<GCallback>(+f);
}

// Runs a std::function on the main loop (from any thread).
void run_on_main(std::function<void()> fn);

// Box helpers
GtkWidget *vbox(int spacing = 6);
GtkWidget *hbox(int spacing = 6);
GtkWidget *label(const std::string &text, float xalign = 0.0f);
GtkWidget *bold_label(const std::string &text);
GtkWidget *frame(const std::string &title, GtkWidget *child);
void set_margins(GtkWidget *w, int m);

// A drop-down of strings; returns the GtkDropDown.
GtkWidget *dropdown(const std::vector<std::string> &items, unsigned selected = 0);
std::string dropdown_text(GtkWidget *dd);
void dropdown_set_items(GtkWidget *dd, const std::vector<std::string> &items, unsigned selected = 0);
bool dropdown_select(GtkWidget *dd, const std::string &text);

// A dialog-like top-level window, modal and transient for `parent`. Closes with Escape.
GtkWidget *dialog_window(GtkWindow *parent, const std::string &title, int width = -1, int height = -1);
// Adds a right-aligned row of buttons to `box`; returns the buttons in the same order.
std::vector<GtkWidget *> button_row(GtkWidget *box, const std::vector<std::string> &labels, int default_index = 0);

// Simple messages.
void show_message(GtkWindow *parent, const std::string &title, const std::string &text, bool error = false);
// Asks a yes/no question; `cb(true)` on Yes.
void ask_yes_no(GtkWindow *parent, const std::string &title, const std::string &text, std::function<void(bool)> cb);
// Asks for a line of text; `cb(text)` on OK only.
void ask_text(GtkWindow *parent, const std::string &title, const std::string &prompt, const std::string &initial,
              std::function<void(const std::string &)> cb);

// Picks a folder (GtkFileDialog); `cb(path)` on success.
void choose_folder(GtkWindow *parent, const std::string &title, const std::string &initial,
                   std::function<void(const std::string &)> cb);
// Picks a file to save; `cb(path)` on success.
void choose_save_file(GtkWindow *parent, const std::string &title, const std::string &initial,
                      std::function<void(const std::string &)> cb);
// Picks files to open; `cb(paths)` on success.
void choose_open_files(GtkWindow *parent, const std::string &title, const std::string &initial_dir,
                       std::function<void(const std::vector<std::string> &)> cb);

// Opens a file with the system's default application.
void launch_file(GtkWindow *parent, const std::string &path);
void copy_to_clipboard(GtkWidget *w, const std::string &text);

}  // namespace szl
