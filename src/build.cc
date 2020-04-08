// Copyright 2011 Google Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "build.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <functional>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

#if defined(__SVR4) && defined(__sun)
#include <sys/termios.h>
#endif

#include "build_log.h"
#include "clparser.h"
#include "debug_flags.h"
#include "depfile_parser.h"
#include "deps_log.h"
#include "disk_interface.h"
#include "graph.h"
#include "metrics.h"
#include "state.h"
#include "status.h"
#include "subprocess.h"
#include "util.h"

namespace {

/// A CommandRunner that doesn't actually run the commands.
struct DryRunCommandRunner : public CommandRunner {
  virtual ~DryRunCommandRunner() {}

  // Overridden from CommandRunner:
  virtual bool CanRunMore();
  virtual bool StartCommand(Edge* edge);
  virtual bool WaitForCommand(Result* result);

 private:
  queue<Edge*> finished_;
};

bool DryRunCommandRunner::CanRunMore() {
  return true;
}

bool DryRunCommandRunner::StartCommand(Edge* edge) {
  finished_.push(edge);
  return true;
}

bool DryRunCommandRunner::WaitForCommand(Result* result) {
   if (finished_.empty())
     return false;

   result->status = ExitSuccess;
   result->edge = finished_.front();
   finished_.pop();
   return true;
}

}  // namespace

Plan::Plan() : command_edges_(0), wanted_edges_(0) {}

void Plan::Reset() {
  command_edges_ = 0;
  wanted_edges_ = 0;
  ready_.clear();
  want_.clear();
}

bool Plan::AddTarget(Node* node, string* err) {
  return AddSubTarget(node, NULL, err);
}

bool Plan::AddSubTarget(Node* node, Node* dependent, string* err) {
  Edge* edge = node->in_edge();
  if (!edge) {  // Leaf node.
    if (node->dirty()) {
      string referenced;
      if (dependent)
        referenced = ", needed by '" + dependent->path() + "',";
      *err = "'" + node->path() + "'" + referenced + " missing "
             "and no known rule to make it";
    }
    return false;
  }

  if (edge->outputs_ready())
    return false;  // Don't need to do anything.

  // If an entry in want_ does not already exist for edge, create an entry which
  // maps to kWantNothing, indicating that we do not want to build this entry itself.
  fprintf(stderr, "build1\n");
  pair<map<Edge*, Want>::iterator, bool> want_ins =
    want_.insert(make_pair(edge, kWantNothing));
  Want& want = want_ins.first->second;

  // If we do need to build edge and we haven't already marked it as wanted,
  // mark it now.
  if (node->dirty() && want == kWantNothing) {
    want = kWantToStart;
    ++wanted_edges_;
    if (edge->AllInputsReady())
      ScheduleWork(want_ins.first);
    if (!edge->is_phony())
      ++command_edges_;
  }

  if (!want_ins.second)
    return true;  // We've already processed the inputs.

  for (vector<Node*>::iterator i = edge->inputs_.begin();
       i != edge->inputs_.end(); ++i) {
    if (!AddSubTarget(*i, node, err) && !err->empty())
      return false;
  }

  return true;
}

Edge* Plan::FindWork() {
  if (ready_.empty())
    return NULL;
  EdgeSet::iterator e = ready_.begin();
  Edge* edge = *e;
  ready_.erase(e);
  return edge;
}

void Plan::ScheduleWork(map<Edge*, Want>::iterator want_e) {
  if (want_e->second == kWantToFinish) {
    // This edge has already been scheduled.  We can get here again if an edge
    // and one of its dependencies share an order-only input, or if a node
    // duplicates an out edge (see https://github.com/ninja-build/ninja/pull/519).
    // Avoid scheduling the work again.
    return;
  }
  assert(want_e->second == kWantToStart);
  want_e->second = kWantToFinish;

  Edge* edge = want_e->first;
  Pool* pool = edge->pool();
  if (pool->ShouldDelayEdge()) {
    pool->DelayEdge(edge);
    pool->RetrieveReadyEdges(&ready_);
  } else {
    pool->EdgeScheduled(*edge);
  fprintf(stderr, "build2\n");
    ready_.insert(edge);
  }
}

