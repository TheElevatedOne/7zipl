// SPDX-License-Identifier: LGPL-2.1-or-later
#include "job.hpp"

#include "dialogs.hpp"
#include "gtkutil.hpp"
#include "util.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <thread>

namespace szl {

namespace {

std::atomic<int> g_running{0};

using Clock = std::chrono::steady_clock;

class Job final : public Ui {
public:
    Job(GtkWindow *parent, const JobSpec &spec, JobWork work, JobDone done)
        : parent_(parent), spec_(spec), work_(std::move(work)), done_(std::move(done))
    {
        if (parent_) g_object_add_weak_pointer(G_OBJECT(parent_), reinterpret_cast<gpointer *>(&parent_));
    }
    ~Job() override
    {
        if (parent_) g_object_remove_weak_pointer(G_OBJECT(parent_), reinterpret_cast<gpointer *>(&parent_));
        if (timer_) g_source_remove(timer_);
    }

    void start();

    // Ui (worker thread)
    void set_total(uint64_t v) override { total_ = v; }
    void set_completed(uint64_t v) override { completed_ = v; }
    void set_percent(unsigned p) override { percent_ = p; }
    bool paused() override { return paused_ && !cancel_; }
    void set_ratio(uint64_t in, uint64_t out) override
    {
        ratio_in_ = in;
        ratio_out_ = out;
    }
    void set_num_files(uint64_t n) override { num_files_ = n; }
    void file_done() override { files_done_++; }
    void set_current(const std::string &what, const std::string &name) override
    {
        std::lock_guard<std::mutex> l(mu_);
        what_ = what;
        name_ = name;
    }
    bool cancelled() override
    {
        if (cancel_) return true;
        if (paused_) {
            std::unique_lock<std::mutex> l(mu_);
            cv_.wait(l, [this] { return !paused_ || cancel_; });
        }
        return cancel_;
    }
    bool ask_password(const std::string &archive, std::string &password) override;
    Overwrite ask_overwrite(const std::string &existing, uint64_t es, int64_t et, const std::string &incoming,
                            uint64_t is, int64_t it) override;
    void message(Level, const std::string &) override {}
    bool ask_memory(uint64_t required, uint64_t limit, const std::string &archive) override;

private:
    void build_window();
    void present();
    void update();
    void finished(Result r);
    void show_result(const Result &r);
    void toggle_pause();
    void cancel();
    void set_paused(bool p);
    uint64_t elapsed_ms() const;

    GtkWindow *parent_;
    JobSpec spec_;
    JobWork work_;
    JobDone done_;
    std::thread thread_;

    std::atomic<unsigned> percent_{0};
    std::atomic<uint64_t> total_{0}, completed_{0}, ratio_in_{0}, ratio_out_{0}, num_files_{0}, files_done_{0};
    std::atomic<bool> cancel_{false}, paused_{false};
    std::atomic<bool> prompting_{false};
    std::mutex mu_;
    std::condition_variable cv_;
    std::string what_, name_;

    Clock::time_point start_;
    Clock::duration paused_total_{};
    Clock::time_point paused_at_;

    GtkWidget *win_ = nullptr;
    GtkWidget *l_elapsed_, *l_remaining_, *l_files_, *l_ratio_, *l_total_, *l_speed_, *l_processed_, *l_packed_;
    GtkWidget *l_ratio_title_, *l_packed_title_;
    GtkWidget *l_what_, *l_name_, *bar_, *b_pause_, *b_cancel_, *box_;
    guint timer_ = 0;
    bool shown_ = false;
    bool done_flag_ = false;
};

uint64_t Job::elapsed_ms() const
{
    auto now = paused_ ? paused_at_ : Clock::now();
    return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(now - start_ - paused_total_).count();
}

void Job::build_window()
{
    win_ = dialog_window(parent_, spec_.title, 460, -1);
    gtk_window_set_modal(GTK_WINDOW(win_), FALSE);
    gtk_widget_add_css_class(win_, "szl-progress");
    box_ = vbox(6);
    set_margins(box_, 12);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 3);
    auto row = [&](int r, int c, const char *title, GtkWidget **value, GtkWidget **title_out = nullptr) {
        GtkWidget *t = label(title);
        *value = label("");
        gtk_label_set_xalign(GTK_LABEL(*value), 1.0f);
        gtk_widget_set_hexpand(*value, TRUE);
        gtk_grid_attach(GTK_GRID(grid), t, c * 2, r, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), *value, c * 2 + 1, r, 1, 1);
        if (title_out) *title_out = t;
    };
    row(0, 0, "Elapsed time:", &l_elapsed_);
    row(1, 0, "Remaining time:", &l_remaining_);
    row(2, 0, "Files:", &l_files_);
    row(3, 0, "Compression ratio:", &l_ratio_, &l_ratio_title_);
    row(0, 1, "Total size:", &l_total_);
    row(1, 1, "Speed:", &l_speed_);
    row(2, 1, "Processed:", &l_processed_);
    row(3, 1, "Compressed size:", &l_packed_, &l_packed_title_);
    gtk_box_append(GTK_BOX(box_), grid);
    if (!spec_.show_ratio) {
        gtk_widget_set_visible(l_ratio_, FALSE);
        gtk_widget_set_visible(l_ratio_title_, FALSE);
        gtk_widget_set_visible(l_packed_, FALSE);
        gtk_widget_set_visible(l_packed_title_, FALSE);
    }

