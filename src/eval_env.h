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

#ifndef NINJA_EVAL_ENV_H_
#define NINJA_EVAL_ENV_H_

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
using namespace std;

#include "hashed_str_view.h"
#include "lexer.h"
#include "string_piece.h"

using DeclIndex = uint32_t;
const DeclIndex kLastDeclIndex = static_cast<DeclIndex>(-1);

struct Scope;

/// Position of a declaration within a scope.
struct ScopePosition {
  ScopePosition(Scope* scope=nullptr, DeclIndex index=0)
      : scope(scope), index(index) {}

  Scope* scope = nullptr;
  DeclIndex index = 0;
};

/// Position of a parsed manifest "clump" within its containing scope and within
/// all manifest files, in DFS order. This field is updated after parallelized
/// parsing is complete.
struct BasePosition {
  BasePosition(ScopePosition scope={}, DeclIndex dfs_location=0)
      : scope(scope), dfs_location(dfs_location) {}

  /// The scope location corresponding to this chunk. (Updated during DFS
  /// manifest parsing.)
  ScopePosition scope = {};

  /// Location of the chunk's declarations within DFS order of all the ninja
  /// files. This field is useful for verifying that pool references and default
  /// target references appear *after* their referents have been declared.
  DeclIndex dfs_location = 0;

  /// Check that the position is valid. (i.e. The clump it belongs to has been
  /// assigned a scope and a DFS position.)
  bool initialized() const {
    return scope.scope != nullptr;
  }
};

/// Offset of a manifest declaration relative to the beginning of a "clump" of
/// declarations.
struct RelativePosition {
  RelativePosition(BasePosition* base=nullptr, DeclIndex offset=0)
      : base(base), offset(offset) {}

  BasePosition* base = nullptr;
  DeclIndex offset = 0;

  DeclIndex dfs_location() const { Validate(); return base->dfs_location + offset; }
  DeclIndex scope_index() const { Validate(); return base->scope.index + offset; }
  Scope* scope() const { Validate(); return base->scope.scope; }
  ScopePosition scope_pos() const { Validate(); return { base->scope.scope, scope_index() }; }

private:
  void Validate() const {
    assert(base->initialized() && "clump position hasn't been set yet");
  }
};

/// An invokable build command and associated metadata (description, etc.).
struct Rule {
  Rule() {}

  /// This constructor is used to construct built-in rules.
  explicit Rule(const HashedStrView& name);

  const std::string& name() const { return name_.str(); }
  const HashedStr& name_hashed() const { return name_; }

  static bool IsReservedBinding(StringPiece var);

  const std::string* GetBinding(const HashedStrView& name) const {
    for (auto it = bindings_.rbegin(); it != bindings_.rend(); ++it) {
      if (it->first == name)
        return &it->second;
    }
    return nullptr;
  }

  /// Temporary fields used only during manifest parsing.
  struct {
    /// The position of the rule in its source file. Used for diagnostics.
    size_t rule_name_diag_pos = 0;
  } parse_state_;

  RelativePosition pos_;
  HashedStr name_;
  std::vector<std::pair<HashedStr, std::string>> bindings_;
};

struct Binding {
  /// This function is thread-safe. It stores a copy of the evaluated binding in
  /// the Binding object if it hasn't been evaluated yet.
  void Evaluate(std::string* out_append);

  const std::string& name() { return name_.str(); }
  const HashedStr& name_hashed() { return name_; }

  RelativePosition pos_;
  HashedStr name_;

  /// The manifest parser initializes this field with the location of the
  /// binding's unevaluated memory in the loaded manifest file.
  StringPiece parsed_value_;

private:
  /// This binding's value is evaluated lazily, but it must be evaluated before
  /// the manifest files are unloaded.
  std::atomic<bool> is_evaluated_;
  std::mutex mutex_;
  std::string final_value_;
};

struct Scope {
  Scope(ScopePosition parent) : parent_(parent) {}

  /// Preallocate space in the hash tables so adding bindings and rules is more
  /// efficient.
  void ReserveTableSpace(size_t new_bindings, size_t new_rules);

  void AddBinding(Binding* binding) {
    bindings_[binding->name_hashed()].push_back(binding);
  }

  /// Searches for a binding using a scope position (i.e. at parse time).
  /// Returns nullptr if the binding doesn't exist.
  static Binding* LookupBindingAtPos(const HashedStrView& var, ScopePosition pos);

  /// Searches for a binding in a scope and its ancestors. The position of a
  /// binding within its containing scope is ignored (i.e. post-parse lookup
  /// semantics). Returns nullptr if no binding is found. The Scope pointer may
  /// be nullptr.
  static Binding* LookupBinding(const HashedStrView& var, Scope* scope);

  /// Append a ${var} reference using a parse-time scope position.
  static void EvaluateVariableAtPos(std::string* out_append,
                                    const HashedStrView& var,
                                    ScopePosition pos);

  /// Append a ${var} reference using a post-parse scope, which may be nullptr.
  static void EvaluateVariable(std::string* out_append,
                               const HashedStrView& var,
                               Scope* scope);

  /// Convenience method for looking up the value of a binding after manifest
  /// parsing is finished.
  std::string LookupVariable(const HashedStrView& var);

  bool AddRule(Rule* rule) {
    fprintf(stderr, "eval_env\n");
    return rules_.insert({ rule->name_hashed(), rule }).second;
  }

  static Rule* LookupRuleAtPos(const HashedStrView& rule_name,
                               ScopePosition pos);

  /// Tests use this function to verify that a scope's rules are correct.
  std::map<std::string, const Rule*> GetRules() const;

  ScopePosition AllocDecls(DeclIndex count) {
    ScopePosition result = GetCurrentEndOfScope();
    pos_ += count;
    return result;
  }

  ScopePosition GetCurrentEndOfScope() { return { this, pos_ }; }

private:
  /// The position of this scope within its parent scope.
  /// (ScopePosition::parent will be nullptr for the root scope.)
  ScopePosition parent_;

  DeclIndex pos_ = 0;

  std::unordered_map<HashedStrView, std::vector<Binding*>> bindings_;

  /// A scope can only declare a rule with a particular name once. Even if a
  /// scope has a rule of a given name, a lookup for that name can still find a
  /// parent scope's rule if the search position comes before this scope's rule.
  std::unordered_map<HashedStrView, Rule*> rules_;
};

#endif  // NINJA_EVAL_ENV_H_