void Plan::EdgeFinished(Edge* edge, EdgeResult result) {
  map<Edge*, Want>::iterator e = want_.find(edge);
  assert(e != want_.end());
  bool directly_wanted = e->second != kWantNothing;

  // See if this job frees up any delayed jobs.
  if (directly_wanted)
    edge->pool()->EdgeFinished(*edge);
  edge->pool()->RetrieveReadyEdges(&ready_);

  // The rest of this function only applies to successful commands.
  if (result != kEdgeSucceeded)
    return;

  if (directly_wanted)
    --wanted_edges_;
  want_.erase(e);
  edge->outputs_ready_ = true;

  // Check off any nodes we were waiting for with this edge.
  for (vector<Node*>::iterator o = edge->outputs_.begin();
       o != edge->outputs_.end(); ++o) {
    NodeFinished(*o);
  }
}

void Plan::NodeFinished(Node* node) {
  // See if we we want any edges from this node.
  const std::vector<Edge*> out_edges = node->GetOutEdges();
  for (vector<Edge*>::const_iterator oe = out_edges.begin();
       oe != out_edges.end(); ++oe) {
    map<Edge*, Want>::iterator want_e = want_.find(*oe);
    if (want_e == want_.end())
      continue;

    // See if the edge is now ready.
    if ((*oe)->AllInputsReady()) {
      if (want_e->second != kWantNothing) {
        ScheduleWork(want_e);
      } else {
        // We do not need to build this edge, but we might need to build one of
        // its dependents.
        EdgeFinished(*oe, kEdgeSucceeded);
      }
    }
  }
}

bool Plan::CleanNode(DependencyScan* scan, Node* node, string* err) {
  node->set_dirty(false);

  const std::vector<Edge*> out_edges = node->GetOutEdges();
  for (vector<Edge*>::const_iterator oe = out_edges.begin();
       oe != out_edges.end(); ++oe) {
    // Don't process edges that we don't actually want.
    map<Edge*, Want>::iterator want_e = want_.find(*oe);
    if (want_e == want_.end() || want_e->second == kWantNothing)
      continue;

    // Don't attempt to clean an edge if it failed to load deps.
    if ((*oe)->deps_missing_)
      continue;

    // No need to clean a phony output edge, as it's always dirty
    if ((*oe)->IsPhonyOutput())
      continue;

    // If all non-order-only inputs for this edge are now clean,
    // we might have changed the dirty state of the outputs.
    vector<Node*>::iterator
        begin = (*oe)->inputs_.begin(),
        end = (*oe)->inputs_.end() - (*oe)->order_only_deps_;
#if __cplusplus < 201703L
#define MEM_FN mem_fun
#else
#define MEM_FN mem_fn  // mem_fun was removed in C++17.
#endif
    if (find_if(begin, end, MEM_FN(&Node::dirty)) == end) {
      // Recompute most_recent_input.
      Node* most_recent_input = NULL;
      for (vector<Node*>::iterator i = begin; i != end; ++i) {
        if (!most_recent_input || (*i)->mtime() > most_recent_input->mtime())
          most_recent_input = *i;
      }

      // Now, this edge is dirty if any of the outputs are dirty.
      // If the edge isn't dirty, clean the outputs and mark the edge as not
      // wanted.
      bool outputs_dirty = false;
      if (!scan->RecomputeOutputsDirty(*oe, most_recent_input,
                                       &outputs_dirty, err)) {
        return false;
      }
      if (!outputs_dirty) {
        for (vector<Node*>::iterator o = (*oe)->outputs_.begin();
             o != (*oe)->outputs_.end(); ++o) {
          if (!CleanNode(scan, *o, err))
            return false;
        }

        want_e->second = kWantNothing;
        --wanted_edges_;
        if (!(*oe)->is_phony())
          --command_edges_;
      }
    }
  }
  return true;
}

void Plan::Dump() {
  printf("pending: %d\n", (int)want_.size());
  for (map<Edge*, Want>::iterator e = want_.begin(); e != want_.end(); ++e) {
    if (e->second != kWantNothing)
      printf("want ");
    e->first->Dump();
  }
  printf("ready: %d\n", (int)ready_.size());
}

struct RealCommandRunner : public CommandRunner {
  explicit RealCommandRunner(const BuildConfig& config) : config_(config) {}
  virtual ~RealCommandRunner() {}
  virtual bool CanRunMore();
  virtual bool StartCommand(Edge* edge);
  virtual bool WaitForCommand(Result* result);
  virtual vector<Edge*> GetActiveEdges();
  virtual void Abort();