    l_what_ = label("");
    gtk_widget_set_margin_top(l_what_, 8);
    gtk_box_append(GTK_BOX(box_), l_what_);
    l_name_ = label("");
    gtk_label_set_ellipsize(GTK_LABEL(l_name_), PANGO_ELLIPSIZE_MIDDLE);
    gtk_label_set_width_chars(GTK_LABEL(l_name_), 50);
    gtk_box_append(GTK_BOX(box_), l_name_);
    bar_ = gtk_progress_bar_new();
    gtk_box_append(GTK_BOX(box_), bar_);

    auto b = button_row(box_, {"_Pause", "Cancel"}, -1);
    b_pause_ = b[0];
    b_cancel_ = b[1];
    g_signal_connect_swapped(b_pause_, "clicked", CB([](Job *j) { j->toggle_pause(); }), this);
    g_signal_connect_swapped(b_cancel_, "clicked", CB([](Job *j) { j->cancel(); }), this);
    // Closing the window while running cancels the operation (7-Zip asks; Cancel is the safe default).
    g_signal_connect_swapped(win_, "close-request", CB([](Job *j) -> gboolean {
                                 if (j->done_flag_) return FALSE;
                                 j->cancel();
                                 return TRUE;
                             }),
                             this);
    gtk_window_set_child(GTK_WINDOW(win_), box_);
}

void Job::present()
{
    if (shown_ || !win_) return;
    shown_ = true;
    gtk_window_present(GTK_WINDOW(win_));
}

void Job::update()
{
    if (!win_) return;
    const uint64_t ms = elapsed_ms();
    const uint64_t total = total_, done = completed_;
    if (!shown_ && (int)ms >= spec_.show_delay_ms) present();
    gtk_label_set_text(GTK_LABEL(l_elapsed_), format_duration(ms / 1000).c_str());
    const unsigned percent = percent_;
    if (total && done && done <= total && ms > 1000) {
        const double rate = (double)done / ms;
        gtk_label_set_text(GTK_LABEL(l_remaining_), format_duration((uint64_t)((total - done) / rate / 1000)).c_str());
    } else if (!total && percent && ms > 1000) {
        // Only a percentage is known (extracting): estimate from it.
        gtk_label_set_text(GTK_LABEL(l_remaining_),
                           format_duration(ms * (100 - std::min(percent, 100u)) / percent / 1000).c_str());
    } else {
        gtk_label_set_text(GTK_LABEL(l_remaining_), "");
    }
    const uint64_t nf = num_files_, fd = files_done_;
    std::string files = format_number(fd);
    if (nf) files += " / " + format_number(nf);
    gtk_label_set_text(GTK_LABEL(l_files_), files.c_str());
    gtk_label_set_text(GTK_LABEL(l_total_), total ? format_size_short(total).c_str() : "");
    gtk_label_set_text(GTK_LABEL(l_processed_), total || done ? format_size_short(done).c_str() : "");
    if (ms > 500 && (total || done)) gtk_label_set_text(GTK_LABEL(l_speed_), (format_size_short(done * 1000 / ms) + "/s").c_str());
    if (spec_.show_ratio) {
        const uint64_t in = ratio_in_, out = ratio_out_;
        gtk_label_set_text(GTK_LABEL(l_ratio_), in ? (std::to_string(out * 100 / in) + "%").c_str() : "");
        gtk_label_set_text(GTK_LABEL(l_packed_), out ? format_size_short(out).c_str() : "");
    }
    const int pct = total ? (int)(done * 100 / total) : (int)std::min(percent, 100u);
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(bar_), total ? std::min(1.0, (double)done / total) : pct / 100.0);
    {
        std::lock_guard<std::mutex> l(mu_);
        gtk_label_set_text(GTK_LABEL(l_what_), what_.c_str());
        gtk_label_set_text(GTK_LABEL(l_name_), name_.c_str());
    }
    std::string title = (paused_ ? "Paused " : std::to_string(pct) + "% ") + spec_.title;
    if (!spec_.archive.empty()) title += " " + path_basename(spec_.archive);
    gtk_window_set_title(GTK_WINDOW(win_), title.c_str());
}

