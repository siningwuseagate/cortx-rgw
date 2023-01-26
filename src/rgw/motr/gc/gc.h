// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=2 sw=2 expandtab ft=cpp

/*
 * Garbage Collector Classes for the CORTX Motr backend
 *
 * Copyright (C) 2022 Seagate Technology LLC and/or its Affiliates
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation. See file COPYING.
 *
 */

#ifndef __MOTR_GC_H__
#define __MOTR_GC_H__

#include "rgw_sal_motr.h"
#include "common/Thread.h"
#include <mutex>
#include <condition_variable>
#include <atomic>

const uint32_t GC_DEFAULT_QUEUES = 64;
const uint32_t GC_DEFAULT_COUNT = 256;
const uint32_t GC_MAX_QUEUES = 4096;
const uint32_t GC_CALLER_ID_STR_LEN = 32;
static const std::string gc_index_prefix = "motr.rgw.gc";
static const std::string gc_thread_prefix = "motr_gc_";
static const std::string obj_tag_prefix = "0_";
static const std::string obj_exp_time_prefix = "1_";
const std::string global_lock_table = "motr.rgw.lock";

namespace rgw::sal {
  class MotrStore;
  class MotrObjectMeta;
  class MotrObject;
}

struct motr_gc_obj_info {
  std::string tag;                // gc obj unique identifier
  std::string name;               // fully qualified object name
  rgw::sal::MotrObjectMeta *mobj; // motr obj (use pointer to avoid circular dependency)
  std::time_t deletion_time;      // time when Motr object was requested for deletion
  std::uint64_t size;             // size of obj
  bool is_multipart;              // flag to indicate if object is multipart
  std::string multipart_iname;    // part index name

  motr_gc_obj_info() {}
  motr_gc_obj_info(const std::string& _tag, const std::string& _name,
		   rgw::sal::MotrObjectMeta* _mobj,
                   const std::time_t& _deletion_time, const std::uint64_t& _size,
                   const std::string& _multipart_iname="")
      : tag(_tag), name(_name), mobj(_mobj), deletion_time(_deletion_time),
        size(_size), is_multipart(_multipart_iname != ""),
        multipart_iname(_multipart_iname) {}

  void encode(bufferlist &bl) const;
  void decode(bufferlist::const_iterator &bl);
};
WRITE_CLASS_ENCODER(motr_gc_obj_info);

class MotrGC : public DoutPrefixProvider {
 private:
  CephContext *cct;
  rgw::sal::MotrStore *store;
  uint32_t max_indices = 0;
  uint32_t max_count = 0;
  std::atomic<uint32_t> enqueue_index;
  std::vector<std::string> index_names;
  std::atomic<bool> down_flag = false;
  std::string caller_id = "";

 public:
  class GCWorker : public Thread {
   private:
    const DoutPrefixProvider *dpp;
    CephContext *cct;
    MotrGC *motr_gc;
    int worker_id;
    std::mutex lock;
    std::condition_variable cv;
   public:
    GCWorker(const DoutPrefixProvider* _dpp, CephContext *_cct,
             MotrGC *_motr_gc, int _worker_id)
      : dpp(_dpp),
        cct(_cct),
        motr_gc(_motr_gc),
        worker_id(_worker_id) {};

    void *entry() override;
    void stop();
    int get_id() { return worker_id; }
  };

  std::vector<std::unique_ptr<MotrGC::GCWorker>> workers;

  MotrGC(CephContext *_cct, rgw::sal::MotrStore* _store)
    : cct(_cct), store(_store) {}

  ~MotrGC() {
    stop_processor();
    finalize();
  }
  int initialize();
  void finalize();

  void start_processor();
  void stop_processor();
  bool going_down();

  uint32_t get_max_indices();
  int enqueue(motr_gc_obj_info obj);
  int dequeue(std::string iname, motr_gc_obj_info obj);

  int list(std::vector<std::unordered_map<std::string, std::string>> &gc_entries,
                        std::vector<std::string>& inac_queues);
  int delete_obj_from_gc(motr_gc_obj_info ginfo);
  int delete_multipart_obj_from_gc(motr_gc_obj_info ginfo, std::time_t end_time);
  int delete_motr_obj(rgw::sal::MotrObjectMeta motr_obj);
  int get_locked_gc_index(uint32_t& rand_ind, uint32_t& lease_duration);
  int un_lock_gc_index(uint32_t& index);

  // Set Up logging prefix for GC
  CephContext *get_cct() const override { return cct; }
  unsigned get_subsys() const;
  std::ostream& gen_prefix(std::ostream& out) const;
};

#endif
