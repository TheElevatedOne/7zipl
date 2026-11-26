// SPDX-License-Identifier: LGPL-2.1-or-later
// 7zipl: runs an engine operation on a worker thread with 7-Zip's progress window
// (elapsed/remaining time, speed, sizes, ratio, Pause and Cancel) and its prompts.
#pragma once

#include "engine.hpp"

#include <gtk/gtk.h>

#include <functional>
#include <string>

namespace szl {

struct JobSpec {
    std::string title;          // "Compressing", "Extracting", "Testing", ...
    std::string archive;        // shown in the window title
    bool show_ratio = false;    // compression ratio and packed size rows
    int show_delay_ms = 0;      // only show the window when the job takes longer than this
    bool quiet_success = true;  // close the window on success without a message
    bool report_success = false;// show "Everything is Ok" on success (Test)
    std::string password;       // a known password for prompts that would ask for one
    bool silent = false;        // never show a result window (the caller reports errors)
};

using JobWork = std::function<Result(Ui &)>;
using JobDone = std::function<void(const Result &)>;

// Starts the job; `done` runs on the main thread after the worker finished.
void start_job(GtkWindow *parent, const JobSpec &spec, JobWork work, JobDone done = nullptr);

// Number of jobs still running (the main window waits for them on close).
int running_jobs();

}  // namespace szl