void Job::set_paused(bool p)
{
    if (p == paused_) return;
    if (p) {
        paused_at_ = Clock::now();
        paused_ = true;
    } else {
        paused_total_ += Clock::now() - paused_at_;
        {
            std::lock_guard<std::mutex> l(mu_);
            paused_ = false;
        }
        cv_.notify_all();
    }
    if (b_pause_) gtk_button_set_label(GTK_BUTTON(b_pause_), paused_ ? "_Continue" : "_Pause");
    gtk_button_set_use_underline(GTK_BUTTON(b_pause_), TRUE);
}

void Job::toggle_pause() { set_paused(!paused_); }

void Job::cancel()
{
    {
        std::lock_guard<std::mutex> l(mu_);
        cancel_ = true;
    }
    cv_.notify_all();
    if (b_cancel_) gtk_widget_set_sensitive(b_cancel_, FALSE);
    if (b_pause_) gtk_widget_set_sensitive(b_pause_, FALSE);
}

bool Job::ask_memory(uint64_t required, uint64_t limit, const std::string &archive)
{
    std::promise<bool> p;
    auto f = p.get_future();
    run_on_main([this, &p, required, limit, archive] {
        present();
        ask_yes_no(win_ ? GTK_WINDOW(win_) : parent_, "Memory usage",
                   "The archive requires a large amount of memory to unpack:\n" + archive + "\n\nRequired: " +
                       format_number(required >> 20) + " MB\nAllowed (Tools > Options > Settings): " +
                       format_number(limit >> 20) + " MB\n\nAllow it for this operation?",
                   [&p](bool yes) { p.set_value(yes); });
    });
    return f.get();
}

bool Job::ask_password(const std::string &archive, std::string &password)
{
    if (!spec_.password.empty()) {
        password = spec_.password;
        spec_.password.clear();  // a second request means it was wrong
        return true;
    }
    std::promise<std::pair<bool, std::string>> p;
    auto f = p.get_future();
    const bool for_new = archive.empty();
    run_on_main([this, &p, archive, for_new] {
        present();
        password_dialog(win_ ? GTK_WINDOW(win_) : parent_, archive, for_new,
                        [&p](bool ok, const std::string &pw) { p.set_value({ok, pw}); });
    });
    auto r = f.get();
    if (!r.first) {
        cancel_ = true;
        return false;
    }
    password = r.second;
    return true;
}

Ui::Overwrite Job::ask_overwrite(const std::string &existing, uint64_t es, int64_t et, const std::string &incoming,
                                 uint64_t is, int64_t it)
{
    std::promise<Overwrite> p;
    auto f = p.get_future();
    run_on_main([this, &p, existing, es, et, incoming, is, it] {
        present();
        overwrite_dialog(win_ ? GTK_WINDOW(win_) : parent_, existing, es, et, incoming, is, it,
                         [&p](Overwrite a) { p.set_value(a); });
    });
    Overwrite a = f.get();
    if (a == Overwrite::Cancel) cancel_ = true;
    return a;
}

void Job::start()
{
    g_running++;
    start_ = Clock::now();
    build_window();
    if (spec_.show_delay_ms <= 0) present();
    timer_ = g_timeout_add(200, [](gpointer p) -> gboolean {
        static_cast<Job *>(p)->update();
        return G_SOURCE_CONTINUE;
    }, this);
    thread_ = std::thread([this] {
        Result r;
        try {
            r = work_(*this);
        } catch (const std::exception &e) {
            r.errors++;
            r.messages.push_back(e.what());
        } catch (...) {
            r.errors++;
            r.messages.push_back("Unknown error");
        }
        if (cancel_ && !r.cancelled) {
            r.cancelled = true;
            r.ok = false;
        }
        run_on_main([this, r] { finished(r); });
    });
}

