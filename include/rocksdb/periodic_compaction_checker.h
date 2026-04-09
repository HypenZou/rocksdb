// Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#pragma once

#include <memory>
#include <string>

#include "rocksdb/customizable.h"
#include "rocksdb/table_properties.h"
#include "rocksdb/utilities/customizable_util.h"

namespace ROCKSDB_NAMESPACE {

// PeriodicCompactionChecker allows applications to further refine the
// eligibility of age-qualified periodic compaction candidates on a per-file
// basis. A checker is invoked in a background compaction thread and never while
// holding the DB mutex.
//
// Typical usage is to interpret TableProperties::user_collected_properties
// written by a TablePropertiesCollector and decide whether rewriting the file
// is worthwhile.
//
// Exceptions MUST NOT propagate out of overridden functions into RocksDB,
// because RocksDB is not exception-safe. This could cause undefined behavior
// including data loss, unreported corruption, deadlocks, and more.
class PeriodicCompactionChecker {
 public:
  struct Context {
    uint32_t column_family_id = 0;
    int level = 0;
    bool is_bottommost_level = false;
    uint64_t current_time = 0;
    uint64_t periodic_compaction_seconds = 0;
    uint64_t file_age = 0;
    uint64_t file_number = 0;
  };

  virtual ~PeriodicCompactionChecker() = default;

  virtual bool ShouldCompact(const Context& context,
                             const TableProperties& table_props) const = 0;

  virtual const char* Name() const = 0;
};

class PeriodicCompactionCheckerFactory : public Customizable {
 public:
  ~PeriodicCompactionCheckerFactory() override = default;

  static const char* Type() { return "PeriodicCompactionCheckerFactory"; }

  static inline Status CreateFromString(
      const ConfigOptions& config_options, const std::string& value,
      std::shared_ptr<PeriodicCompactionCheckerFactory>* result) {
    return LoadSharedObject<PeriodicCompactionCheckerFactory>(
        config_options, value, result);
  }

  virtual std::unique_ptr<PeriodicCompactionChecker>
  CreatePeriodicCompactionChecker() = 0;

  virtual const char* Name() const override = 0;
};

}  // namespace ROCKSDB_NAMESPACE