  const BuildConfig& config_;
  SubprocessSet subprocs_;
  map<Subprocess*, Edge*> subproc_to_edge_;
};

vector<Edge*> RealCommandRunner::GetActiveEdges() {
  vector<Edge*> edges;
  for (map<Subprocess*, Edge*>::iterator e = subproc_to_edge_.begin();
       e != subproc_to_edge_.end(); ++e)
    edges.push_back(e->second);
  return edges;
}

void RealCommandRunner::Abort() {
  subprocs_.Clear();
}

bool RealCommandRunner::CanRunMore() {
  size_t subproc_number =
      subprocs_.running_.size() + subprocs_.finished_.size();
  return (int)subproc_number < config_.parallelism
    && ((subprocs_.running_.empty() || config_.max_load_average <= 0.0f)
        || GetLoadAverage() < config_.max_load_average);
}

bool RealCommandRunner::StartCommand(Edge* edge) {
  string command = edge->EvaluateCommand();
  Subprocess* subproc = subprocs_.Add(command, edge->use_console());
  if (!subproc)
    return false;
  fprintf(stderr, "build3 -%s-\n", command.c_str());
  subproc_to_edge_.insert(make_pair(subproc, edge));
  fprintf(stderr, "build3 done\n");

  return true;
}

bool RealCommandRunner::WaitForCommand(Result* result) {
  Subprocess* subproc;
  while ((subproc = subprocs_.NextFinished()) == NULL) {
    bool interrupted = subprocs_.DoWork();
    if (interrupted)
      return false;
  }

  result->status = subproc->Finish();
#ifndef _WIN32
  result->rusage = *subproc->GetUsage();
#endif
  result->output = subproc->GetOutput();

  map<Subprocess*, Edge*>::iterator e = subproc_to_edge_.find(subproc);
  result->edge = e->second;
  subproc_to_edge_.erase(e);

  delete subproc;
  return true;
}

Builder::Builder(State* state, const BuildConfig& config,
                 BuildLog* build_log, DepsLog* deps_log,
                 DiskInterface* disk_interface, Status* status,
                 int64_t start_time_millis)
    : state_(state), config_(config), status_(status),
      start_time_millis_(start_time_millis), disk_interface_(disk_interface),
      scan_(state, build_log, deps_log, disk_interface, config.uses_phony_outputs) {
}

Builder::~Builder() {
  Cleanup();
}

void Builder::Cleanup() {
  if (command_runner_.get()) {
    vector<Edge*> active_edges = command_runner_->GetActiveEdges();
    command_runner_->Abort();

    for (vector<Edge*>::iterator e = active_edges.begin();
         e != active_edges.end(); ++e) {
      if ((*e)->IsPhonyOutput())
        continue;
      string depfile = (*e)->GetUnescapedDepfile();
      for (vector<Node*>::iterator o = (*e)->outputs_.begin();
           o != (*e)->outputs_.end(); ++o) {
        // Only delete this output if it was actually modified.  This is
        // important for things like the generator where we don't want to
        // delete the manifest file if we can avoid it.  But if the rule
        // uses a depfile, always delete.  (Consider the case where we
        // need to rebuild an output because of a modified header file
        // mentioned in a depfile, and the command touches its depfile
        // but is interrupted before it touches its output file.)
        string err;
        bool is_dir = false;
        TimeStamp new_mtime = disk_interface_->LStat((*o)->path(), &is_dir, &err);
        if (new_mtime == -1)  // Log and ignore LStat() errors.
          status_->Error("%s", err.c_str());
        if (!is_dir && (!depfile.empty() || (*o)->mtime() != new_mtime))
          disk_interface_->RemoveFile((*o)->path());
      }
      if (!depfile.empty())
        disk_interface_->RemoveFile(depfile);
    }
  }
}

Node* Builder::AddTarget(const string& name, string* err) {
  Node* node = state_->LookupNode(name);
  if (!node) {
    *err = "unknown target: '" + name + "'";
    return NULL;
  }
  if (!AddTargets({ node }, err))
    return NULL;
  return node;
}