void Job::finished(Result r)
{
    if (thread_.joinable()) thread_.join();
    g_running--;
    if (timer_) {
        g_source_remove(timer_);
        timer_ = 0;
    }
    done_flag_ = true;
    if (done_) done_(r);
    const bool problems = !spec_.silent && !r.cancelled && (!r.ok || r.errors || r.warnings);
    if (problems || (!spec_.silent && spec_.report_success && r.ok && !r.cancelled)) {
        show_result(r);  // the window lives on and deletes the job when closed
        return;
    }
    if (win_) gtk_window_destroy(GTK_WINDOW(win_));
    delete this;
}

void Job::show_result(const Result &r)
{
    update();
    present();
    // Replace the progress view with 7-Zip's message list.
    GtkWidget *box = vbox(6);
    set_margins(box, 12);
    std::string head = r.ok ? (r.warnings ? "Finished with warnings" : "There are no errors") : "There are some errors";
    if (r.ok && !r.warnings) head = r.summary.empty() ? "Everything is Ok" : r.summary;
    GtkWidget *h = bold_label(head);
    gtk_box_append(GTK_BOX(box), h);
    if (!spec_.archive.empty()) {
        GtkWidget *a = label(spec_.archive);
        gtk_label_set_use_underline(GTK_LABEL(a), FALSE);
        gtk_label_set_text(GTK_LABEL(a), spec_.archive.c_str());
        gtk_label_set_selectable(GTK_LABEL(a), TRUE);
        gtk_label_set_ellipsize(GTK_LABEL(a), PANGO_ELLIPSIZE_MIDDLE);
        gtk_box_append(GTK_BOX(box), a);
    }
    if (r.files || r.folders || r.size) {
        std::string s;
        if (r.folders) s += "Folders: " + format_number(r.folders) + "    ";
        if (r.files) s += "Files: " + format_number(r.files) + "    ";
        if (r.size) s += "Size: " + format_number(r.size) + "    ";
        if (r.packed) s += "Compressed: " + format_number(r.packed);
        gtk_box_append(GTK_BOX(box), label(s));
    }
    if (!r.messages.empty()) {
        std::string text;
        for (const auto &m : r.messages) text += m + "\n";
        GtkWidget *tv = gtk_text_view_new();
        gtk_text_view_set_editable(GTK_TEXT_VIEW(tv), FALSE);
        gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(tv), GTK_WRAP_WORD_CHAR);
        gtk_widget_add_css_class(tv, "szl-messages");
        gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(tv)), text.c_str(), -1);
        GtkWidget *sw = gtk_scrolled_window_new();
        gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sw), tv);
        gtk_widget_set_size_request(sw, 520, 220);
        gtk_widget_set_vexpand(sw, TRUE);
        gtk_box_append(GTK_BOX(box), sw);
        std::string count;
        if (r.errors) count += "Errors: " + format_number(r.errors) + "  ";
        if (r.warnings) count += "Warnings: " + format_number(r.warnings);
        if (!count.empty()) gtk_box_append(GTK_BOX(box), label(count));
    }
    auto b = button_row(box, {"_Close"});
    g_signal_connect_swapped(b[0], "clicked", G_CALLBACK(gtk_window_destroy), win_);
    g_signal_connect_swapped(win_, "destroy", CB([](Job *j) {
                                 j->win_ = nullptr;
                                 delete j;
                             }),
                             this);
    gtk_window_set_child(GTK_WINDOW(win_), box);
    gtk_window_set_default_widget(GTK_WINDOW(win_), b[0]);
    std::string title = spec_.title;
    if (!spec_.archive.empty()) title += " " + path_basename(spec_.archive);
    gtk_window_set_title(GTK_WINDOW(win_), title.c_str());
    b_pause_ = b_cancel_ = nullptr;
}

}  // namespace

void start_job(GtkWindow *parent, const JobSpec &spec, JobWork work, JobDone done)
{
    auto *j = new Job(parent, spec, std::move(work), std::move(done));
    j->start();
}

int running_jobs() { return g_running; }

}  // namespace szl