bool Builder::AddTargets(const std::vector<Node*> &nodes, string* err) {
  if (!scan_.RecomputeNodesDirty(nodes, err))
    return false;

  for (Node* node : nodes) {
    std::string plan_err;
    if (!plan_.AddTarget(node, &plan_err)) {
      if (!plan_err.empty()) {
        *err = plan_err;
        return false;
      } else {
        // Added a target that is already up-to-date; not really
        // an error.
      }
    }
  }

  return true;
}

bool Builder::AlreadyUpToDate() const {
  return !plan_.more_to_do();
}

bool Builder::Build(string* err) {
  assert(!AlreadyUpToDate());

  status_->PlanHasTotalEdges(plan_.command_edge_count());
  int pending_commands = 0;
  int failures_allowed = config_.failures_allowed;

  // Set up the command runner if we haven't done so already.
  if (!command_runner_.get()) {
    if (config_.dry_run)
      command_runner_.reset(new DryRunCommandRunner);
    else
      command_runner_.reset(new RealCommandRunner(config_));
  }

  // We are about to start the build process.
  status_->BuildStarted();

  // This main loop runs the entire build process.
  // It is structured like this:
  // First, we attempt to start as many commands as allowed by the
  // command runner.
  // Second, we attempt to wait for / reap the next finished command.
  while (plan_.more_to_do()) {
    // See if we can start any more commands.
    if (failures_allowed && command_runner_->CanRunMore()) {
      if (Edge* edge = plan_.FindWork()) {
        if (!StartEdge(edge, err)) {
          Cleanup();
          status_->BuildFinished();
          return false;
        }

        if (edge->is_phony()) {
          plan_.EdgeFinished(edge, Plan::kEdgeSucceeded);
        } else {
          ++pending_commands;
        }

        // We made some progress; go back to the main loop.
        continue;
      }
    }

    // See if we can reap any finished commands.
    if (pending_commands) {
      CommandRunner::Result result;
      if (!command_runner_->WaitForCommand(&result) ||
          result.status == ExitInterrupted) {
        Cleanup();
        status_->BuildFinished();
        *err = "interrupted by user";
        return false;
      }

      --pending_commands;
      if (!FinishCommand(&result, err)) {
        Cleanup();
        status_->BuildFinished();
        return false;
      }

      if (!result.success()) {
        if (failures_allowed)
          failures_allowed--;
      }

      // We made some progress; start the main loop over.
      continue;
    }

    // If we get here, we cannot make any more progress.
    status_->BuildFinished();
    if (failures_allowed == 0) {
      if (config_.failures_allowed > 1)
        *err = "subcommands failed";
      else
        *err = "subcommand failed";
    } else if (failures_allowed < config_.failures_allowed)
      *err = "cannot make progress due to previous errors";
    else
      *err = "stuck [this is a bug]";

    return false;
  }

  status_->BuildFinished();
  return true;
}

bool Builder::StartEdge(Edge* edge, string* err) {
  METRIC_RECORD("StartEdge");
  if (edge->is_phony())
    return true;

  int64_t start_time_millis = GetTimeMillis() - start_time_millis_;
  fprintf(stderr, "build4\n");
  running_edges_.insert(make_pair(edge, start_time_millis));

  status_->BuildEdgeStarted(edge, start_time_millis);

  if (!edge->IsPhonyOutput()) {
    for (vector<Node*>::iterator o = edge->outputs_.begin();
         o != edge->outputs_.end(); ++o) {
      // Create directories necessary for outputs.
      // XXX: this will block; do we care?
      if (!disk_interface_->MakeDirs((*o)->path()))
        return false;

      if (!(*o)->exists())
        continue;

      // Remove existing outputs for non-restat rules.
      // XXX: this will block; do we care?
      if (config_.pre_remove_output_files && !edge->IsRestat() && !config_.dry_run) {
        if (disk_interface_->RemoveFile((*o)->path()) < 0)
          return false;
      }
    }
  }

  // Create response file, if needed
  // XXX: this may also block; do we care?
  string rspfile = edge->GetUnescapedRspfile();
  if (!rspfile.empty()) {
    string content = edge->GetBinding("rspfile_content");
    if (!disk_interface_->WriteFile(rspfile, content))
      return false;
  }

  // start command computing and run it
  if (!command_runner_->StartCommand(edge)) {
    err->assign("command '" + edge->EvaluateCommand() + "' failed.");
    return false;
  }

  return true;
}

bool Builder::FinishCommand(CommandRunner::Result* result, string* err) {
  METRIC_RECORD("FinishCommand");

  Edge* edge = result->edge;
  bool phony_output = edge->IsPhonyOutput();

  vector<Node*> deps_nodes;
  string deps_type = edge->GetBinding("deps");
  if (!phony_output) {
    // First try to extract dependencies from the result, if any.
    // This must happen first as it filters the command output (we want
    // to filter /showIncludes output, even on compile failure) and
    // extraction itself can fail, which makes the command fail from a
    // build perspective.
    const string deps_prefix = edge->GetBinding("msvc_deps_prefix");
    if (!deps_type.empty()) {
      string extract_err;
      if (!ExtractDeps(result, deps_type, deps_prefix, &deps_nodes,
                       &extract_err) &&
          result->success()) {
        if (!result->output.empty())
          result->output.append("\n");
        result->output.append(extract_err);
        result->status = ExitFailure;
      }
    }
  }

  int64_t start_time_millis, end_time_millis;
  RunningEdgeMap::iterator i = running_edges_.find(edge);
  start_time_millis = i->second;
  end_time_millis = GetTimeMillis() - start_time_millis_;
  running_edges_.erase(i);

  // Restat the edge outputs
  TimeStamp output_mtime = 0;
  if (result->success() && !config_.dry_run && !phony_output) {
    bool restat = edge->IsRestat();
    vector<Node*> nodes_cleaned;

    TimeStamp newest_input = 0;
    Node* newest_input_node = nullptr;
    for (vector<Node*>::iterator i = edge->inputs_.begin();
         i != edge->inputs_.end() - edge->order_only_deps_; ++i) {
      TimeStamp input_mtime = (*i)->mtime();
      if (input_mtime == -1)
        return false;
      if (input_mtime > newest_input) {
        newest_input = input_mtime;
        newest_input_node = (*i);
      }
    }

    for (vector<Node*>::iterator o = edge->outputs_.begin();
         o != edge->outputs_.end(); ++o) {
      bool is_dir = false;
      TimeStamp old_mtime = (*o)->mtime();
      if (!(*o)->LStat(disk_interface_, &is_dir, err))
        return false;
      TimeStamp new_mtime = (*o)->mtime();
      if (config_.uses_phony_outputs) {
        if (new_mtime == 0) {
          if (!result->output.empty())
            result->output.append("\n");
          result->output.append("ninja: output file missing after successful execution: ");
          result->output.append((*o)->path());
          if (config_.missing_output_file_should_err) {
            result->status = ExitFailure;
          }
        } else if (!restat && new_mtime < newest_input) {
          if (!result->output.empty())
            result->output.append("\n");
          result->output.append("ninja: Missing `restat`? An output file is older than the most recent input:\n output: ");
          result->output.append((*o)->path());
          result->output.append("\n  input: ");
          result->output.append(newest_input_node->path());
          if (config_.old_output_should_err) {
            result->status = ExitFailure;
          }
        }
        if (is_dir) {
          if (!result->output.empty())
            result->output.append("\n");
          result->output.append("ninja: outputs should be files, not directories: ");
          result->output.append((*o)->path());
          if (config_.output_directory_should_err) {
            result->status = ExitFailure;
          }
        }
      }
      if (new_mtime > output_mtime)
        output_mtime = new_mtime;
      if (old_mtime == new_mtime && restat) {
        nodes_cleaned.push_back(*o);
        continue;
      }
    }

    status_->BuildEdgeFinished(edge, end_time_millis, result);

    if (result->success() && !nodes_cleaned.empty()) {
      for (vector<Node*>::iterator o = nodes_cleaned.begin();
           o != nodes_cleaned.end(); ++o) {
        // The rule command did not change the output.  Propagate the clean
        // state through the build graph.
        // Note that this also applies to nonexistent outputs (mtime == 0).
        if (!plan_.CleanNode(&scan_, *o, err))
          return false;
      }

      // If any output was cleaned, find the most recent mtime of any
      // (existing) non-order-only input or the depfile.
      TimeStamp restat_mtime = newest_input;

      string depfile = edge->GetUnescapedDepfile();
      if (restat_mtime != 0 && deps_type.empty() && !depfile.empty()) {
        TimeStamp depfile_mtime = disk_interface_->Stat(depfile, err);
        if (depfile_mtime == -1)
          return false;
        if (depfile_mtime > restat_mtime)
          restat_mtime = depfile_mtime;
      }

      // The total number of edges in the plan may have changed as a result
      // of a restat.
      status_->PlanHasTotalEdges(plan_.command_edge_count());

      output_mtime = restat_mtime;
    }
  } else {
    status_->BuildEdgeFinished(edge, end_time_millis, result);
  }

  plan_.EdgeFinished(edge, result->success() ? Plan::kEdgeSucceeded : Plan::kEdgeFailed);

  // The rest of this function only applies to successful commands.
  if (!result->success()) {
    return true;
  }

  // Delete any left over response file.
  string rspfile = edge->GetUnescapedRspfile();
  if (!rspfile.empty() && !g_keep_rsp)
    disk_interface_->RemoveFile(rspfile);

  if (scan_.build_log() && !phony_output) {
    if (!scan_.build_log()->RecordCommand(edge, start_time_millis,
                                          end_time_millis, output_mtime)) {
      *err = string("Error writing to build log: ") + strerror(errno);
      return false;
    }
  }

  if (!deps_type.empty() && !config_.dry_run && !phony_output) {
    Node* out = edge->outputs_[0];
    TimeStamp deps_mtime = disk_interface_->LStat(out->path(), nullptr, err);
    if (deps_mtime == -1)
      return false;
    if (!scan_.deps_log()->RecordDeps(out, deps_mtime, deps_nodes)) {
      *err = string("Error writing to deps log: ") + strerror(errno);
      return false;
    }
  }
  return true;
}

bool Builder::ExtractDeps(CommandRunner::Result* result,
                          const string& deps_type,
                          const string& deps_prefix,
                          vector<Node*>* deps_nodes,
                          string* err) {
  if (deps_type == "msvc") {
    CLParser parser;
    string output;
    if (!parser.Parse(result->output, deps_prefix, &output, err))
      return false;
    result->output = output;
    for (set<string>::iterator i = parser.includes_.begin();
         i != parser.includes_.end(); ++i) {
      // ~0 is assuming that with MSVC-parsed headers, it's ok to always make
      // all backslashes (as some of the slashes will certainly be backslashes
      // anyway). This could be fixed if necessary with some additional
      // complexity in IncludesNormalize::Relativize.
	fprintf(stderr, "build 11\n");
      deps_nodes->push_back(state_->GetNode(*i, ~0u));
    }
  } else
  if (deps_type == "gcc") {
    string depfile = result->edge->GetUnescapedDepfile();
    if (depfile.empty()) {
      *err = string("edge with deps=gcc but no depfile makes no sense");
      return false;
    }

    // Read depfile content.  Treat a missing depfile as empty.
    string content;
    switch (disk_interface_->ReadFile(depfile, &content, err)) {
    case DiskInterface::Okay:
      break;
    case DiskInterface::NotFound:
      err->clear();
      // We only care if the depfile is missing when the tool succeeded.
      if (!config_.dry_run && result->status == ExitSuccess) {
        if (config_.missing_depfile_should_err) {
          *err = string("depfile is missing");
          return false;
        } else {
          status_->Warning("depfile is missing (%s for %s)", depfile.c_str(), result->edge->outputs_[0]->path().c_str());
        }
      }
      break;
    case DiskInterface::OtherError:
      return false;
    }
    if (content.empty())
      return true;

    DepfileParser deps;
    if (!deps.Parse(&content, err))
      return false;

    // XXX check depfile matches expected output.
    deps_nodes->reserve(deps.ins_.size());
    for (vector<StringPiece>::iterator i = deps.ins_.begin();
         i != deps.ins_.end(); ++i) {
      uint64_t slash_bits;
      if (!CanonicalizePath(const_cast<char*>(i->str_), &i->len_, &slash_bits,
                            err))
        return false;
	fprintf(stderr, "build 12 -%s-\n", depfile.c_str());
      deps_nodes->push_back(state_->GetNode(*i, slash_bits));
    }

    if (!g_keep_depfile) {
      if (disk_interface_->RemoveFile(depfile) < 0) {
        *err = string("deleting depfile: ") + strerror(errno) + string("\n");
        return false;
      }
    }
  } else {
    Fatal("unknown deps type '%s'", deps_type.c_str());
  }

  return true;
}
