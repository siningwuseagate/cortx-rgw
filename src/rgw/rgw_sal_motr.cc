// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=2 sw=2 expandtab ft=cpp

/*
 * Ceph - scalable distributed file system
 *
 * SAL implementation for the CORTX Motr backend
 *
 * Copyright (C) 2021 Seagate Technology LLC and/or its Affiliates
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation. See file COPYING.
 *
 */

#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include <chrono>

extern "C" {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wextern-c-compat"
#include "motr/config.h"
#include "lib/types.h"
#include "lib/trace.h"   // m0_trace_set_mmapped_buffer
#include "motr/layout.h" // M0_OBJ_LAYOUT_ID
#include "helpers/helpers.h" // m0_ufid_next
#include "lib/thread.h"	     // m0_thread_adopt
#pragma clang diagnostic pop
}

#include "common/Clock.h"
#include "common/errno.h"

#include "rgw_compression.h"
#include "rgw_sal.h"
#include "rgw_sal_motr.h"
#include "rgw_bucket.h"
#include "rgw_quota.h"
#include "motr/addb/rgw_addb.h"
#include "rgw_rest.h"

#define dout_subsys ceph_subsys_rgw

using std::string;
using std::map;
using std::vector;
using std::set;
using std::list;

static string mp_ns = RGW_OBJ_NS_MULTIPART;
static struct m0_ufid_generator ufid_gr;

namespace rgw::sal {
static const unsigned MAX_ACC_SIZE = 32 * 1024 * 1024;

using ::ceph::encode;
using ::ceph::decode;

class MotrADDBLogger {
private:
  uint64_t req_id;
  bool is_m0_thread = false;
  struct m0_thread thread;
  static struct m0* m0_instance;
public:
  MotrADDBLogger() {
    struct m0_thread_tls *tls = m0_thread_tls();

    req_id = (uint64_t)-1;
    memset(&thread, 0, sizeof(struct m0_thread));

    // m0_thread_tls() always return non-NULL pointer to
    // actual thread tls. Motr and non-Motr threads can be
    // distinguished by checking of addb2_mach. Motr thread
    // has addb2_mach assigned, while non-Motr haven't.
    if (tls->tls_addb2_mach == NULL)
    {
      M0_ASSERT(m0_instance != nullptr);
      m0_thread_adopt(&thread, m0_instance);
    } else {
      is_m0_thread = true;
    }
  }

  ~MotrADDBLogger() {
    if (!is_m0_thread) {
      m0_addb2_force_all();
      m0_thread_arch_shun();
    }
  }

  void set_id(uint64_t id) {
    req_id = id;
  }

  void set_id(RGWObjectCtx* rctx) {
    struct req_state* s = static_cast<req_state*>(rctx->get_private());
    req_id = s->id;
  }

  uint64_t get_id() {
    return req_id;
  }

  static void set_m0_instance(struct m0* instance) {
    m0_instance = instance;
  }
};

struct m0* MotrADDBLogger::m0_instance = nullptr;

static thread_local MotrADDBLogger addb_logger;

static std::string motr_global_indices[] = {
  RGW_MOTR_USERS_IDX_NAME,
  RGW_MOTR_BUCKET_INST_IDX_NAME,
  RGW_MOTR_BUCKET_HD_IDX_NAME,
  RGW_IAM_MOTR_ACCESS_KEY,
  RGW_IAM_MOTR_EMAIL_KEY
};

// version-id(31 byte = base62 timstamp(8-byte) + UUID(23 byte)
#define TS_LEN 8
#define UUID_LEN 23

static uint64_t roundup(uint64_t x, uint64_t by)
{
  if (x == 0)
    return 0;
  return ((x - 1) / by + 1) * by;
}

static uint64_t rounddown(uint64_t x, uint64_t by)
{
  return x / by * by;
}

int parse_tags(const DoutPrefixProvider* dpp, bufferlist& tags_bl, struct req_state* s)
{
  std::unique_ptr<RGWObjTags> obj_tags;
  if (s->info.env->exists("HTTP_X_AMZ_TAGGING")) {
    auto tag_str = s->info.env->get("HTTP_X_AMZ_TAGGING");
    obj_tags = std::make_unique<RGWObjTags>();
    int ret = obj_tags->set_from_string(tag_str);
    if (ret < 0) {
      ldpp_dout(dpp, LOG_ERROR) <<__func__ << ": ERROR: setting obj tags failed with rc=" << ret << dendl;
      if (ret == -ERR_INVALID_TAG) {
        ret = -EINVAL; //s3 returns only -EINVAL for PUT requests
      }
      return ret;
    }
    obj_tags->encode(tags_bl);
  }
  return 0;
}

std::string base62_encode(uint64_t value, size_t pad)
{
  // Integer to Base62 encoding table. Characters are sorted in
  // lexicographical order, which makes the encoded result
  // also sortable in the same way as the integer source.
  constexpr std::array<char, 62> base62_chars{
      // 0-9
      '0', '1', '2', '3', '4', '5', '6', '7', '8', '9',
      // A-Z
      'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M',
      'N', 'O', 'P', 'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z',
      // a-z
      'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l', 'm',
      'n', 'o', 'p', 'q', 'r', 's', 't', 'u', 'v', 'w', 'x', 'y', 'z'};

  std::string ret;
  ret.reserve(TS_LEN);
  if (value == 0) {
    ret = base62_chars[0];
  }

  while (value > 0) {
    ret += base62_chars[value % base62_chars.size()];
    value /= base62_chars.size();
  }
  reverse(ret.begin(), ret.end());
  if (ret.size() < pad) ret.insert(0, pad - ret.size(), base62_chars[0]);

  return ret;
}

inline std::string get_bucket_name(const std::string& tenant,  const std::string& bucket)
{
  if (tenant != "")
    return tenant + "$" + bucket;
  else
    return bucket;
}

int static update_bucket_stats(const DoutPrefixProvider *dpp, MotrStore *store,
                               std::string owner, std::string bucket_name,
                               uint64_t size, uint64_t actual_size,
                               uint64_t num_objects = 1, bool add_stats = true) {
  uint64_t multiplier = add_stats ? 1 : -1;
  bufferlist bl;
  std::string user_stats_iname = "motr.rgw.user.stats." + owner;
  rgw_bucket_dir_header bkt_header;
  int rc = store->do_idx_op_by_name(user_stats_iname,
                            M0_IC_GET, bucket_name, bl);
  if (rc != 0) {
    ldpp_dout(dpp, LOG_ERROR) <<__func__ << ": ERROR: Failed to get the bucket header."
      << " bucket=" << bucket_name << ", ret=" << rc << dendl;
    return rc;
  }

  bufferlist::const_iterator bitr = bl.begin();
  bkt_header.decode(bitr);
  rgw_bucket_category_stats& bkt_stat = bkt_header.stats[RGWObjCategory::Main];
  bkt_stat.num_entries += multiplier * num_objects;
  bkt_stat.total_size += multiplier * size;
  bkt_stat.actual_size += multiplier * actual_size;

  bl.clear();
  bkt_header.encode(bl);
  rc = store->do_idx_op_by_name(user_stats_iname, M0_IC_PUT, bucket_name, bl);
  return rc;
}

void MotrMetaCache::invalid(const DoutPrefixProvider *dpp,
                           const string& name)
{
  cache.invalidate_remove(dpp, name);
}

int MotrMetaCache::put(const DoutPrefixProvider *dpp,
                       const string& name,
                       const bufferlist& data)
{
  ldpp_dout(dpp, 0) <<__func__ << ": Put into cache: name=" << name << dendl;

  ObjectCacheInfo info;
  info.status = 0;
  info.data.append(data);
  info.flags = CACHE_FLAG_DATA;
  info.meta.mtime = ceph::real_clock::now();
  info.meta.size = data.length();
  cache.put(dpp, name, info, NULL);

  // Inform other rgw instances. Do nothing if it gets some error?
  int rc = distribute_cache(dpp, name, info, UPDATE_OBJ);
  if (rc < 0)
    ldpp_dout(dpp, LOG_ERROR) <<__func__ <<": ERROR: failed to distribute cache for " << name << dendl;

  ldpp_dout(dpp, 0) <<__func__ << ": Put into cache: name=" << name << ": success" << dendl;
  return 0;
}

int MotrMetaCache::get(const DoutPrefixProvider *dpp,
                       const string& name, bufferlist& data)
{
  ObjectCacheInfo info;
  uint32_t flags = CACHE_FLAG_DATA;
  int rc = cache.get(dpp, name, info, flags, NULL);
  if (rc == 0) {
    if (info.status < 0)
      return info.status;

    bufferlist& bl = info.data;
    bufferlist::iterator it = bl.begin();
    data.clear();

    it.copy_all(data);
    ldpp_dout(dpp, 0) <<__func__ << ": Cache hit: name=" << name << dendl;
    return 0;
  }

  ldpp_dout(dpp, 0) <<__func__ << ": Cache miss: name=" << name << ", rc="<< rc << dendl;
  if (rc == -ENODATA)
    rc = -ENOENT;

  return rc;
}

int MotrMetaCache::remove(const DoutPrefixProvider *dpp,
                          const string& name)

{
  cache.invalidate_remove(dpp, name);

  ObjectCacheInfo info;
  int rc = distribute_cache(dpp, name, info, INVALIDATE_OBJ);
  if (rc < 0) {
    ldpp_dout(dpp, LOG_ERROR) <<__func__ << ": ERROR: failed to distribute cache: rc=" << rc << dendl;
  }

  ldpp_dout(dpp, 0) <<__func__ << ": Remove from cache: name=" << name << dendl;
  return 0;
}

int MotrMetaCache::distribute_cache(const DoutPrefixProvider *dpp,
                                    const string& normal_name,
                                    ObjectCacheInfo& obj_info, int op)
{
  return 0;
}

int MotrMetaCache::watch_cb(const DoutPrefixProvider *dpp,
                            uint64_t notify_id,
                            uint64_t cookie,
                            uint64_t notifier_id,
                            bufferlist& bl)
{
  return 0;
}

void MotrMetaCache::set_enabled(bool status)
{
  cache.set_enabled(status);
}

// TODO: properly handle the number of key/value pairs to get in
// one query. Now the POC simply tries to retrieve all `max` number of pairs
// with starting key `marker`.
int MotrUser::list_buckets(const DoutPrefixProvider *dpp, const string& marker,
    const string& end_marker, uint64_t max, bool need_stats,
    BucketList &buckets, optional_yield y)
{
  int rc;
  vector<string> keys(max);
  vector<bufferlist> vals(max);
  bool is_truncated = false;

  ldpp_dout(dpp, 20) <<__func__ << ": list_user_buckets: marker=" << marker
                    << " end_marker=" << end_marker
                    << " max=" << max << dendl;

  // Retrieve all `max` number of pairs.
  buckets.clear();
  string user_info_iname = "motr.rgw.user.info." + info.user_id.to_str();
  keys[0] = marker;
  rc = store->next_query_by_name(user_info_iname, keys, vals);
  if (rc < 0) {
    ldpp_dout(dpp, LOG_ERROR) << __func__ << ": ERROR: NEXT query failed, rc=" << rc << dendl;
    return rc;
  } else if (rc == 0) {
    ldpp_dout(dpp, 0) << __func__ << ": No buckets to list, rc=" << rc << dendl;
    return rc;
  }

  // Process the returned pairs to add into BucketList.
  uint64_t bcount = 0;
  for (const auto& bl: vals) {
    if (bl.length() == 0)
      break;

    RGWBucketEnt ent;
    auto iter = bl.cbegin();
    ent.decode(iter);

    std::time_t ctime = ceph::real_clock::to_time_t(ent.creation_time);
    ldpp_dout(dpp, 20) <<__func__ << "got creation time: " << std::put_time(std::localtime(&ctime), "%F %T") << dendl;

    if (!end_marker.empty() &&
         end_marker.compare(ent.bucket.marker) <= 0)
      break;

    buckets.add(std::make_unique<MotrBucket>(this->store, ent, this));
    bcount++;
  }
  if (bcount == max)
    is_truncated = true;
  buckets.set_truncated(is_truncated);

  return 0;
}

int MotrUser::create_bucket(const DoutPrefixProvider* dpp,
                            const rgw_bucket& b,
                            const std::string& zonegroup_id,
                            rgw_placement_rule& placement_rule,
                            std::string& swift_ver_location,
                            const RGWQuotaInfo* pquota_info,
                            const RGWAccessControlPolicy& policy,
                            Attrs& attrs,
                            RGWBucketInfo& info,
                            obj_version& ep_objv,
                            bool exclusive,
                            bool obj_lock_enabled,
                            bool* existed,
                            req_info& req_info,
                            std::unique_ptr<Bucket>* bucket_out,
                            optional_yield y)
{
  int ret;
  std::unique_ptr<Bucket> bucket;

  // Look up the bucket. Create it if it doesn't exist.
  ret = this->store->get_bucket(dpp, this, b, &bucket, y);
  if (ret < 0 && ret != -ENOENT)
    return ret;

  if (ret != -ENOENT) {
    *existed = true;
    // if (swift_ver_location.empty()) {
    //   swift_ver_location = bucket->get_info().swift_ver_location;
    // }
    // placement_rule.inherit_from(bucket->get_info().placement_rule);

    // TODO: ACL policy
    // // don't allow changes to the acl policy
    //RGWAccessControlPolicy old_policy(ctx());
    //int rc = rgw_op_get_bucket_policy_from_attr(
    //           dpp, this, u, bucket->get_attrs(), &old_policy, y);
    //if (rc >= 0 && old_policy != policy) {
    //    bucket_out->swap(bucket);
    //    return -EEXIST;
    //}
  } else {

    placement_rule.name = "default";
    placement_rule.storage_class = "STANDARD";
    bucket = std::make_unique<MotrBucket>(store, b, this);
    bucket->set_attrs(attrs);
    *existed = false;
  }

  if (!*existed){
    // TODO: how to handle zone and multi-site.
    info.placement_rule = placement_rule;
    info.bucket = b;
    info.owner = this->get_info().user_id;
    info.zonegroup = zonegroup_id;
    if (obj_lock_enabled)
      info.flags = BUCKET_VERSIONED | BUCKET_OBJ_LOCK_ENABLED;
    bucket->set_version(ep_objv);
    bucket->get_info() = info;

    // Create a new bucket: (1) Add a key/value pair in the
    // bucket instance index. (2) Create a new bucket index.
    MotrBucket* mbucket = static_cast<MotrBucket*>(bucket.get());
    // "put_info" accepts boolean value mentioning whether to create new or update existing.
    // "yield" is not a boolean flag hence explicitly passing true to create a new record.
    ret = mbucket->put_info(dpp, true, ceph::real_time())? :
          mbucket->create_bucket_index() ? :
          mbucket->create_multipart_indices();
    if (ret < 0) {
      ldpp_dout(dpp, LOG_ERROR) <<__func__ << ": ERROR: failed to create bucket indices!" << ret << dendl;
      return ret;
    }

    // Insert the bucket entry into the user info index.
    ret = mbucket->link_user(dpp, this, y);
    if (ret < 0) {
      ldpp_dout(dpp, LOG_ERROR) <<__func__ << ": ERROR: failed to add bucket entry!" << ret << dendl;
      return ret;
    }

    // Add bucket entry in user stats index table.
    std::string user_stats_iname = "motr.rgw.user.stats." + info.owner.to_str();
    bufferlist blst;
    rgw_bucket_dir_header bkt_header;
    bkt_header.encode(blst);
    std::string bkt_name = get_bucket_name(b.tenant, b.name);
    ret = store->do_idx_op_by_name(user_stats_iname,
                              M0_IC_PUT, bkt_name, blst);

    if (ret != 0) {
      ldpp_dout(dpp, LOG_ERROR) <<__func__ << ": ERROR: Failed to add the stats entry "
        << "for the bucket=" << bkt_name << ", ret=" << ret << dendl;
      return ret;
    }

    ldpp_dout(dpp, 20) <<__func__ << ": Added an empty stats entry for "
        << "the bucket=" << bkt_name << ", ret=" << ret << dendl;
  } else {
    return -EEXIST;
    // bucket->set_version(ep_objv);
    // bucket->get_info() = info;
  }

  bucket_out->swap(bucket);

  return ret;
}

int MotrUser::read_attrs(const DoutPrefixProvider* dpp, optional_yield y)
{
  int rc = 0;
  if (not attrs.empty())
    return rc;

  struct MotrUserInfo muinfo;
  bufferlist bl;
  if (store->get_user_cache()->get(dpp, info.user_id.to_str(), bl)) {
    // Cache miss
    rc = store->do_idx_op_by_name(RGW_MOTR_USERS_IDX_NAME,
                                      M0_IC_GET, info.user_id.to_str(), bl);
    ldpp_dout(dpp, 20) <<__func__ << ": do_idx_op_by_name, rc=" << rc << dendl;
    if (rc < 0)
      return rc;
    // Put into cache.
    store->get_user_cache()->put(dpp, info.user_id.to_str(), bl);
  }
  bufferlist& blr = bl;
  auto iter = blr.cbegin();
  muinfo.decode(iter);
  attrs = muinfo.attrs;
  ldpp_dout(dpp, 20) <<__func__ << ": user attributes fetched successfully." << dendl;

  return rc;
}

int MotrUser::read_stats(const DoutPrefixProvider *dpp,
    optional_yield y, RGWStorageStats* stats,
    ceph::real_time *last_stats_sync,
    ceph::real_time *last_stats_update)
{
  int rc, num_of_entries, max_entries = 100; // to fetch in chunks of 100
  vector<string> keys(max_entries);
  vector<bufferlist> vals(max_entries);
  std::string user_stats_iname = "motr.rgw.user.stats." + info.user_id.to_str();
  rgw_bucket_dir_header bkt_header;

  do {
    rc = store->next_query_by_name(user_stats_iname, keys, vals);
    if (rc < 0) {
      ldpp_dout(dpp, LOG_ERROR) <<__func__ << ": ERROR: failed to get the user stats info for user  = "
                        << info.user_id.to_str() << dendl;
      return rc;
    } else if (rc == 0) {
      ldpp_dout(dpp, 20) <<__func__ << ": No bucket to fetch the stats." << dendl;
      return rc;
    }
    num_of_entries = rc;

    for (int i = 0 ; i < num_of_entries; i++) {
      bufferlist::const_iterator bitr = vals[i].begin();
      bkt_header.decode(bitr);

      for (const auto& pair : bkt_header.stats) {
        const rgw_bucket_category_stats& header_stats = pair.second;
        stats->num_objects += header_stats.num_entries;
        stats->size += header_stats.total_size;
        stats->size_rounded += rgw_rounded_kb(header_stats.actual_size) * 1024;
      }
    }
    keys[0] = keys[num_of_entries-1]; // keys[0] will be used as a marker in next loop.
  } while(num_of_entries == max_entries);

  return 0;
}

/* stats - Not for first pass */
int MotrUser::read_stats_async(const DoutPrefixProvider *dpp, RGWGetUserStats_CB *cb)
{
  return 0;
}

int MotrUser::complete_flush_stats(const DoutPrefixProvider *dpp, optional_yield y)
{
  return 0;
}

int MotrUser::read_usage(const DoutPrefixProvider *dpp, uint64_t start_epoch, uint64_t end_epoch, uint32_t max_entries,
    bool *is_truncated, RGWUsageIter& usage_iter,
    map<rgw_user_bucket, rgw_usage_log_entry>& usage)
{
  return -ENOENT;
}

int MotrUser::trim_usage(const DoutPrefixProvider *dpp, uint64_t start_epoch, uint64_t end_epoch)
{
  return 0;
}

int MotrUser::load_user_from_idx(const DoutPrefixProvider *dpp,
                              MotrStore *store,
                              RGWUserInfo& info, map<string, bufferlist> *attrs,
                              RGWObjVersionTracker *objv_tr)
{
  struct MotrUserInfo muinfo;
  bufferlist bl;
  ldpp_dout(dpp, 20) <<__func__ << ": info.user_id.id=" << info.user_id.id << dendl;
  if (store->get_user_cache()->get(dpp, info.user_id.to_str(), bl)) {
    // Cache misses
    int rc = store->do_idx_op_by_name(RGW_MOTR_USERS_IDX_NAME,
                                      M0_IC_GET, info.user_id.to_str(), bl);
    ldpp_dout(dpp, 20) <<__func__ << ": do_idx_op_by_name(), rc=" << rc << dendl;
    if (rc < 0)
        return rc;

    // Put into cache.
    store->get_user_cache()->put(dpp, info.user_id.to_str(), bl);
  }

  bufferlist& blr = bl;
  auto iter = blr.cbegin();
  muinfo.decode(iter);
  info = muinfo.info;
  if (attrs)
    *attrs = muinfo.attrs;
  if (objv_tr) {
    objv_tr->read_version = muinfo.user_version;
    objv_tracker.read_version = objv_tr->read_version;
  }

  if (!info.access_keys.empty()) {
    for(auto key : info.access_keys) {
      access_key_tracker.insert(key.first);
    }
  }

  return 0;
}

int MotrUser::load_user(const DoutPrefixProvider *dpp,
                        optional_yield y)
{
  ldpp_dout(dpp, 20) <<__func__ << ": user_id=" << info.user_id.to_str() << dendl;
  return load_user_from_idx(dpp, store, info, &attrs, &objv_tracker);
}

int MotrUser::create_user_info_idx()
{
  string user_info_iname = "motr.rgw.user.info." + info.user_id.to_str();
  return store->create_motr_idx_by_name(user_info_iname);
}

int inline MotrUser::create_user_stats_idx()
{
  string user_stats_iname = "motr.rgw.user.stats." + info.user_id.to_str();
  return store->create_motr_idx_by_name(user_stats_iname);
}

int MotrUser::merge_and_store_attrs(const DoutPrefixProvider* dpp, Attrs& new_attrs, optional_yield y)
{
  for (auto& it : new_attrs)
    attrs[it.first] = it.second;

  return store_user(dpp, y, false);
}

int MotrUser::store_user(const DoutPrefixProvider* dpp,
                         optional_yield y, bool exclusive, RGWUserInfo* old_info)
{
  bufferlist bl;
  struct MotrUserInfo muinfo;
  RGWUserInfo orig_info;
  RGWObjVersionTracker objv_tr = {};
  obj_version& obj_ver = objv_tr.read_version;

  ldpp_dout(dpp, 20) <<__func__ << ": User=" << info.user_id.id << dendl;
  orig_info.user_id = info.user_id;
  // XXX: we open and close motr idx 2 times in this method:
  // 1) on load_user_from_idx() here and 2) on do_idx_op_by_name(PUT) below.
  // Maybe this can be optimised later somewhow.
  int rc = load_user_from_idx(dpp, store, orig_info, nullptr, &objv_tr);
  ldpp_dout(dpp, 10) <<__func__ << ": load_user_from_idx, rc=" << rc << dendl;

  // Check if the user already exists
  if (rc == 0 && obj_ver.ver > 0) {
    if (old_info)
      *old_info = orig_info;

    if (obj_ver.ver != objv_tracker.read_version.ver) {
      rc = -ECANCELED;
      ldpp_dout(dpp, LOG_ERROR) <<__func__ << ": ERROR: User Read version mismatch" << dendl;
      goto out;
    }

    if (exclusive)
      return rc;

    obj_ver.ver++;
  } else {
    obj_ver.ver = 1;
    obj_ver.tag = "UserTAG";
  }

  // Insert the user to user info index.
  muinfo.info = info;
  muinfo.attrs = attrs;
  muinfo.user_version = obj_ver;
  muinfo.encode(bl);
  rc = store->do_idx_op_by_name(RGW_MOTR_USERS_IDX_NAME,
                                M0_IC_PUT, info.user_id.to_str(), bl);
  ldpp_dout(dpp, 10) <<__func__ << ": store user to motr index: rc=" << rc << dendl;
  if (rc == 0) {
    objv_tracker.read_version = obj_ver;
    objv_tracker.write_version = obj_ver;
  }

  // Store access key in access key index
  if (!info.access_keys.empty()) {
    std::string access_key;
    std::string secret_key;
    std::map<std::string, RGWAccessKey>::const_iterator iter = info.access_keys.begin();
    const RGWAccessKey& k = iter->second;
    access_key = k.id;
    secret_key = k.key;
    MotrAccessKey MGWUserKeys(access_key, secret_key, info.user_id.to_str());
    store->store_access_key(dpp, y, MGWUserKeys);
    access_key_tracker.insert(access_key);
  }

  // Check if any key need to be deleted
  if (access_key_tracker.size() != info.access_keys.size()) {
    std::string key_for_deletion;
    for (auto key : access_key_tracker) {
      if (!info.get_key(key)) {
        key_for_deletion = key;
        ldpp_dout(dpp, 0) <<__func__ << ": deleting access key: " << key_for_deletion << dendl;
        store->delete_access_key(dpp, y, key_for_deletion);
        if (rc < 0) {
          ldpp_dout(dpp, 0) <<__func__ << ": unable to delete access key, rc=" << rc << dendl;
        }
      }
    }
    if(rc >= 0) {
      access_key_tracker.erase(key_for_deletion);
    }
  }

  if (!info.user_email.empty()) {
     MotrEmailInfo MGWEmailInfo(info.user_id.to_str(), info.user_email);
     store->store_email_info(dpp, y, MGWEmailInfo);
  }

  // Create user info index to store all buckets that are belong
  // to this bucket.
  rc = create_user_info_idx();
  if (rc < 0 && rc != -EEXIST) {
    ldpp_dout(dpp, 0) <<__func__ << ": failed to create user info index: rc=" << rc << dendl;
    goto out;
  }

  // Create user stats index to store stats for
  // all the buckets belonging to a user.
  rc = create_user_stats_idx();
  if (rc < 0 && rc != -EEXIST) {
    ldpp_dout(dpp, 0) << __func__ 
      << "Failed to create user stats index: rc=" << rc << dendl;
    goto out;
  }

  // Put the user info into cache.
  rc = store->get_user_cache()->put(dpp, info.user_id.to_str(), bl);

out:
  return rc;
}

int MotrUser::remove_user(const DoutPrefixProvider* dpp, optional_yield y)
{
  // Remove user info from cache
  // Delete access keys for user
  // Delete user info
  // Delete user from user index
  // Delete email for user - TODO
  bufferlist bl;
  int rc;
  // Remove the user info from cache.
  store->get_user_cache()->remove(dpp, info.user_id.to_str());

  // Delete all access key of user
  if (!info.access_keys.empty()) {
    for(auto acc_key = info.access_keys.begin(); acc_key != info.access_keys.end(); acc_key++) {
      auto access_key = acc_key->first;
      rc = store->delete_access_key(dpp, y, access_key);
      // TODO
      // Check error code for access_key does not exist
      // Continue to next step only if delete failed because key doesn't exists
      if (rc < 0) {
        ldpp_dout(dpp, 0) <<__func__ << ": unable to delete access key, rc=" << rc << dendl;
      }
    }
  }

  //Delete email id
  if (!info.user_email.empty()) {
    rc = store->do_idx_op_by_name(RGW_IAM_MOTR_EMAIL_KEY,
		             M0_IC_DEL, info.user_email, bl);
    if (rc < 0 && rc != -ENOENT) {
       ldpp_dout(dpp, LOG_ERROR) <<__func__ << ": ERROR: unable to delete email id " << rc << dendl;
    }
  }

  // Delete user info index
  string user_info_iname = "motr.rgw.user.info." + info.user_id.to_str();
  store->delete_motr_idx_by_name(user_info_iname);
  ldpp_dout(dpp, 10) <<__func__ << ": deleted user info index - " << user_info_iname << dendl;

  // Delete user stats index
  string user_stats_iname = "motr.rgw.user.stats." + info.user_id.to_str();
  store->delete_motr_idx_by_name(user_stats_iname);
  ldpp_dout(dpp, 10) << "Deleted user stats index - " << user_stats_iname << dendl;

  // Delete user from user index
  rc = store->do_idx_op_by_name(RGW_MOTR_USERS_IDX_NAME,
                           M0_IC_DEL, info.user_id.to_str(), bl);
  if (rc < 0){
    ldpp_dout(dpp, 0) <<__func__ << ": unable to delete user from user index " << rc << dendl;
    return rc;
  }

  // TODO
  // Delete email for user
  // rc = store->do_idx_op_by_name(RGW_IAM_MOTR_EMAIL_KEY,
  //                          M0_IC_DEL, info.user_email, bl);
  // if (rc < 0){
  //   ldpp_dout(dpp, 0) << "Unable to delete email for user" << rc << dendl;
  //   return rc;
  // }
  return 0;
}


int MotrBucket::remove_bucket(const DoutPrefixProvider *dpp, bool delete_children, bool forward_to_master, req_info* req_info, optional_yield y)
{
  int ret;
  string tenant_bkt_name = get_bucket_name(info.bucket.tenant, info.bucket.name);
  ldpp_dout(dpp, 20) <<__func__ << ": entry=" << tenant_bkt_name << dendl;

  // Refresh info
  ret = load_bucket(dpp, y);
  if (ret < 0) {
    ldpp_dout(dpp, LOG_ERROR) << __func__ << ": ERROR: load_bucket failed rc=" << ret << dendl;
    return ret;
  }

  ListParams params;
  params.list_versions = true;
  params.allow_unordered = true;

  ListResults results;

  // 1. Check if Bucket has objects.
  // If bucket contains objects and delete_children is true, delete all objects.
  // Else throw error that bucket is not empty.
  do {
    results.objs.clear();

    // Check if bucket has objects.
    ret = list(dpp, params, 1000, results, y);
    if (ret < 0) {
      return ret;
    }

    // If result contains entries, bucket is not empty.
    if (!results.objs.empty() && !delete_children) {
      ldpp_dout(dpp, LOG_ERROR) <<__func__ << ": ERROR: could not remove non-empty bucket " << info.bucket.name << dendl;
      return -ENOTEMPTY;
    }

    for (const auto& obj : results.objs) {
      rgw_obj_key key(obj.key);
      /* xxx dang */
      ret = rgw_remove_object(dpp, store, this, key);
      if (ret < 0 && ret != -ENOENT) {
        ldpp_dout(dpp, LOG_ERROR) <<__func__ << ": ERROR: rgw_remove_object failed rc=" << ret << dendl;
	      return ret;
      }
    }
  } while(results.is_truncated);

  // 2. Abort Mp uploads on the bucket.
  ret = abort_multiparts(dpp, store->ctx());
  if (ret < 0) {
    return ret;
  }

  // 3. Remove mp index??
  string iname = "motr.rgw.bucket." + tenant_bkt_name + ".multiparts.in-progress";
  ret = store->delete_motr_idx_by_name(iname);
  if (ret < 0) {
    ldpp_dout(dpp, LOG_ERROR) <<__func__ << ": ERROR: failed to remove multipart.in-progress index rc=" << ret << dendl;
    return ret;
  }
  iname = "motr.rgw.bucket." + tenant_bkt_name + ".multiparts";
  ret = store->delete_motr_idx_by_name(iname);
  if (ret < 0) {
    ldpp_dout(dpp, LOG_ERROR) <<__func__ << ": ERROR: failed to remove multipart index rc=" << ret << dendl;
    return ret;
  }

  // 4. Delete the bucket stats.
  bufferlist blst;
  std::string user_stats_iname = "motr.rgw.user.stats." + info.owner.to_str();

  ret = store->do_idx_op_by_name(user_stats_iname,
                            M0_IC_DEL, tenant_bkt_name, blst);

  if (ret != 0) {
    ldpp_dout(dpp, LOG_ERROR) <<__func__ << ": ERROR: Failed to delete the stats entry "
                      << "for the bucket=" << tenant_bkt_name
                      << ", ret=" << ret << dendl;
  }
  else
    ldpp_dout(dpp, 20) <<__func__ << ": Deleted the stats successfully for the "
                      << " bucket=" << tenant_bkt_name << dendl;

  // 5. Remove the bucket from user info index. (unlink user)
  ret = this->unlink_user(dpp, info.owner, y);
  if (ret < 0) {
    ldpp_dout(dpp, LOG_ERROR) <<__func__ << ": ERROR: unlink_user failed rc=" << ret << dendl;
    return ret;
  }

  // 6. Remove bucket index.
  string bucket_index_iname = "motr.rgw.bucket.index." + tenant_bkt_name;
  ret = store->delete_motr_idx_by_name(bucket_index_iname);
  if (ret < 0) {
    ldpp_dout(dpp, LOG_ERROR) <<__func__ << ": ERROR: unlink_user failed rc=" << ret << dendl;
    return ret;
  }

  // 7. Remove bucket instance info.
  bufferlist bl;
  ret = store->get_bucket_inst_cache()->remove(dpp, tenant_bkt_name);
  if (ret < 0) {
    ldpp_dout(dpp, LOG_ERROR) <<__func__ << ": ERROR: failed to remove bucket instance from cache rc="
      << ret << dendl;
    return ret;
  }

  ret = store->do_idx_op_by_name(RGW_MOTR_BUCKET_INST_IDX_NAME,
                                  M0_IC_DEL, tenant_bkt_name, bl);
  if (ret < 0) {
    ldpp_dout(dpp, LOG_ERROR) <<__func__ << ": ERROR: failed to remove bucket instance rc="
      << ret << dendl;
    return ret;
  }

  // TODO :
  // 8. Remove Notifications
  // if bucket has notification definitions associated with it
  // they should be removed (note that any pending notifications on the bucket are still going to be sent)

  // 9. Forward request to master.
  if (forward_to_master) {
    bufferlist in_data;
    ret = store->forward_request_to_master(dpp, owner, &bucket_version, in_data, nullptr, *req_info, y);
    if (ret < 0) {
      if (ret == -ENOENT) {
        /* adjust error, we want to return with NoSuchBucket and not
        * NoSuchKey */
        ret = -ERR_NO_SUCH_BUCKET;
      }
      ldpp_dout(dpp, LOG_ERROR) <<__func__ << ": ERROR: forward to master failed. ret=" << ret << dendl;
      return ret;
    }
  }

  ldpp_dout(dpp, 20) <<__func__ << ": exit=" << tenant_bkt_name << dendl;

  return ret;
}

int MotrBucket::remove_bucket_bypass_gc(int concurrent_max, bool
        keep_index_consistent,
        optional_yield y, const
        DoutPrefixProvider *dpp) {
  return 0;
}

int MotrBucket::put_info(const DoutPrefixProvider *dpp, bool exclusive, ceph::real_time _mtime)
{
  bufferlist bl;
  struct MotrBucketInfo mbinfo;
  string tenant_bkt_name = get_bucket_name(info.bucket.tenant, info.bucket.name);

  ldpp_dout(dpp, 20) <<__func__ << ": bucket_id=" << info.bucket.bucket_id << dendl;
  mbinfo.info = info;
  mbinfo.bucket_attrs = attrs;
  mbinfo.mtime = _mtime;
  mbinfo.bucket_version = bucket_version;
  mbinfo.encode(bl);

  // Insert bucket instance using bucket's marker (string).
  int rc = store->do_idx_op_by_name(RGW_MOTR_BUCKET_INST_IDX_NAME,
                                  M0_IC_PUT, tenant_bkt_name, bl, !exclusive);
  if (rc == 0)
    store->get_bucket_inst_cache()->put(dpp, tenant_bkt_name, bl);

  return rc;
}

int MotrBucket::load_bucket(const DoutPrefixProvider *dpp, optional_yield y, bool get_stats)
{
  // Get bucket instance using bucket's name (string). or bucket id?
  bufferlist bl;
  string tenant_bkt_name = get_bucket_name(info.bucket.tenant, info.bucket.name);
  if (store->get_bucket_inst_cache()->get(dpp, tenant_bkt_name, bl)) {
    // Cache misses.
    ldpp_dout(dpp, 20) <<__func__ << ": name=" << tenant_bkt_name << dendl;
    int rc = store->do_idx_op_by_name(RGW_MOTR_BUCKET_INST_IDX_NAME,
                                      M0_IC_GET, tenant_bkt_name, bl);
    ldpp_dout(dpp, 20) <<__func__ << ": do_idx_op_by_name, rc=" << rc << dendl;
    if (rc < 0)
      return rc;
    store->get_bucket_inst_cache()->put(dpp, tenant_bkt_name, bl);
  }

  struct MotrBucketInfo mbinfo;
  bufferlist& blr = bl;
  auto iter =blr.cbegin();
  mbinfo.decode(iter); //Decode into MotrBucketInfo.

  info = mbinfo.info;
  ldpp_dout(dpp, 20) <<__func__ << ": bucket_id=" << info.bucket.bucket_id << dendl;
  rgw_placement_rule placement_rule;
  placement_rule.name = "default";
  placement_rule.storage_class = "STANDARD";
  info.placement_rule = placement_rule;

  attrs = mbinfo.bucket_attrs;
  mtime = mbinfo.mtime;
  bucket_version = mbinfo.bucket_version;

  return 0;
}

int MotrBucket::link_user(const DoutPrefixProvider* dpp, User* new_user, optional_yield y)
{
  bufferlist bl;
  RGWBucketEnt new_bucket;
  ceph::real_time creation_time = get_creation_time();

  // RGWBucketEnt or cls_user_bucket_entry is the structure that is stored.
  new_bucket.bucket = info.bucket;
  new_bucket.size = 0;
  if (real_clock::is_zero(creation_time))
    creation_time = ceph::real_clock::now();
  new_bucket.creation_time = creation_time;
  new_bucket.encode(bl);
  std::time_t ctime = ceph::real_clock::to_time_t(new_bucket.creation_time);
  ldpp_dout(dpp, 20) <<__func__ << ": got creation time: "
                     << std::put_time(std::localtime(&ctime), "%F %T") << dendl;
  string tenant_bkt_name = get_bucket_name(info.bucket.tenant, info.bucket.name);

  // Insert the user into the user info index.
  string user_info_idx_name = "motr.rgw.user.info." + new_user->get_info().user_id.to_str();
  return store->do_idx_op_by_name(user_info_idx_name,
                                  M0_IC_PUT, tenant_bkt_name, bl);

}

int MotrBucket::unlink_user(const DoutPrefixProvider* dpp, const rgw_user &bucket_owner, optional_yield y)
{
  // Remove the user into the user info index.
  bufferlist bl;
  string tenant_bkt_name = get_bucket_name(info.bucket.tenant, info.bucket.name);
  string user_info_idx_name = "motr.rgw.user.info." + bucket_owner.to_str();
  return store->do_idx_op_by_name(user_info_idx_name,
                                  M0_IC_DEL, tenant_bkt_name, bl);
}

/* stats - Not for first pass */
int MotrBucket::read_stats(const DoutPrefixProvider *dpp, int shard_id,
    std::string *bucket_ver, std::string *master_ver,
    std::map<RGWObjCategory, RGWStorageStats>& stats,
    std::string *max_marker, bool *syncstopped)
{
  std::string user_stats_iname = "motr.rgw.user.stats." + info.owner.to_str();
  bufferlist bl;
  int rc = this->store->do_idx_op_by_name(user_stats_iname,
                                  M0_IC_GET, info.bucket.get_key(), bl);
  if (rc < 0) {
    ldpp_dout(dpp, LOG_ERROR) <<__func__ << ": ERROR: failed to get the bucket stats for bucket = "
                       << info.bucket.get_key() << dendl;
    return rc;
  }

  rgw_bucket_dir_header bkt_header;
  ceph::buffer::list::const_iterator bitr = bl.begin();
  bkt_header.decode(bitr);
  for(const auto& [category, bkt_stat]: bkt_header.stats) {
    RGWStorageStats& s = stats[category];
    s.num_objects = bkt_stat.num_entries;
    s.size = bkt_stat.total_size;
    s.size_rounded = bkt_stat.actual_size;
  }
  return 0;
}

int MotrBucket::create_bucket_index()
{
  string tenant_bkt_name = get_bucket_name(info.bucket.tenant, info.bucket.name);
  string bucket_index_iname = "motr.rgw.bucket.index." + tenant_bkt_name;
  return store->create_motr_idx_by_name(bucket_index_iname);
}

int MotrBucket::create_multipart_indices()
{
  int rc;
  string tenant_bkt_name = get_bucket_name(info.bucket.tenant, info.bucket.name);

  // There are two additional indexes per bucket for multiparts:
  // one for in-progress uploads, another one for completed uploads.

  // Key is the object name + upload_id, value is a rgw_bucket_dir_entry.
  // An entry is inserted when a multipart upload is initialised (
  // MotrMultipartUpload::init()) and will be removed when the upload
  // is completed (MotrMultipartUpload::complete()).
  // MotrBucket::list_multiparts() will scan this index to return all
  // in-progress multipart uploads in the bucket.
  string iname = "motr.rgw.bucket." + tenant_bkt_name + ".multiparts.in-progress";
  rc = store->create_motr_idx_by_name(iname);
  if (rc < 0) {
    ldout(store->cctx, LOG_ERROR) <<__func__
      << ": ERROR: failed to create bucket in-progress multiparts index "
      << iname << ", rc=" << rc << dendl;
    return rc;
  }

  iname = "motr.rgw.bucket." + tenant_bkt_name + ".multiparts";
  rc = store->create_motr_idx_by_name(iname);
  if (rc < 0) {
    ldout(store->cctx, LOG_ERROR) <<__func__
      << ": ERROR: failed to create bucket multiparts index "
      << iname << ", rc=" << rc << dendl;
    return rc;
  }

  return 0;
}


int MotrBucket::read_stats_async(const DoutPrefixProvider *dpp, int shard_id, RGWGetBucketStats_CB *ctx)
{
  return 0;
}

int MotrBucket::sync_user_stats(const DoutPrefixProvider *dpp, optional_yield y)
{
  return 0;
}

int MotrBucket::update_container_stats(const DoutPrefixProvider *dpp)
{
  return 0;
}

int MotrBucket::check_bucket_shards(const DoutPrefixProvider *dpp)
{
  return 0;
}

int MotrBucket::chown(const DoutPrefixProvider *dpp, User* new_user, User* old_user, optional_yield y, const std::string* marker)
{
  // TODO: update bucket with new owner

  /* XXX: Update policies of all the bucket->objects with new user */
  return 0;
}

/* Make sure to call load_bucket() if you need it first */
bool MotrBucket::is_owner(User* user)
{
  return (info.owner.compare(user->get_id()) == 0);
}

int MotrBucket::check_empty(const DoutPrefixProvider *dpp, optional_yield y)
{
  /* XXX: Check if bucket contains any objects */
  return 0;
}

int MotrBucket::check_quota(const DoutPrefixProvider *dpp,
    RGWQuotaInfo& user_quota, RGWQuotaInfo& bucket_quota,
    uint64_t obj_size, optional_yield y, bool check_size_only) {
  RGWQuotaHandler* quota_handler = \
    RGWQuotaHandler::generate_handler(dpp, store, false);

  ldpp_dout(dpp, 20) <<__func__ << ": called. check_size_only = "
     << check_size_only << ", obj_size=" << obj_size << dendl;

  int rc = quota_handler->check_quota(dpp, info.owner, info.bucket,
                                      user_quota, bucket_quota,
                                      check_size_only ? 0 : 1, obj_size, y);
  RGWQuotaHandler::free_handler(quota_handler);
  return rc;
}

int MotrBucket::merge_and_store_attrs(const DoutPrefixProvider *dpp, Attrs& new_attrs, optional_yield y)
{
  // Assign updated bucket attributes map to attrs map variable
  attrs = new_attrs;
  // "put_info" second bool argument is meant to update existing metadata,
  // which is not needed here. So explicitly passing false.
  return put_info(dpp, false, ceph::real_time());
}

int MotrBucket::try_refresh_info(const DoutPrefixProvider *dpp, ceph::real_time *pmtime)
{
  return 0;
}

/* XXX: usage and stats not supported in the first pass */
int MotrBucket::read_usage(const DoutPrefixProvider *dpp, uint64_t start_epoch, uint64_t end_epoch,
    uint32_t max_entries, bool *is_truncated,
    RGWUsageIter& usage_iter,
    map<rgw_user_bucket, rgw_usage_log_entry>& usage)
{
  return -ENOENT;
}

int MotrBucket::trim_usage(const DoutPrefixProvider *dpp, uint64_t start_epoch, uint64_t end_epoch)
{
  return 0;
}

int MotrBucket::remove_objs_from_index(const DoutPrefixProvider *dpp, std::list<rgw_obj_index_key>& objs_to_unlink)
{
  /* XXX: CHECK: Unlike RadosStore, there is no seperate bucket index table.
   * Delete all the object in the list from the object table of this
   * bucket
   */
  return 0;
}

int MotrBucket::check_index(const DoutPrefixProvider *dpp, std::map<RGWObjCategory, RGWStorageStats>& existing_stats, std::map<RGWObjCategory, RGWStorageStats>& calculated_stats)
{
  /* XXX: stats not supported yet */
  return 0;
}

int MotrBucket::rebuild_index(const DoutPrefixProvider *dpp)
{
  /* there is no index table in dbstore. Not applicable */
  return 0;
}

int MotrBucket::set_tag_timeout(const DoutPrefixProvider *dpp, uint64_t timeout)
{
  /* XXX: CHECK: set tag timeout for all the bucket objects? */
  return 0;
}

int MotrBucket::purge_instance(const DoutPrefixProvider *dpp)
{
  /* XXX: CHECK: for dbstore only single instance supported.
   * Remove all the objects for that instance? Anything extra needed?
   */
  return 0;
}

int MotrBucket::set_acl(const DoutPrefixProvider *dpp, RGWAccessControlPolicy &acl, optional_yield y)
{
  int ret = 0;
  bufferlist aclbl;

  acls = acl;
  acl.encode(aclbl);

  Attrs attrs = get_attrs();
  attrs[RGW_ATTR_ACL] = aclbl;

  // TODO: update bucket entry with the new attrs

  return ret;
}

std::unique_ptr<Object> MotrBucket::get_object(const rgw_obj_key& k)
{
  return std::make_unique<MotrObject>(this->store, k, this);
}

// List object versions in such a way that the null-version object is
// positioned in the right place among the other versions ordered by mtime.
// (AWS S3 spec says that the object versions should be ordered by mtime.)
//
// Note: all versioned objects have "key[instance]" format of the key in
// motr index, and the instance hash is generated reverse-ordered by mtime
// (see MotrObject::gen_rand_obj_instance_name()), so versions are ordered
// as needed just as we fetch from them motr, but null-version objects do
// not have any [instance] suffix key in their name, that's why we have to
// position it correctly among the other object versions, that's why the
// code is a bit tricky here.
//
// The basic algorithm is this: save null-version in null_ent and put it
// to the result only if the next version is older than it. If marker is
// provided (from which version to give the result), put null-version to
// the result only if it's older than the marker. If the number of objects
// is bigger than max and the result is_truncated, make sure we put the
// correct next_marker, which can be the null-version too. If the marker
// is the null-version, put it and only the older versions to the result.
int MotrBucket::list(const DoutPrefixProvider *dpp, ListParams& params, int max, ListResults& results, optional_yield y)
{
  int rc;
  if (max == 0)  // Return an emtpy response.
    return 0;

  string tenant_bkt_name = get_bucket_name(info.bucket.tenant, info.bucket.name);

  ldpp_dout(dpp, 20) <<__func__ << ": bucket=" << tenant_bkt_name
                    << " prefix=" << params.prefix
                    << " marker=" << params.marker
                    << " max=" << max << dendl;
  int batch_size = 100;
  vector<string> keys(batch_size);
  vector<bufferlist> vals(batch_size);
  rgw_bucket_dir_entry null_ent;

  // Retrieve all `max` number of pairs.
  string bucket_index_iname = "motr.rgw.bucket.index." + tenant_bkt_name;

  // Modify the marker based on its type
  keys[0] = params.prefix;
  if (!params.marker.empty()) {
    keys[0] = params.marker.name;
    // Get the position of delimiter string
    if (params.delim != "") {
      int delim_pos = keys[0].find(params.delim, params.prefix.length());
      // If delimiter is present at the very end, append "\xff" to skip all
      // the dir entries.
      if (delim_pos == (int)(keys[0].length() - params.delim.length()))
        keys[0].append("\xff");
    }
  }

  // Return an error in case of invalid version-id-marker
  bufferlist bl;
  std::string marker_key;
  ceph::real_time marker_mtime;

  if (params.marker.instance == "null")
    marker_key = params.marker.name + '\a';
  else
    marker_key = params.marker.name + '\a' + params.marker.instance;

  if (params.marker.instance != "") {
    rc = store->do_idx_op_by_name(bucket_index_iname, M0_IC_GET, marker_key, bl);
    if (rc < 0) {
      ldpp_dout(dpp, LOG_ERROR) <<__func__ << ": ERROR: invalid version-id-marker, rc=" << rc << dendl;
      return -EINVAL;
    }
  }

  results.is_truncated = false;
  int keycount=0; // how many keys we've put to the results so far
  std::string next_key;
  while (keycount <= max) {
    if (!next_key.empty())
      keys[0] = next_key;
    rc = store->next_query_by_name(bucket_index_iname, keys, vals, params.prefix,
                                   params.delim);
    if (rc < 0) {
      ldpp_dout(dpp, LOG_ERROR) <<__func__ << ": ERROR: next_query_by_name failed, rc=" << rc << dendl;
      return rc;
    }
    ldpp_dout(dpp, 20) <<__func__ << ": items: " << rc << dendl;
    // Process the returned pairs to add into ListResults.
    for (int i = 0; i < rc; ++i) {
      ldpp_dout(dpp, 70) <<__func__ << ": key["<<i<<"] :"<< keys[i] <<dendl;
      if (i == 0 && !next_key.empty()) {
        ldpp_dout(dpp, 70) <<__func__ << ": skipping previous next_key: " << next_key << dendl;
        continue;
      }
      if (vals[i].length() == 0) {
        results.common_prefixes[keys[i]] = true;
      } else {
        rgw_bucket_dir_entry ent;
        auto iter = vals[i].cbegin();
        ent.decode(iter);
        rgw_obj_key key(ent.key);
        if (params.list_versions || ent.is_visible()) {
          if (key.name == params.marker.name) {
            // skip the object for non-versioned bucket
            if (!(ent.flags & rgw_bucket_dir_entry::FLAG_VER)) 
              continue;
            // filter out versions which go before marker.instance
            if (params.marker.instance != "") {
              // Check if params.marker.instance is null
              if (params.marker.instance == "null") {  
                  if ((!null_ent.key.empty() && null_ent.meta.mtime < ent.meta.mtime))
                    continue;               
              } else {
                if (key.instance != "" && key.instance < params.marker.instance) {
                  if(null_ent.meta.mtime >= ent.meta.mtime)
                    marker_mtime = null_ent.meta.mtime;
                  continue;
                }
              }
            }
          }
check_keycount:
          if (keycount >= max) {
            if (!null_ent.key.empty() &&
                (null_ent.key.name != ent.key.name ||
                 null_ent.meta.mtime > ent.meta.mtime))
              results.next_marker = rgw_obj_key(key.name, "null");
            else
              results.next_marker = rgw_obj_key(key.name, key.instance);
            results.is_truncated = true;
            break;
          }
          // Put null-entry ordered by mtime.
          // Note: this depends on object-versions being ordered,
          // see MotrObject::gen_rand_obj_instance_name().
          if (!null_ent.key.empty() &&
              (null_ent.key.name != ent.key.name ||
               null_ent.meta.mtime > ent.meta.mtime)) {
            if (params.marker.instance != "" &&
                key.instance == params.marker.instance)
              null_ent.key = {}; // filtered out by the marker
            else {
              if (null_ent.meta.mtime != marker_mtime) {
                results.objs.emplace_back(std::move(null_ent));
                keycount++;
                goto check_keycount;
              }
            }
          }
          if (key.instance == "")
            null_ent = std::move(ent);
          else {
            results.objs.emplace_back(std::move(ent));
            keycount++;
          }
        }
      }
    }

    if (rc == 0 || rc < batch_size || results.is_truncated)
      break;

    next_key = keys[rc-1]; // next marker key
    keys.clear();
    vals.clear();
    keys.resize(batch_size);
    vals.resize(batch_size);
  }

  if (!null_ent.key.empty() && !results.is_truncated) {
    if (keycount < max) {
      if (null_ent.meta.mtime != marker_mtime)
          results.objs.emplace_back(std::move(null_ent));
    }
    else { // there was no more records in the bucket
      results.next_marker = rgw_obj_key(null_ent.key.name, "null");
      results.is_truncated = true;
    }
  }

  return 0;
}

int MotrBucket::list_multiparts(const DoutPrefixProvider *dpp,
      const string& prefix,
      string& marker,
      const string& delim,
      const int& max_uploads,
      vector<std::unique_ptr<MultipartUpload>>& uploads,
      map<string, bool> *common_prefixes,
      bool *is_truncated)
{
  int rc = 0;
  if( max_uploads <= 0 )
	return rc;
  int upl = max_uploads;
  if( marker != "" )
	upl++;
  vector<string> key_vec(upl);
  vector<bufferlist> val_vec(upl);
  string tenant_bkt_name = get_bucket_name(this->get_tenant(), this->get_name());

  string bucket_multipart_iname =
      "motr.rgw.bucket." + tenant_bkt_name + ".multiparts.in-progress";
  key_vec[0].clear();
  key_vec[0].assign(marker.begin(), marker.end());
  rc = store->next_query_by_name(bucket_multipart_iname, key_vec, val_vec, prefix, delim);
  if (rc < 0) {
    ldpp_dout(dpp, LOG_ERROR) <<__func__ << ": ERROR: next_query_by_name failed, rc=" << rc << dendl;
    return rc;
  }

  // Process the returned pairs to add into ListResults.
  // The POC can only support listing all objects or selecting
  // with prefix.
  int ocount = 0;
  rgw_obj_key last_obj_key;
  *is_truncated = false;

  for (const auto& bl: val_vec) {

    if (bl.length() == 0)
      continue;

    if((marker != "") && (ocount == 0)) {
      ocount++;
      continue;
    }
    rgw_bucket_dir_entry ent;
    auto iter = bl.cbegin();
    ent.decode(iter);

    rgw_obj_key key(ent.key);
    if (prefix.size() &&
        (0 != key.name.compare(0, prefix.size(), prefix))) {
      ldpp_dout(dpp, 20) <<__func__ <<
        ": skippping \"" << key <<
        "\" because doesn't match prefix" << dendl;
      continue;
    }

    uploads.push_back(this->get_multipart_upload(key.name));
    last_obj_key = key;
    ocount++;
    if (ocount == upl) {
      *is_truncated = true;
      break;
    }
  }
  marker = last_obj_key.name;

  // What is common prefix? We don't handle it for now.

  return 0;

}

int MotrBucket::abort_multiparts(const DoutPrefixProvider *dpp, CephContext *cct)
{
  return 0;
}

void MotrStore::finalize(void) {
  // stop gc worker threads
  stop_gc();
  // close connection with motr
  m0_client_fini(this->instance, true);
}

MotrStore& MotrStore::set_run_gc_thread(bool _use_gc_threads) {
  use_gc_threads = _use_gc_threads;
  return *this;
}

MotrStore& MotrStore::set_use_cache(bool _use_cache) {
  use_cache = _use_cache;
  return *this;
}

int MotrStore::initialize(CephContext *cct, const DoutPrefixProvider *dpp) {
  // Create metadata objects and set enabled=use_cache value
  int rc = init_metadata_cache(dpp, cct);
  if (rc != 0) {
    ldpp_dout(dpp, LOG_ERROR) << __func__  << ": ERROR: Metadata cache init failed " <<
      "with rc = " << rc << dendl;
    return rc;
  }

  if (use_gc_threads) {
    // Create MotrGC object and start GCWorker threads
    int rc = create_gc();
    if (rc != 0)
      ldpp_dout(dpp, LOG_ERROR) << __func__  << ": ERROR: Failed to Create MotrGC " <<
        "with rc = " << rc << dendl;
  }
  return rc;
}

int MotrStore::create_gc() {
  int ret = 0;
  motr_gc = std::make_unique<MotrGC>(cctx, this);
  ret = motr_gc->initialize();
  if (ret < 0) {
    // Failed to initialize MotrGC
    return ret;
  }
  motr_gc->start_processor();
  return ret;
}

void MotrStore::stop_gc() {
  if (motr_gc) {
    motr_gc->stop_processor();
    motr_gc->finalize();
  }
}

uint64_t MotrStore::get_new_req_id()
{
  uint64_t req_id = ceph::util::generate_random_number<uint64_t>();

  addb_logger.set_id(req_id);
  ADDB(RGW_ADDB_REQUEST_ID, addb_logger.get_id(),
       RGW_ADDB_FUNC_GET_NEW_REQ_ID, RGW_ADDB_PHASE_START);

  return req_id;
}

const RGWZoneGroup& MotrZone::get_zonegroup()
{
  return *zonegroup;
}

int MotrZone::get_zonegroup(const std::string& id, RGWZoneGroup& zg)
{
  /* XXX: for now only one zonegroup supported */
  zg = *zonegroup;
  return 0;
}

const RGWZoneParams& MotrZone::get_params()
{
  return *zone_params;
}

const rgw_zone_id& MotrZone::get_id()
{
  return cur_zone_id;
}

const RGWRealm& MotrZone::get_realm()
{
  return *realm;
}

const std::string& MotrZone::get_name() const
{
  return zone_params->get_name();
}

bool MotrZone::is_writeable()
{
  return true;
}

bool MotrZone::get_redirect_endpoint(std::string* endpoint)
{
  return false;
}

bool MotrZone::has_zonegroup_api(const std::string& api) const
{
  return (zonegroup->api_name == api);
}

const std::string& MotrZone::get_current_period_id()
{
  return current_period->get_id();
}

std::unique_ptr<LuaScriptManager> MotrStore::get_lua_script_manager()
{
  return std::make_unique<MotrLuaScriptManager>(this);
}

int MotrObject::get_obj_state(const DoutPrefixProvider* dpp, RGWObjectCtx* rctx, RGWObjState **_state, optional_yield y, bool follow_olh)
{
  if (state == nullptr)
    state = new RGWObjState();
  *_state = state;
  req_state* s = static_cast<req_state*>(rctx->get_private());
  // Get object's metadata (those stored in rgw_bucket_dir_entry).
  rgw_bucket_dir_entry ent;
  int rc = this->get_bucket_dir_ent(dpp, ent);
  if (rc < 0) {
    if(rc == -ENOENT) {
        s->err.message = "The specified key does not exist.";
    }
    return rc;
  }

  // Set object's type.
  this->category = ent.meta.category;

  // Set object state.
  state->obj = get_obj();
  state->exists = true;
  state->size = ent.meta.size;
  state->accounted_size = ent.meta.size;
  state->mtime = ent.meta.mtime;

  state->has_attrs = true;
  bufferlist etag_bl;
  string& etag = ent.meta.etag;
  ldpp_dout(dpp, 20) <<__func__ << ": object's etag:  " << ent.meta.etag << dendl;
  etag_bl.append(etag);
  state->attrset[RGW_ATTR_ETAG] = etag_bl;

  return 0;
}

MotrObject::~MotrObject() {
  delete state;
  this->close_mobj();
}

//  int MotrObject::read_attrs(const DoutPrefixProvider* dpp, Motr::Object::Read &read_op, optional_yield y, rgw_obj* target_obj)
//  {
//    read_op.params.attrs = &attrs;
//    read_op.params.target_obj = target_obj;
//    read_op.params.obj_size = &obj_size;
//    read_op.params.lastmod = &mtime;
//
//    return read_op.prepare(dpp);
//  }

int MotrObject::fetch_obj_entry_and_key(const DoutPrefixProvider* dpp, rgw_bucket_dir_entry& ent, std::string& bname, std::string& key, rgw_obj* target_obj)
{
  int rc = this->get_bucket_dir_ent(dpp, ent);

  if (rc < 0) {
      ldpp_dout(dpp, LOG_ERROR) <<__func__ << ": ERROR: failed to get object entry. rc=" << rc << dendl;
      return rc;
  }
  if (ent.is_delete_marker()) {
    ldpp_dout(dpp, LOG_ERROR) <<__func__ << ": ERROR: delete marker is not an object." << dendl;
    return -ENOENT;
  }

  if (target_obj)
    bname = get_bucket_name(target_obj->bucket.tenant, target_obj->bucket.name);
  else
    bname = get_bucket_name(this->get_bucket()->get_tenant(), this->get_bucket()->get_name());

  rgw_obj_key objkey(ent.key);

  // Remove the "null" from instance to avoid "VersionId" field in the response
  // and overwrite the existing null object entry.
  if (ent.key.instance == "null") {
    ent.key.instance = "";
    key = objkey.name + '\a';
  }
  else
    key = objkey.name + '\a' + objkey.instance;

  ldpp_dout(dpp, 20) <<__func__ << ": bucket=" << bname << " key=" << key << dendl;

  return 0;
}

int MotrObject::set_obj_attrs(const DoutPrefixProvider* dpp, RGWObjectCtx* rctx, Attrs* setattrs, Attrs* delattrs, optional_yield y, rgw_obj* target_obj)
{

  rgw_bucket_dir_entry ent;
  string bname, key;
  int rc;

  rc = fetch_obj_entry_and_key(dpp, ent, bname, key, target_obj);
  if (rc < 0) {
    ldpp_dout(dpp, LOG_ERROR) <<__func__ << ": ERROR: Failed to get key or object's entry from bucket index. rc=" << rc << dendl;
    return rc;
  }
  // set attributes present in setattrs
  if (setattrs != nullptr) {
    for (auto& it : *setattrs) {
      attrs[it.first]=it.second;
      ldpp_dout(dpp, LOG_INFO) <<__func__ << ": INFO: adding "<< it.first << " to attribute list." << dendl;
    }
  }

  // delete attributes present in delattrs
  if (delattrs != nullptr) {
    for (auto& it: *delattrs) {
      auto del_it = attrs.find(it.first);
        if (del_it != attrs.end()) {
            ldpp_dout(dpp, LOG_INFO) <<__func__ << ": INFO: removing "<< it.first << " from attribute list." << dendl;

          attrs.erase(del_it);
        }
    }
  }
  bufferlist update_bl;
  string bucket_index_iname = "motr.rgw.bucket.index." + bname;

  ent.encode(update_bl);
  encode(attrs, update_bl);
  meta.encode(update_bl);

  rc = this->store->do_idx_op_by_name(bucket_index_iname, M0_IC_PUT, key, update_bl);
  if (rc < 0) {
    ldpp_dout(dpp, LOG_ERROR) <<__func__ << ": ERROR: Failed to put object's entry to bucket index. rc=" << rc << dendl;
    return rc;
  }
  // Put into cache.
  this->store->get_obj_meta_cache()->put(dpp, key, update_bl);

  return 0;
}

int MotrObject::get_obj_attrs(RGWObjectCtx* rctx, optional_yield y, const DoutPrefixProvider* dpp, rgw_obj* target_obj)
{
  req_state *s = (req_state *) rctx->get_private();
  // if ::get_obj_attrs() is called from radosgw-admin, s will be nullptr.
  if (s != nullptr) {
     string req_method = s->info.method;
    /* TODO: Temp fix: Enabled Multipart-GET Obj. and disabled other multipart request methods */
    if (this->category == RGWObjCategory::MultiMeta && (req_method == "POST" || req_method == "PUT"))
     return 0;
  }

  int rc;
  rgw_bucket_dir_entry ent;
  string bname, key;
  rc = fetch_obj_entry_and_key(dpp, ent, bname, key, target_obj);
  if (rc < 0) {
      ldpp_dout(dpp, LOG_ERROR) <<__func__ << ": ERROR: Failed to get key or object's entry from bucket index. rc=" << rc << dendl;
      return rc;
  }

  // get_obj_attrs() is called when radosgw-admin executes "object stat" cmd.
  bufferlist obj_fid_bl;
  std::string obj_fid_str = this->get_obj_fid_str();
  obj_fid_bl.append(obj_fid_str.c_str(), obj_fid_str.size());
  attrs.emplace(std::move(RGW_ATTR_META_PREFIX "motr-obj-fid"), std::move(obj_fid_bl));

  return 0;
}

int MotrObject::modify_obj_attrs(RGWObjectCtx* rctx, const char* attr_name, bufferlist& attr_val, optional_yield y, const DoutPrefixProvider* dpp)
{
  rgw_obj target = get_obj();
  sal::Attrs set_attrs;

  set_atomic(rctx);
  set_attrs[attr_name] = attr_val;
  return set_obj_attrs(dpp, rctx, &set_attrs, nullptr, y, &target);
}

int MotrObject::delete_obj_attrs(const DoutPrefixProvider* dpp, RGWObjectCtx* rctx, const char* attr_name, optional_yield y)
{
  rgw_obj target = get_obj();
  Attrs rm_attr;
  bufferlist bl;

  set_atomic(rctx);
  rm_attr[attr_name] = bl;
  return set_obj_attrs(dpp, rctx, nullptr, &rm_attr, y, &target);
}

/* RGWObjectCtx will be moved out of sal */
/* XXX: Placeholder. Should not be needed later after Dan's patch */
void MotrObject::set_atomic(RGWObjectCtx* rctx) const
{
  return;
}

/* RGWObjectCtx will be moved out of sal */
/* XXX: Placeholder. Should not be needed later after Dan's patch */
void MotrObject::set_prefetch_data(RGWObjectCtx* rctx)
{
  return;
}

/* RGWObjectCtx will be moved out of sal */
/* XXX: Placeholder. Should not be needed later after Dan's patch */
void MotrObject::set_compressed(RGWObjectCtx* rctx)
{
  return;
}

bool MotrObject::is_expired() {
  return false;
}

// Taken from rgw_rados.cc
void MotrObject::gen_rand_obj_instance_name()
{
  // Creating version-id based on timestamp value
  // to list/store object versions in lexicographically sorted order.
  char buf[UUID_LEN + 1];
  std::string version_id;
  // As the version ID timestamp is encoded in Base62, the maximum value
  // for 8-characters is 62^8 - 1. This is the maximum time interval in ms.
  constexpr uint64_t max_ts_count = 218340105584895;
  using UnsignedMillis = std::chrono::duration<uint64_t, std::milli>;
  const auto ms_since_epoch = std::chrono::time_point_cast<UnsignedMillis>(
                              std::chrono::system_clock::now()).time_since_epoch().count();
  uint64_t cur_time = max_ts_count - ms_since_epoch;
  auto version_ts = base62_encode(cur_time, TS_LEN);
  gen_rand_alphanumeric_no_underscore(store->ctx(), buf, UUID_LEN+1);
  version_id = version_ts + buf;
  key.set_instance(version_id);
}

int MotrObject::omap_get_vals(const DoutPrefixProvider *dpp, const std::string& marker, uint64_t count,
    std::map<std::string, bufferlist> *m,
    bool* pmore, optional_yield y)
{
  return 0;
}

int MotrObject::omap_get_all(const DoutPrefixProvider *dpp, std::map<std::string, bufferlist> *m,
    optional_yield y)
{
  return 0;
}

int MotrObject::omap_get_vals_by_keys(const DoutPrefixProvider *dpp, const std::string& oid,
    const std::set<std::string>& keys,
    Attrs* vals)
{
  return 0;
}

int MotrObject::omap_set_val_by_key(const DoutPrefixProvider *dpp, const std::string& key, bufferlist& val,
    bool must_exist, optional_yield y)
{
  return 0;
}

MPSerializer* MotrObject::get_serializer(const DoutPrefixProvider *dpp, const std::string& lock_name)
{
  return new MPMotrSerializer(dpp, store, this, lock_name);
}

int MotrObject::transition(RGWObjectCtx& rctx,
    Bucket* bucket,
    const rgw_placement_rule& placement_rule,
    const real_time& mtime,
    uint64_t olh_epoch,
    const DoutPrefixProvider* dpp,
    optional_yield y)
{
  return 0;
}

bool MotrObject::placement_rules_match(rgw_placement_rule& r1, rgw_placement_rule& r2)
{
  /* XXX: support single default zone and zonegroup for now */
  return true;
}

int MotrObject::dump_obj_layout(const DoutPrefixProvider *dpp, optional_yield y, Formatter* f, RGWObjectCtx* obj_ctx)
{
  return 0;
}

std::unique_ptr<Object::ReadOp> MotrObject::get_read_op(RGWObjectCtx* ctx)
{
  return std::make_unique<MotrObject::MotrReadOp>(this, ctx);
}

MotrObject::MotrReadOp::MotrReadOp(MotrObject *_source, RGWObjectCtx *_rctx) :
  source(_source),
  rctx(_rctx)
{
  struct req_state* s = static_cast<req_state*>(_rctx->get_private());
  ADDB(RGW_ADDB_REQUEST_OPCODE_ID, addb_logger.get_id(), s->op_type);
}

int MotrObject::MotrReadOp::prepare(optional_yield y, const DoutPrefixProvider* dpp)
{
  int rc;
  ldpp_dout(dpp, 20) <<__func__ << ": bucket=" << source->get_bucket()->get_name() << dendl;

  rgw_bucket_dir_entry ent;
  rc = source->get_bucket_dir_ent(dpp, ent);
  if (rc < 0)
    return rc;

  req_state* s = static_cast<req_state*>(rctx->get_private());

  // In GET/HEAD object API, return "MethodNotAllowed"
  // if delete-marker is the latest object entry
  // Else, return "NoSuchKey" error
  if (ent.is_delete_marker()) {
    if (source->get_instance() == ent.key.instance && ent.key.instance != "") {
      ldpp_dout(dpp, LOG_DEBUG) <<__func__ << ": DEBUG: The GET/HEAD object with version-id of "
                                            "delete-marker is not allowed." << dendl;
      s->err.message = "The specified method is not allowed against this resource.";
      return -ERR_METHOD_NOT_ALLOWED;
    }
    return -ENOENT;
  }

  // Set source object's attrs. The attrs is key/value map and is used
  // in send_response_data() to set attributes, including etag.
  bufferlist etag_bl;
  string& etag = ent.meta.etag;
  ldpp_dout(dpp, 20) <<__func__ << ": object's etag: " << ent.meta.etag << dendl;
  etag_bl.append(etag.c_str(), etag.size());
  source->get_attrs().emplace(std::move(RGW_ATTR_ETAG), std::move(etag_bl));
  source->set_key(ent.key);
  source->set_obj_size(ent.meta.size);
  source->category = ent.meta.category;

  // ReadOp::prepare() is called when processing OBJECT GET or INFO
  // request from s3 client, adding the object id to attrs will
  // expose internal details to client user, will this be a security
  // concern? If a user can access an object and its metadata, it
  // must have permission to do so, it should be O.K?
  bufferlist obj_fid_bl;
  std::string obj_fid_str = source->get_obj_fid_str();
  obj_fid_bl.append(obj_fid_str.c_str(), obj_fid_str.size());
  source->get_attrs().emplace(std::move(RGW_ATTR_META_PREFIX "motr-obj-fid"), std::move(obj_fid_bl));

  *params.lastmod = ent.meta.mtime;
  if (params.mod_ptr || params.unmod_ptr) {
    // Convert all times go GMT to make them compatible
    obj_time_weight src_weight;
    src_weight.init(*params.lastmod, params.mod_zone_id, params.mod_pg_ver);
    src_weight.high_precision = params.high_precision_time;

    obj_time_weight dest_weight;
    dest_weight.high_precision = params.high_precision_time;

    // Check if-modified-since condition
    if (params.mod_ptr && !params.if_nomatch) {
      dest_weight.init(*params.mod_ptr, params.mod_zone_id, params.mod_pg_ver);
      ldpp_dout(dpp, 10) <<__func__ << ": If-Modified-Since: " << dest_weight << " & "
                         << "Last-Modified: " << src_weight << dendl;
      if (!(dest_weight < src_weight)) {
        s->err.message = "At least one of the pre-conditions you specified did not hold ";
        return -ERR_PRECONDITION_FAILED;
      }
    }

    // Check if-unmodified-since condition
    if (params.unmod_ptr && !params.if_match) {
      dest_weight.init(*params.unmod_ptr, params.mod_zone_id, params.mod_pg_ver);
      ldpp_dout(dpp, 10) <<__func__ << ": If-UnModified-Since: " << dest_weight << " & "
                         << "Last-Modified: " << src_weight << dendl;
      if (dest_weight < src_weight) {
        s->err.message = "At least one of the pre-conditions you specified did not hold ";
        return -ERR_PRECONDITION_FAILED;
      }
    }
  }
  // Check if-match condition
  if (params.if_match) {
    string if_match_str = rgw_string_unquote(params.if_match);
    ldpp_dout(dpp, 10) <<__func__ << ": ETag: " << etag << " & "
                       << "If-Match: " << if_match_str << dendl;
    if (if_match_str.compare(etag) != 0) {
     s->err.message = "At least one of the pre-conditions you specified did not hold ";
      return -ERR_PRECONDITION_FAILED;
    }
  }
  // Check if-none-match condition
  if (params.if_nomatch) {
    string if_nomatch_str = rgw_string_unquote(params.if_nomatch);
    ldpp_dout(dpp, 10) <<__func__ << ": ETag: " << etag << " & "
                       << "If-NoMatch: " << if_nomatch_str << dendl;
    if (if_nomatch_str.compare(etag) == 0) {
      s->err.message = "At least one of the pre-conditions you specified did not hold ";
      return -ERR_PRECONDITION_FAILED;
    }
  }
  return 0;
}

int MotrObject::MotrReadOp::read(int64_t off, int64_t end, bufferlist& bl, optional_yield y, const DoutPrefixProvider* dpp)
{
  ldpp_dout(dpp, 20) << __func__ << ": sync read." << dendl;
  return 0;
}

// RGWGetObj::execute() calls ReadOp::iterate() to read object from 'off' to 'end'.
// The returned data is processed in 'cb' which is a chain of post-processing
// filters such as decompression, de-encryption and sending back data to client
// (RGWGetObj_CB::handle_dta which in turn calls RGWGetObj::get_data_cb() to
// send data back.).
//
// POC implements a simple sync version of iterate() function in which it reads
// a block of data each time and call 'cb' for post-processing.
int MotrObject::MotrReadOp::iterate(const DoutPrefixProvider* dpp, int64_t off, int64_t end, RGWGetDataCB* cb, optional_yield y)
{
  int rc;

  addb_logger.set_id(rctx);

  // Note that a composite object can be read just like a 
  // ordinary object.
  if (source->category == RGWObjCategory::MultiMeta &&
      !source->meta.is_composite) {
    ldpp_dout(dpp, 20) <<__func__ << ": open obj parts..." << dendl;
    rc = source->get_part_objs(dpp, this->part_objs)? :
         source->open_part_objs(dpp, this->part_objs);
    if (rc < 0) {
      ldpp_dout(dpp, LOG_ERROR) <<__func__ << ": ERROR: failed to open motr object: rc=" << rc << dendl;
      return rc;
    }
    rc = source->read_multipart_obj(dpp, off, end, cb, part_objs);
  }
  else {
    ldpp_dout(dpp, 20) <<__func__ << ": open object..." << dendl;
    rc = source->open_mobj(dpp);
    if (rc < 0) {
      ldpp_dout(dpp, LOG_ERROR) <<__func__ << ": ERROR: failed to open motr object: rc=" << rc << dendl;
      return rc;
    }
    rc = source->read_mobj(dpp, off, end, cb);
  }
  return rc;
}

int MotrObject::MotrReadOp::get_attr(const DoutPrefixProvider* dpp, const char* name, bufferlist& dest, optional_yield y)
{
  if (source == nullptr)
    return -ENODATA;
  rgw::sal::Attrs &attrs = source->get_attrs();
  auto iter = attrs.find(name);
  if (iter != attrs.end()) {
    dest = iter->second;
    return 0;
  }
  return -ENODATA;
}

std::unique_ptr<Object::DeleteOp> MotrObject::get_delete_op(RGWObjectCtx* ctx)
{
  return std::make_unique<MotrObject::MotrDeleteOp>(this, ctx);
}

MotrObject::MotrDeleteOp::MotrDeleteOp(MotrObject *_source, RGWObjectCtx *_rctx) :
  source(_source),
  rctx(_rctx)
{
  // - In case of the operation remove_user with --purge-data, we don't 
  //   have access to the `req_state* s` via `RGWObjectCtx* rctx`.
  // - In this case, we are generating a new req_id per obj deletion operation.
  //   This will retrict us from traking all delete req per user_remove req in ADDB
  //   untill we make changes to access req_state without using RGWObjectCtx ptr.

  if (rctx->get_private()) {
    addb_logger.set_id(rctx);
  } else {
    addb_logger.set_id(_source->store->get_new_req_id());
  }
}

// Implementation of DELETE OBJ also requires MotrObject::get_obj_state()
// to retrieve and set object's state from object's metadata.
int MotrObject::MotrDeleteOp::delete_obj(const DoutPrefixProvider* dpp, optional_yield y)
{
  int rc;
  bufferlist bl;
  string tenant_bkt_name = get_bucket_name(source->get_bucket()->get_tenant(), source->get_bucket()->get_name());
  string bucket_index_iname = "motr.rgw.bucket.index." + tenant_bkt_name;
  rgw_bucket_dir_entry ent;
  RGWBucketInfo &info = source->get_bucket()->get_info();

  rc = source->get_bucket_dir_ent(dpp, ent);
  if (rc < 0) {
    ldpp_dout(dpp, LOG_ERROR) <<__func__ << ": ERROR: Failed to get object's entry from bucket index. rc="<< rc << dendl;
    return rc;
  }

  string delete_key = source->get_key_str();

  //TODO: When integrating with background GC for object deletion,
  // we should consider adding object entry to GC before deleting the metadata.
  // Delete from the cache first.
  source->store->get_obj_meta_cache()->remove(dpp, delete_key);
  ldpp_dout(dpp, 20) << __func__  << ": Deleting key " << delete_key << " from "
                            << tenant_bkt_name << dendl;
  // Remove the motr object.
  // versioning enabled and suspended case.
  if (info.versioned()) {
    if (source->have_instance()) {
      // delete object permanently.
      result.version_id = ent.key.instance;
      if (ent.is_delete_marker())
        result.delete_marker = true;

      rc = source->remove_mobj_and_index_entry(
          dpp, ent, delete_key, bucket_index_iname, tenant_bkt_name);
      if (rc < 0) {
        ldpp_dout(dpp, LOG_ERROR) << __func__  << ": ERROR: Failed to delete the object from Motr."
                          <<" key=" << delete_key << dendl;
        return rc;
      }
      // if deleted object version is the latest version,
      // then update is-latest flag to true for previous version.
      if (ent.is_current()) {
        ldpp_dout(dpp, 20) << __func__  << ": Updating previous version entries " << dendl;
        bool set_is_latest=true;
        rc = source->update_version_entries(dpp, set_is_latest);
        if (rc < 0)
          return rc;
      }
    } else {
      // generate version-id for delete marker.
      result.delete_marker = true;
      source->gen_rand_obj_instance_name();
      std::string del_marker_ver_id = source->get_instance();
      result.version_id = del_marker_ver_id;
      source->delete_marker = true;

      if (!info.versioning_enabled()) {
        result.version_id = "";
        if (ent.is_delete_marker() && ent.key.instance == "") {
          ldpp_dout(dpp, 0) << __func__  << ": null-delete-marker is already present." << dendl;
          return 0;
        }
        // if latest version is null version, then delete the null version-object and
        // add reference of delete-marker in null reference key.
        ldpp_dout(dpp, 20) <<__func__ << ": ent.key=" << ent.key.to_string() << dendl;
        if (ent.key.instance == "") {
          source->set_instance(ent.key.instance);
          rc = source->remove_mobj_and_index_entry(
            dpp, ent, delete_key, bucket_index_iname, tenant_bkt_name);
          if (rc < 0) {
            ldpp_dout(dpp, LOG_ERROR) << __func__  << ": ERROR: Failed to delete the object from Motr, key="<< delete_key << dendl;
            return rc;
          }
        }
      }

      source->set_instance(result.version_id);
      // update is-latest=false for current version entry.
      ldpp_dout(dpp, 20) << __func__  << ": Updating previous version entries " << dendl;
      rc = source->update_version_entries(dpp);
      if (rc < 0)
        return rc;
      rc = this->create_delete_marker(dpp, ent);
      if (rc < 0)
        return rc;
    }
    if (result.version_id == "")
      result.version_id = "null"; // show it as "null" in the reply
  } else {
    // Unversioned flow
    rc = source->remove_mobj_and_index_entry(
        dpp, ent, delete_key, bucket_index_iname, tenant_bkt_name);
    if (rc < 0) {
      ldpp_dout(dpp, LOG_ERROR) << ": ERROR: Failed to delete the object from Motr, key="<< delete_key << dendl;
      return rc;
    }
  }

  return 0;
}

int MotrObject::MotrDeleteOp::create_delete_marker(const DoutPrefixProvider* dpp, rgw_bucket_dir_entry& ent)
{
  // creating a delete marker
  string tenant_bkt_name = get_bucket_name(source->get_bucket()->get_tenant(), source->get_bucket()->get_name());
  string bucket_index_iname = "motr.rgw.bucket.index." + tenant_bkt_name;
  bufferlist del_mark_bl;
  rgw_bucket_dir_entry ent_del_marker;
  ent_del_marker.key.name = source->get_name();
  ent_del_marker.key.instance = result.version_id;
  ent_del_marker.meta.owner = params.obj_owner.get_id().to_str();
  ent_del_marker.meta.owner_display_name = params.obj_owner.get_display_name();
  ent_del_marker.flags = rgw_bucket_dir_entry::FLAG_DELETE_MARKER | rgw_bucket_dir_entry::FLAG_CURRENT;
  if (real_clock::is_zero(params.mtime))
    ent_del_marker.meta.mtime = real_clock::now();
  else
    ent_del_marker.meta.mtime = params.mtime;

  rgw::sal::Attrs attrs;
  ent_del_marker.encode(del_mark_bl);
  encode(attrs, del_mark_bl);
  ent_del_marker.meta.encode(del_mark_bl);
  // key for delete marker - obj1[delete-markers's ver-id].
  std::string delete_marker_key = source->get_key_str();
  ldpp_dout(dpp, 20) <<__func__ << ": Add delete marker in bucket index, key=" << delete_marker_key << dendl;
  int rc = source->store->do_idx_op_by_name(bucket_index_iname,
                                        M0_IC_PUT, delete_marker_key, del_mark_bl);
  if (rc < 0) {
    ldpp_dout(dpp, LOG_ERROR) <<__func__ << ": ERROR: Failed to add delete marker in bucket." << dendl;
    return rc;
  }
  // Update in the cache.
  source->store->get_obj_meta_cache()->put(dpp, delete_marker_key, del_mark_bl);

  return rc;
}

int MotrObject::remove_mobj_and_index_entry(
    const DoutPrefixProvider* dpp, rgw_bucket_dir_entry& ent,
    std::string delete_key, std::string bucket_index_iname,
    std::string bucket_name)
{
  int rc;
  bufferlist bl;
  uint64_t size_rounded = 0;
  bool pushed_to_gc = false;

  // handling empty size object case
  if (ent.meta.size != 0) {
    if (ent.meta.category == RGWObjCategory::MultiMeta) {
      this->set_category(RGWObjCategory::MultiMeta);
      if (store->gc_enabled()) {
        std::string upload_id;
        rc = store->get_upload_id(bucket_name, delete_key, upload_id);
        if (rc < 0) {
          ldpp_dout(dpp, 0) <<__func__<< ": ERROR: get_upload_id failed. rc=" << rc << dendl;
        } else {
          std::string obj_fqdn = this->get_name() + "." + upload_id;
          std::string iname = "motr.rgw.bucket." + bucket_name + ".multiparts";
          ldpp_dout(dpp, 20) << __func__ << ": object part index=" << iname << dendl;
          //MotrObjectMeta *mobj = reinterpret_cast<MotrObjectMeta*>(&this->meta);
          motr_gc_obj_info gc_obj(upload_id, obj_fqdn, &this->meta, std::time(nullptr),
                                  ent.meta.size, iname);
          rc = store->get_gc()->enqueue(gc_obj);
          if (rc == 0) {
            pushed_to_gc = true;
            ldpp_dout(dpp, 20) << __func__ << ": pushed object " << obj_fqdn
                               << " with tag " << upload_id 
                               << " to motr garbage collector." << dendl;
          }
        }
      }
      if (!pushed_to_gc) {
        if (this->meta.is_composite)
          rc = this->delete_part_objs(dpp, &size_rounded)? : // Remove part info only.
   	       this->delete_hsm_enabled_mobj(dpp);
	else
          rc = this->delete_part_objs(dpp, &size_rounded);
      }
    } else {
      // Handling Simple Object Deletion
      // Open the object if not already open.
      // No need to close mobj as delete_mobj will open it again
      if (mobj == nullptr) {
        rc = this->open_mobj(dpp);
        if (rc < 0) {
          ADDB(RGW_ADDB_REQUEST_ID, addb_logger.get_id(),
               RGW_ADDB_FUNC_DELETE_MOBJ, RGW_ADDB_PHASE_ERROR);
          return rc;
        }
      }
      size_rounded = roundup(ent.meta.size, get_unit_sz());
      if (store->gc_enabled()) {
        std::string tag = this->meta.oid_str();
        std::string obj_fqdn = bucket_name + "/" + delete_key;
        //MotrObjectMeta *mobj = reinterpret_cast<MotrObjectMeta*>(&this->meta);
        motr_gc_obj_info gc_obj(tag, obj_fqdn, &this->meta, std::time(nullptr),
                                ent.meta.size);
        rc = store->get_gc()->enqueue(gc_obj);
        if (rc == 0) {
          pushed_to_gc = true;
          ldpp_dout(dpp, 20) << __func__ << ": pushed object " << obj_fqdn
                               << " with tag " << tag 
                               << " to motr garbage collector." << dendl;
        }
      }

      ldpp_dout(dpp, 0) <<__func__ << "[sining]: don't push to gc" << dendl;
      if (! pushed_to_gc)
        rc = this-meta.is_composite? 
	     this->delete_hsm_enabled_mobj(dpp) : this->delete_mobj(dpp);
    }
    if (rc < 0) {
      ldpp_dout(dpp, LOG_ERROR) <<__func__ << ": ERROR: Failed to delete the object " << delete_key  <<" from Motr." << dendl;
      return rc;
    }
  }
  rc = this->store->do_idx_op_by_name(bucket_index_iname,
                                      M0_IC_DEL, delete_key, bl);
  if (rc < 0) {
    ldpp_dout(dpp, LOG_ERROR) <<__func__ << ": ERROR: Failed to delete object's entry " << delete_key
                                      << " from bucket index." << dendl;
    return rc;
  }

  // Subtract object size & count from the bucket stats.
  if (ent.is_delete_marker())
    return rc;
  rc = update_bucket_stats(dpp, this->store, ent.meta.owner, bucket_name,
                           ent.meta.size, size_rounded, 1, false);
  if (rc != 0) {
    ldpp_dout(dpp, 0) <<__func__ << ": Failed stats substraction for the "
      << "bucket/obj=" << bucket_name << "/" << delete_key
      << ", rc=" << rc << dendl;
    return rc;
  }
  ldpp_dout(dpp, 70) <<__func__ << ": Stats subtracted successfully for the "
      << "bucket/obj=" << bucket_name << "/" << delete_key
      << ", rc=" << rc << dendl;

  return rc;
}

int MotrObject::delete_object(const DoutPrefixProvider* dpp, RGWObjectCtx* obj_ctx, optional_yield y, bool prevent_versioning)
{
  MotrObject::MotrDeleteOp del_op(this, obj_ctx);
  del_op.params.bucket_owner = bucket->get_info().owner;
  del_op.params.versioning_status = bucket->get_info().versioning_status();

  return del_op.delete_obj(dpp, y);
}

int MotrObject::delete_obj_aio(const DoutPrefixProvider* dpp, RGWObjState* astate,
    Completions* aio, bool keep_index_consistent,
    optional_yield y)
{
  /* XXX: Make it async */
  return 0;
}

int MotrCopyObj_CB::handle_data(bufferlist& bl, off_t bl_ofs, off_t bl_len)
{
  int rc = 0;
  ldpp_dout(m_dpp, 20) << "Offset=" << bl_ofs << " Length = "
                       << " Write Offset=" << write_offset << bl_len << dendl;

  //offset is zero and bufferlength is equal to bl_len
  if (!bl_ofs && bl_len == bl.length()) {
    bufferptr bptr(bl.c_str(), bl_len);
    bufferlist blist;
    blist.push_back(bptr);
    rc = m_dst_writer->process(std::move(blist), write_offset);
    if(rc < 0){
      ldpp_dout(m_dpp, LOG_ERROR) << ": ERROR: writer process bl_ofs=0 && " <<
                          "bl_len=" << bl.length() << " Write Offset=" <<
                          write_offset << "failed rc=" << rc << dendl;
    }
    write_offset += bl_len;
    dump_continue(s);
    return rc;
  }

  bufferptr bp(bl.c_str() + bl_ofs, bl_len);
  bufferlist new_bl;
  new_bl.push_back(bp);

  rc = m_dst_writer->process(std::move(new_bl), write_offset);
  if(rc < 0){
    ldpp_dout(m_dpp, LOG_ERROR) <<__func__ << ": ERROR: writer process failed rc=" << rc
                         << " Write Offset=" << write_offset << dendl;
    return rc;
  }
  dump_continue(s);
  write_offset += bl_len;

  ldpp_dout(m_dpp, 20) <<__func__ << ": MotrCopyObj_CB handle_data called rc=" << rc << dendl;
  return rc;
}


int MotrObject::copy_object_same_zone(RGWObjectCtx& obj_ctx,
    User* user,
    req_info* info,
    const rgw_zone_id& source_zone,
    rgw::sal::Object* dest_object,
    rgw::sal::Bucket* dest_bucket,
    rgw::sal::Bucket* src_bucket,
    const rgw_placement_rule& dest_placement,
    ceph::real_time* src_mtime,
    ceph::real_time* mtime,
    const ceph::real_time* mod_ptr,
    const ceph::real_time* unmod_ptr,
    bool high_precision_time,
    const char* if_match,
    const char* if_nomatch,
    AttrsMod attrs_mod,
    bool copy_if_newer,
    Attrs& new_attrs,
    RGWObjCategory category,
    uint64_t olh_epoch,
    boost::optional<ceph::real_time> delete_at,
    std::string* version_id,
    std::string* tag,
    std::string* etag,
    void (*progress_cb)(off_t, void *),
    void* progress_data,
    const DoutPrefixProvider* dpp,
    optional_yield y)
{
  int rc = 0;
  std::string ver_id;
  std::string req_id;

  ldpp_dout(dpp, 20) << "Src Object Name : " << this->get_key().get_oid() << dendl;
  ldpp_dout(dpp, 20) << "Dest Object Name : " << dest_object->get_key().get_oid() << dendl;

  //similar src and dest object name is not supported as of now
  if(this->get_obj() == dest_object->get_obj())
    return -ERR_NOT_IMPLEMENTED;

  std::unique_ptr<rgw::sal::Object::ReadOp> read_op = this->get_read_op(&obj_ctx);

  // prepare read op
  read_op->params.lastmod = src_mtime;
  read_op->params.if_match = if_match;
  read_op->params.if_nomatch = if_nomatch;
  read_op->params.mod_ptr = mod_ptr;
  read_op->params.unmod_ptr = unmod_ptr;

  rc = read_op->prepare(y, dpp);
  if(rc < 0) {
    ldpp_dout(dpp, LOG_ERROR) <<__func__ << ": ERROR: read op prepare failed rc=" << rc << dendl;
    return rc;
  }

  if(version_id) {
    ver_id = *version_id;
  }
  if(tag) {
    req_id = *tag;
  }

  struct req_state* s = static_cast<req_state*>(obj_ctx.get_private());
  // prepare write op
  std::shared_ptr<rgw::sal::Writer> dst_writer = store->get_atomic_writer(dpp, y,
        dest_object->clone(),
        s->bucket_owner.get_id(), obj_ctx,
        &dest_placement, olh_epoch, req_id);

  rc = dst_writer->prepare(y);
  if(rc < 0) {
    ldpp_dout(dpp, LOG_ERROR) <<__func__ << ": ERROR: writer prepare failed rc=" << rc << dendl;
    return rc;
  }

  // Create filter object.
  MotrCopyObj_CB cb(dpp, dst_writer, obj_ctx);
  MotrCopyObj_Filter* filter = &cb;

  // Get offsets.
  int64_t cur_ofs = 0, cur_end = obj_size;
  rc = this->range_to_ofs(obj_size, cur_ofs, cur_end);
  if (rc < 0) {
    ldpp_dout(dpp, LOG_ERROR) <<__func__ << ": ERROR: read op range_to_ofs failed rc=" << rc << dendl;
    return rc;
  }

  // read from/write to motr, if source object is non empty object.
  if (obj_size > 0) {
    // read::iterate -> handle_data() -> write::process
    rc = read_op->iterate(dpp, cur_ofs, cur_end, filter, y);
    if (rc < 0) {
      ldpp_dout(dpp, LOG_ERROR) <<__func__ << ": ERROR: read op iterate failed rc=" << rc << dendl;
      return rc;
    }
  }

  real_time time = ceph::real_clock::now();
  if(mtime) {
    *mtime = time;
  }

  //fetch etag.
  bufferlist bl;
  rc = read_op->get_attr(dpp, RGW_ATTR_ETAG, bl, y);
  if (rc < 0) {
    ldpp_dout(dpp, LOG_ERROR) <<__func__ << ": ERROR: read op for etag failed rc=" << rc << dendl;
    return rc;
  }
  string etag_str;
  etag_str.assign(bl.c_str(), bl.length());

  if(etag) {
    *etag = etag_str;
  }

  //Set object tags based on tagging-directive
  auto tagging_drctv = s->info.env->get("HTTP_X_AMZ_TAGGING_DIRECTIVE");

  bufferlist tags_bl;
  if (tagging_drctv) {
    if (strcasecmp(tagging_drctv, "COPY") == 0) {
      rc = read_op->get_attr(dpp, RGW_ATTR_TAGS, tags_bl, y);
      if (rc < 0) {
        ldpp_dout(dpp, LOG_DEBUG) <<__func__ << ": DEBUG: No tags present for source object rc=" << rc << dendl;
      }
    } else if (strcasecmp(tagging_drctv, "REPLACE") == 0) {
      ldpp_dout(dpp, LOG_INFO) <<__func__ << ": INFO: Parse tag values for object: " << dest_object->get_key().to_str() << dendl;
      int r = parse_tags(dpp, tags_bl, s);
      if (r < 0) {
        ldpp_dout(dpp, LOG_ERROR) <<__func__ << ": ERROR: Parsing object tags failed rc=" << rc << dendl;
        return r;
      }
    }
  attrs[RGW_ATTR_TAGS] = tags_bl;
  }

  real_time del_time;

  // write::complete - overwrite and md handling done here
  rc = dst_writer->complete(obj_size, etag_str,
                      mtime, time,
                      attrs,
                      del_time,
                      if_match,
                      if_nomatch,
                      nullptr,
                      nullptr,
                      nullptr,
                      y);
  if (rc < 0) {
    ldpp_dout(dpp, LOG_ERROR) << ": ERROR: writer complete failed rc=" << rc << dendl;
    return rc;
  }

  return rc;
}

int MotrObject::copy_object(RGWObjectCtx& obj_ctx,
    User* user,
    req_info* info,
    const rgw_zone_id& source_zone,
    rgw::sal::Object* dest_object,
    rgw::sal::Bucket* dest_bucket,
    rgw::sal::Bucket* src_bucket,
    const rgw_placement_rule& dest_placement,
    ceph::real_time* src_mtime,
    ceph::real_time* mtime,
    const ceph::real_time* mod_ptr,
    const ceph::real_time* unmod_ptr,
    bool high_precision_time,
    const char* if_match,
    const char* if_nomatch,
    AttrsMod attrs_mod,
    bool copy_if_newer,
    Attrs& attrs,
    RGWObjCategory category,
    uint64_t olh_epoch,
    boost::optional<ceph::real_time> delete_at,
    std::string* version_id,
    std::string* tag,
    std::string* etag,
    void (*progress_cb)(off_t, void *),
    void* progress_data,
    const DoutPrefixProvider* dpp,
    optional_yield y)
{
  int rc = 0;
  auto& src_zonegrp = src_bucket->get_info().zonegroup;
  auto& dest_zonegrp = dest_bucket->get_info().zonegroup;

  if(src_zonegrp.compare(dest_zonegrp) != 0){
    ldpp_dout(dpp, LOG_WARNING) <<__func__ << ": WARNING: Unsupported Action Requested." << dendl;
    return -ERR_NOT_IMPLEMENTED;
  }

  ldpp_dout(dpp, 20) <<__func__ << "Src and Dest Zonegroups are same."
                    << "src_zonegrp : " << src_zonegrp
                    << "dest_zonegrp : " << dest_zonegrp << dendl;

  //
  // Check if src object is encrypted.
  rgw::sal::Attrs &src_attrs = this->get_attrs();
  if (src_attrs.count(RGW_ATTR_CRYPT_MODE)) {
    // Current implementation does not follow S3 spec and even
    // may result in data corruption silently when copying
    // multipart objects acorss pools. So reject COPY operations
    //on encrypted objects before it is fully functional.
    ldpp_dout(dpp, LOG_ERROR) <<__func__ << ": ERROR: copy op for encrypted object has not been implemented." << dendl;
    return -ERR_NOT_IMPLEMENTED;
  }

  rc = copy_object_same_zone(obj_ctx,
                            user,
                            info,
                            source_zone,
                            dest_object,
                            dest_bucket,
                            src_bucket,
                            dest_placement,
                            src_mtime,
                            mtime,
                            mod_ptr,
                            unmod_ptr,
                            high_precision_time,
                            if_match,
                            if_nomatch,
                            attrs_mod,
                            copy_if_newer,
                            attrs,
                            category,
                            olh_epoch,
                            delete_at,
                            version_id,
                            tag,
                            etag,
                            progress_cb,
                            progress_data,
                            dpp,
                            y);
  if (rc < 0) {
    ldpp_dout(dpp, LOG_ERROR) <<__func__ << ": ERROR: copy_object_same_zone failed rc=" << rc << dendl;
    return rc;
  }

  ldpp_dout(dpp, 10) <<__func__ << ": Copy op completed rc=" << rc << dendl;
  return rc;
}

int MotrObject::swift_versioning_restore(RGWObjectCtx* obj_ctx,
    bool& restored,
    const DoutPrefixProvider* dpp)
{
  return 0;
}

int MotrObject::swift_versioning_copy(RGWObjectCtx* obj_ctx,
    const DoutPrefixProvider* dpp,
    optional_yield y)
{
  return 0;
}

MotrAtomicWriter::MotrAtomicWriter(const DoutPrefixProvider *dpp,
          optional_yield y,
          std::unique_ptr<rgw::sal::Object> _head_obj,
          MotrStore* _store,
          const rgw_user& _owner, RGWObjectCtx& obj_ctx,
          const rgw_placement_rule *_ptail_placement_rule,
          uint64_t _olh_epoch,
          const std::string& _unique_tag) :
        Writer(dpp, y),
        store(_store),
        owner(_owner),
        ptail_placement_rule(_ptail_placement_rule),
        olh_epoch(_olh_epoch),
        unique_tag(_unique_tag),
        obj(_store, _head_obj->get_key(), _head_obj->get_bucket()) {
  struct req_state* s = static_cast<req_state*>(obj_ctx.get_private());
  req_id = s->id;
  addb_logger.set_id(req_id);

  ADDB(RGW_ADDB_REQUEST_OPCODE_ID, addb_logger.get_id(), s->op_type);
}

static const unsigned MAX_BUFVEC_NR = 256;

int MotrAtomicWriter::prepare(optional_yield y)
{
  total_data_size = 0;

  addb_logger.set_id(req_id);

  if (obj.is_opened())
    return 0;

  rgw_bucket_dir_entry ent;

  int rc = m0_bufvec_empty_alloc(&buf, MAX_BUFVEC_NR) ?:
           m0_bufvec_alloc(&attr, MAX_BUFVEC_NR, 1) ?:
           m0_indexvec_alloc(&ext, MAX_BUFVEC_NR);
  if (rc != 0)
    this->cleanup();

  return rc;
}

#define M0_OP_EXEC_SYNC(op, rc) \
do { \
  m0_op_launch(&op, 1); \
  rc = m0_op_wait(op, M0_BITS(M0_OS_FAILED, M0_OS_STABLE), M0_TIME_NEVER) ?: \
       m0_rc(op); \
  m0_op_fini(op); \
  m0_op_free(op); \
  op = nullptr; \
 \
} while(0)

int MotrObject::create_hsm_enabled_mobj(const DoutPrefixProvider *dpp, uint64_t sz)
{
  // Note that the extents are created when a whole write op is done as
  // the offset and size are known then.
  struct m0_uint128 top_layer_oid;
  int rc = create_composite_obj(dpp, sz, &top_layer_oid);
  if (rc == 0) {
    this->meta.is_composite = true;
    this->meta.top_layer_oid = top_layer_oid;
  }
  return rc;
}

int MotrObject::create_composite_obj(const DoutPrefixProvider *dpp, uint64_t sz,
                                     struct m0_uint128 *layer_oid)
{
  // Creating a composite object includes 2 steps:
  // (1) Create a `normal` object.
  // (2) Set composite layout for this newly created object.
  //
  // The decision to not pack the above 2 steps in is to handle failures
  // properly. A failure could happen in creating a `normal` object or
  // setting composite layout, which requires different clear-up handling.
  //
  // Note: MIO only supports creating a composite object from a newly created
  // one (not yet written any data). As a composite object will be accessed
  // by HSM tool, the object's metadata has to be stored in Motr, so set
  // the 'store_own_meta' flag to be false for create_mobj().
  ldpp_dout(dpp, 0) <<__func__ << "[sining]: create a normal object" << dendl;
  int rc = this->create_mobj(dpp, sz, false);
  if (rc != 0)
    return rc;

  // Add a top layer and an extent to cover the whole top layer.
  std::vector<std::pair<uint64_t, uint64_t>> exts;
  exts.emplace_back(std::pair<uint64_t, uint64_t>(0, M0_BCOUNT_MAX));
  rc = add_composite_layer(dpp, -1, layer_oid)? :
       add_composite_layer_extents(dpp, *layer_oid, exts, true) ? :
       add_composite_layer_extents(dpp, *layer_oid, exts, false);
  if (rc != 0) {
    this->delete_mobj(dpp);
    return rc;
  }
  
  return 0;
}

int MotrObject::add_composite_layer(const DoutPrefixProvider *dpp, int priority,
                                    struct m0_uint128 *layer_oid)
{
  struct m0_client_layout *layout = nullptr; 
  struct m0_obj *layer_obj = nullptr;
  uint64_t lid = M0_OBJ_LAYOUT_ID(meta.layout_id);
  struct m0_op *op = nullptr;
  bool layout_is_alloced = false;
  ldpp_dout(dpp, 0) <<__func__ << "[sining]: enter" << dendl;

  // Generate an object id for the top layer.
  // Must be conherent with Motr HSM API's defintion on layer ID.
  M0_ASSERT(layer_oid != nullptr);
  memset(layer_oid, 0, sizeof(*layer_oid));
  int rc = m0_ufid_next(&ufid_gr, 1, layer_oid);
  if (rc != 0) {
    ldpp_dout(dpp, LOG_ERROR) <<__func__ << ": ERROR: m0_ufid_next() failed: " << rc << dendl;
    return rc;
  } 
  if (priority == -1) {
    int32_t gen = 0;
    uint8_t top_tier = 0;
    priority = ((0x00FFFFFF - gen) << 8) | top_tier;
  }
  //layer_oid->u_hi <<= 32;
  //layer_oid->u_hi |= (1LL << 31);
  //layer_oid->u_hi |= priority;

  ldpp_dout(dpp, 0) <<__func__ << "[sining]: layer_oid=[0x" << std::hex << layer_oid->u_hi
                                  << ":0x" << std::hex << layer_oid->u_lo 
                                  << "], layout_id=0x" << std::hex << lid << dendl;

  // Create an object for this layer.
  ldpp_dout(dpp, 0) <<__func__ << "[sining]: create a layer object" << dendl;
  layer_obj = new m0_obj();
  m0_obj_init(layer_obj, &store->container.co_realm, layer_oid, lid);
  layer_obj->ob_entity.en_flags |= M0_ENF_GEN_DI;
  rc = m0_entity_create(nullptr, &layer_obj->ob_entity, &op);
  if (rc != 0) {
    ldpp_dout(dpp, LOG_ERROR) <<__func__ << ": ERROR: m0_entity_create() failed, rc=" << rc << dendl;
    goto error;
  }
  M0_OP_EXEC_SYNC(op, rc);
  if (rc != 0) {
    ldpp_dout(dpp, LOG_ERROR) <<__func__ << ": ERROR: failed to create motr object. rc=" << rc << dendl;
    goto error;
  }
  ldpp_dout(dpp, 0) <<__func__ << "[sining]: pver = ["
                                  << "0x" << std::hex << layer_obj->ob_attr.oa_pver.f_container
				  << ":0x" << std::hex << layer_obj->ob_attr.oa_pver.f_key << "]" << dendl;

  // Update composite object's layout to add the layer.
  layout = this->mobj->ob_layout; 
  if (layout == nullptr) {
    // When an object is newly created, ob_layout is not set until
    // LAYOUT_SET op is executed.
    ldpp_dout(dpp, 0) <<__func__ << "[sining]: create a composite layout" << dendl;
    layout = m0_client_layout_alloc(M0_LT_COMPOSITE);
    if (layout == NULL) {
      rc = -ENOMEM;
      goto error;
    }
    layout_is_alloced = true;
  }
  ldpp_dout(dpp, 0) <<__func__ << "[sining]: update the layout by adding a new layer" << dendl;
  rc = m0_composite_layer_add(layout, layer_obj, priority);
  if (rc != 0)
    goto error;

  ldpp_dout(dpp, 0) <<__func__ << "[sining]: launch layout op" << dendl;
  m0_client_layout_op(this->mobj, M0_EO_LAYOUT_SET, layout, &op);
  M0_OP_EXEC_SYNC(op, rc);
  if (rc < 0)
    goto error;
  return 0;

error:
  if (layout_is_alloced == true)
    m0_client_layout_free(layout);
  if (layer_obj) {
    rc = m0_entity_delete(&layer_obj->ob_entity, &op);
    if (rc != 0) {
      ldpp_dout(dpp, LOG_ERROR) <<__func__ << ": ERROR: m0_entity_delete() failed. rc=" << rc << dendl;
      return rc;
    }

    M0_OP_EXEC_SYNC(op, rc);   
  }
  return rc;
}

int MotrObject::add_composite_layer_extents(const DoutPrefixProvider *dpp,
                                            struct m0_uint128 layer_oid,
                                            vector<std::pair<uint64_t, uint64_t>>& exts,
					    bool is_write)
{
  struct m0_idx idx = {};
  char *kbuf;
  char *vbuf;
  uint64_t klen;
  uint64_t vlen;

  ldpp_dout(dpp, 0) <<__func__ << "[sining]: enter" << dendl;
  int rc = m0_composite_layer_idx(layer_oid, is_write, &idx);
  ldpp_dout(dpp, 0) <<__func__ << "[sining]: get composite layer idx rc = " << rc << dendl;
  if (rc != 0) {
    ldpp_dout(dpp, LOG_ERROR) <<__func__ << ": ERROR: failed to get composite layer index: rc="
                              << rc << dendl;
    return rc;
  }

  for (auto& ext: exts) {
    struct m0_composite_layer_idx_key ext_key;
    ext_key.cek_layer_id = layer_oid;
    ext_key.cek_off = ext.first;
    struct m0_composite_layer_idx_val ext_val;
    ext_val.cev_len = ext.second;
    rc = m0_composite_layer_idx_key_to_buf(&ext_key, (void **)&kbuf, &klen)? :
         m0_composite_layer_idx_val_to_buf(&ext_val, (void **)&vbuf, &vlen);
    if (rc < 0)
      break;

    ldpp_dout(dpp, 0) <<__func__ << "[sining]: layer_oid=[0x" << std::hex << layer_oid.u_hi
                                 << ":0x" << std::hex << layer_oid.u_lo 
                                 << "], off =0x" << std::hex << ext.first
				 << ", len = 0x" << std::hex << ext.second  << ", "
				 << (is_write? "write" : "read" ) << "ext" << dendl;

    // This is not performance-wise. All key-value pairs should
    // be sent in one op. Add do_idx_op_batch() in MotrStore.
    vector<uint8_t> key(klen);
    vector<uint8_t> val(vlen);
    memcpy(reinterpret_cast<char*>(key.data()), kbuf, klen);
    memcpy(reinterpret_cast<char*>(val.data()), vbuf, vlen);
    ldpp_dout(dpp, 0) <<__func__ << "[sining]: add the extent to idx" << dendl;
    rc = this->store->do_idx_op(&idx, M0_IC_PUT, key, val);
    if (rc < 0)
      break;
  }

  return rc;
}

// A new Motr API to parse the Motr composite layout data structure is
// needed as a privately defined list is used in Motr.
// Place holder: m0_composite_layer_get().
// 
// Ugly Hack: it is done by iterating the layer list via explictly
// manipulating the list as m0_tl_* APIs are exposed in 'libmotr'.

int MotrObject::list_composite_layers(const DoutPrefixProvider *dpp,
                                      std::vector<struct m0_uint128>& layer_oids)
{
  int rc = 0;
  struct m0_client_layout *layout; 
  struct m0_op *op = nullptr;

  layout = m0_client_layout_alloc(M0_LT_COMPOSITE);
  if (layout == nullptr)
    return -ENOMEM;
  m0_client_layout_op(this->mobj, M0_EO_LAYOUT_GET, layout, &op);
  M0_OP_EXEC_SYNC(op, rc);
  if (rc < 0)
    return rc;

  struct m0_client_composite_layout *clayout;
  clayout = container_of(layout, struct m0_client_composite_layout, ccl_layout);
  int nr_layers = clayout->ccl_nr_layers;
  ldpp_dout(dpp, 0) <<__func__ << "[sining]: nr_layers = " << nr_layers << dendl;
  struct m0_list_link *lnk;
  struct m0_tlink *tlnk;
  lnk = clayout->ccl_layers.t_head.l_head;
  for (int i = 0; i < nr_layers; i++) {
    if (lnk == NULL)
      break;

    ldpp_dout(dpp, 0) <<__func__ << "[sining]:  lnk = " << std::hex << lnk << dendl;
    tlnk = container_of(lnk, struct m0_tlink, t_link);
    ldpp_dout(dpp, 0) <<__func__ << "[sining]:  tlnk = " << std::hex << tlnk << dendl;
    struct m0_composite_layer *layer = container_of(tlnk, struct m0_composite_layer, ccr_tlink);
    ldpp_dout(dpp, 0) <<__func__ << "[sining]:  layer = " << std::hex << layer << dendl;
    ldpp_dout(dpp, 0) <<__func__ << "[sining]:  emplace" << dendl;
    layer_oids.emplace_back(layer->ccr_subobj);
    ldpp_dout(dpp, 0) <<__func__ << "[sining]:  layer oid = [0x" 
                                 << std::hex << layer->ccr_subobj.u_hi 
    				 << ":0x" << std::hex  << layer->ccr_subobj.u_lo << "]" << dendl;

    lnk = lnk->ll_next;
  }

  return 0;
}

int MotrObject::list_composite_layer_extents(const DoutPrefixProvider *dpp,
                                             struct m0_uint128 layer_oid,
				 	     int max_ext_num,
                                             vector<std::pair<uint64_t, uint64_t>>& exts,
                                             uint64_t curr_off, uint64_t *next_off,
					     bool *truncated)
{
  unsigned nr_kvp = std::min(max_ext_num, 128);
  struct m0_idx idx = {};
  vector<vector<uint8_t>> keys(nr_kvp);
  vector<vector<uint8_t>> vals(nr_kvp);
  char *kbuf;
  uint64_t klen;

  ldpp_dout(dpp, 0) <<__func__ << "[sining]:  layer oid = [0x" 
                               << std::hex << layer_oid.u_hi 
                               << ":0x" << std::hex  << layer_oid.u_lo << "]" << dendl;
  ldpp_dout(dpp, 0) <<__func__ << "[sining]:  enter, get layer idx" << dendl;
  int rc = m0_composite_layer_idx(layer_oid, true, &idx);
  if (rc != 0) {
    ldpp_dout(dpp, LOG_ERROR) <<__func__ << ": ERROR: failed to get composite layer index: rc="
                              << rc << dendl;
    return rc;
  }

  // Only the first element for keys needs to be set for NEXT query.
  // The keys will be set will the returned keys from motr index.
  struct m0_composite_layer_idx_key ext_key;
  ext_key.cek_layer_id = layer_oid;
  ext_key.cek_off = curr_off;
  rc = m0_composite_layer_idx_key_to_buf(&ext_key, (void **)&kbuf, &klen);
  keys[0].insert(keys[0].begin(), kbuf, kbuf + klen);
  ldpp_dout(dpp, 0) <<__func__ << "[sining]:  query for extents in index" << dendl;
  rc = this->store->do_idx_next_op(&idx, keys, vals);
  if (rc < 0) {
      ldpp_dout(dpp, LOG_ERROR) <<__func__ << ": ERROR: NEXT query failed, rc=" << rc << dendl;
      return rc;
  }

  // free(kbuf); ???

  uint64_t last_off = 0;
  exts.clear();

  ldpp_dout(dpp, 0) <<__func__ << "[sining]:  parse extents" << dendl;
  int ext_cnt = 0;
  for (int i = 0; i < int(keys.size()) && ext_cnt < max_ext_num; i++) {
    auto key = keys[i];
    auto val = vals[i];
    if (key.size() == 0 || val.size() == 0)
      break;

    ldpp_dout(dpp, 0) <<__func__ << "[sining]:  get extent out from buf" << dendl;
    struct m0_composite_layer_idx_val ext_val;
    m0_composite_layer_idx_key_from_buf(&ext_key, key.data());
    m0_composite_layer_idx_val_from_buf(&ext_val, val.data());

    ldpp_dout(dpp, 0) <<__func__ << "[sining]:  layer oid = [0x" 
                                 << std::hex << ext_key.cek_layer_id.u_hi 
    				 << ":0x" << std::hex  << ext_key.cek_layer_id.u_lo << "]" 
				 << ", off = " << std::hex << ext_key.cek_off 
				 << ", len = " << std::hex << ext_val.cev_len << dendl;
    if (ext_key.cek_layer_id.u_hi != layer_oid.u_hi ||
        ext_key.cek_layer_id.u_lo != layer_oid.u_lo)
      break;

    if (ext_key.cek_off > curr_off) {
      last_off = ext_key.cek_off;
      uint64_t ext_off = ext_key.cek_off;
      uint64_t ext_len = ext_val.cev_len;
      exts.emplace_back(std::pair<uint64_t, uint64_t>(ext_off, ext_len));
      ext_cnt++;
      ldpp_dout(dpp, 0) <<__func__ << "[sining]:  ext_cnt = " << ext_cnt << ", off = " << ext_off << ", len = " << ext_len << dendl;
    }
  }

  if (truncated)
    *truncated = ext_cnt > max_ext_num? true : false;

  if (next_off)
    *next_off = last_off;

  return 0;
}

int MotrObject::delete_composite_layer(const DoutPrefixProvider *dpp, struct m0_uint128 layer_oid)
{
  int rc;
  struct m0_client_layout *layout; 
  struct m0_op *op = nullptr;
  struct m0_obj layer_obj;

  // Opening an object doesn't retrieve its layout from motr,
  // get it via LAYOUT_GET.
  ldpp_dout(dpp, 0) <<__func__ << "[sining]:  retrieve layout then update it" << dendl;
  layout = m0_client_layout_alloc(M0_LT_COMPOSITE);
  if (layout == nullptr)
    return -ENOMEM;
  m0_client_layout_op(this->mobj, M0_EO_LAYOUT_GET, layout, &op);
  M0_OP_EXEC_SYNC(op, rc);
  if (rc < 0)
    goto exit;	
  m0_composite_layer_del(layout, layer_oid);
  m0_client_layout_op(this->mobj, M0_EO_LAYOUT_SET, layout, &op);
  M0_OP_EXEC_SYNC(op, rc);
  if (rc < 0)
    goto exit;

  // Delete this layer's sub-object.
  ldpp_dout(dpp, 0) <<__func__ << "[sining]:  delete the layer object" << dendl;
  memset(&layer_obj, 0, sizeof layer_obj);
  m0_obj_init(&layer_obj, &store->container.co_realm, &layer_oid, store->conf.mc_layout_id);
  //layer_obj.ob_entity.en_flags |= M0_ENF_GEN_DI;
  op = nullptr;
  rc = m0_entity_delete(&layer_obj.ob_entity, &op);
  if (rc != 0)
    goto exit;
  M0_OP_EXEC_SYNC(op, rc);

exit:
  m0_client_layout_free(layout);
  return rc;
}

int MotrObject::delete_composite_layer_extents(const DoutPrefixProvider *dpp,
                                               struct m0_uint128 layer_oid,
                                               std::vector<std::pair<uint64_t, uint64_t>>& exts)
{
  struct m0_idx idx = {};
  int rc = m0_composite_layer_idx(layer_oid, true, &idx);
  if (rc != 0) {
    ldpp_dout(dpp, LOG_ERROR) <<__func__ << ": ERROR: failed to get composite layer index: rc="
                              << rc << dendl;
    return rc;
  }

  for (auto ext: exts) {
    struct m0_composite_layer_idx_key ext_key;
    ext_key.cek_layer_id = layer_oid;
    ext_key.cek_off = ext.first;

    char *kbuf;
    uint64_t klen;
    rc = m0_composite_layer_idx_key_to_buf(&ext_key, (void **)&kbuf, &klen);
    if (rc < 0)
      break;

    // TODO: see add_composite_layer_extents().
    std::vector<uint8_t> key(klen);
    std::vector<uint8_t> val;
    memcpy(reinterpret_cast<char*>(key.data()), kbuf, klen);
    rc = this->store->do_idx_op(&idx, M0_IC_DEL, key, val);
    if (rc < 0)
      break;
  }

  return rc;
}

int MotrObject::delete_composite_obj(const DoutPrefixProvider *dpp)
{
  return this->delete_mobj(dpp);
}

int MotrObject::delete_hsm_enabled_mobj(const DoutPrefixProvider *dpp)
{
  int rc;
  ldpp_dout(dpp, 0) <<__func__ << "[sining]: open mobj" << dendl;
  if (this->mobj == nullptr) {
    rc = this->open_mobj(dpp);
    if (rc < 0)
      return rc;
  }

  // Get layers.
  std::vector<struct m0_uint128> layer_oids;
  ldpp_dout(dpp, 0) <<__func__ << "[sining]: list layers" << dendl;
  rc = list_composite_layers(dpp, layer_oids);
  if (rc < 0)
    return rc;

  // For each layers, get all extents and remove all of them.
  for (auto layer_oid: layer_oids) {
    int max_ext_num = 128;
    std::vector<std::pair<uint64_t, uint64_t>> exts;
    uint64_t next_off = 0;
    bool truncated = true;

    do {
      exts.clear();
      ldpp_dout(dpp, 0) <<__func__ << "[sining]: delete extents of a layer" << dendl;
      rc = list_composite_layer_extents(dpp, layer_oid, max_ext_num,
                                        exts, next_off, &next_off, &truncated)? :
           delete_composite_layer_extents(dpp, layer_oid, exts);
      if (rc < 0)
        return rc;
    } while (truncated);

    rc = delete_composite_layer(dpp, layer_oid);
    ldpp_dout(dpp, 0) <<__func__ << "[sining]: delete a layer, rc = " << rc << dendl;
    if (rc < 0)
      break;
  }

  return rc;
}

int MotrObject::create_mobj(const DoutPrefixProvider *dpp, uint64_t sz, bool store_own_meta)
{
  ADDB(RGW_ADDB_REQUEST_ID, addb_logger.get_id(),
       RGW_ADDB_FUNC_CREATE_MOBJ, RGW_ADDB_PHASE_START);

  if (mobj != nullptr) {
    ADDB(RGW_ADDB_REQUEST_ID, addb_logger.get_id(),
         RGW_ADDB_FUNC_CREATE_MOBJ, RGW_ADDB_PHASE_ERROR);
    ldpp_dout(dpp, LOG_ERROR) <<__func__ << ": ERROR: object is already opened" << dendl;

    return -EINVAL;
  }

  int rc = m0_ufid_next(&ufid_gr, 1, &meta.oid);
  if (rc != 0) {
    ADDB(RGW_ADDB_REQUEST_ID, addb_logger.get_id(),
         RGW_ADDB_FUNC_CREATE_MOBJ, RGW_ADDB_PHASE_ERROR);
    ldpp_dout(dpp, LOG_ERROR) <<__func__ << ": ERROR: m0_ufid_next() failed: " << rc << dendl;

    return rc;
  }
  expected_obj_size = sz;
  chunk_io_sz = expected_obj_size;
  if (expected_obj_size > MAX_ACC_SIZE)
    // Cap it to MAX_ACC_SIZE
    chunk_io_sz = MAX_ACC_SIZE;

  ldpp_dout(dpp, 20) <<__func__ << ": key=" << this->get_key().to_str()
                     << " size=" << sz << " meta:oid=[0x" << std::hex
                     << meta.oid.u_hi << ":0x" << meta.oid.u_lo << "]" << dendl;

  int64_t lid = m0_layout_find_by_objsz(store->instance, nullptr, sz);
  if (lid <= 0) {
    ldpp_dout(dpp, LOG_ERROR) <<__func__ << ": ERROR: failed to get lid: " << lid << dendl;
    return lid == 0 ? -EAGAIN : (int)lid;
  }

  M0_ASSERT(mobj == nullptr);
  mobj = new m0_obj();
  m0_obj_init(mobj, &store->container.co_realm, &meta.oid, lid);

  struct m0_op *op = nullptr;
  mobj->ob_entity.en_flags |= M0_ENF_GEN_DI;
  if (store_own_meta)
    mobj->ob_entity.en_flags |= M0_ENF_META; // Motr won't store metadata if this flag is set.
  rc = m0_entity_create(nullptr, &mobj->ob_entity, &op);
  if (rc != 0) {
    ADDB(RGW_ADDB_REQUEST_ID, addb_logger.get_id(),
         RGW_ADDB_FUNC_CREATE_MOBJ, RGW_ADDB_PHASE_ERROR);
    this->close_mobj();
    ldpp_dout(dpp, LOG_ERROR) <<__func__ << ": ERROR: m0_entity_create() failed, rc=" << rc << dendl;
    return rc;
  }
  ldpp_dout(dpp, 20) <<__func__ << ": call m0_op_launch()..." << dendl;
  ADDB(RGW_ADDB_REQUEST_TO_MOTR_ID, addb_logger.get_id(),
       m0_sm_id_get(&op->op_sm));
  m0_op_launch(&op, 1);
  rc = m0_op_wait(op, M0_BITS(M0_OS_FAILED, M0_OS_STABLE), M0_TIME_NEVER) ?:
       m0_rc(op);
  m0_op_fini(op);
  m0_op_free(op);

  if (rc != 0) {
    ADDB(RGW_ADDB_REQUEST_ID, addb_logger.get_id(),
         RGW_ADDB_FUNC_CREATE_MOBJ, RGW_ADDB_PHASE_ERROR);
    this->close_mobj();
    ldpp_dout(dpp, LOG_ERROR) <<__func__ << ": ERROR: failed to create motr object. rc=" << rc << dendl;
    return rc;
  }

  meta.layout_id = mobj->ob_attr.oa_layout_id;
  meta.pver      = mobj->ob_attr.oa_pver;
  ldpp_dout(dpp, 20) <<__func__ << ": key=" << this->get_key() << ", meta:oid=[0x" << std::hex << meta.oid.u_hi
                                  << ":0x" << std::hex << meta.oid.u_lo << "], meta:pvid=[0x" << std::hex
                                  << meta.pver.f_container << ":0x" << std::hex << meta.pver.f_key
                                  << "], meta:layout_id=0x" << std::hex << meta.layout_id << dendl;

  ADDB(RGW_ADDB_REQUEST_ID, addb_logger.get_id(),
       RGW_ADDB_FUNC_CREATE_MOBJ, RGW_ADDB_PHASE_DONE);
  // TODO: add key:user+bucket+key+obj.meta.oid value:timestamp to
  // gc.queue.index. See more at github.com/Seagate/cortx-rgw/issues/7.

  return rc;
}

int MotrObject::open_mobj(const DoutPrefixProvider *dpp)
{
  ldpp_dout(dpp, 20) <<__func__ << ": key=" << this->get_key().to_str() << ", meta:oid=[0x" << std::hex
                                 << meta.oid.u_hi  << ":0x" << std::hex  << meta.oid.u_lo << "]" << dendl;

  ADDB(RGW_ADDB_REQUEST_ID, addb_logger.get_id(),
       RGW_ADDB_FUNC_OPEN_MOBJ, RGW_ADDB_PHASE_START);

  int rc;
  if (meta.layout_id == 0) {
    rgw_bucket_dir_entry ent;
    rc = this->get_bucket_dir_ent(dpp, ent);
    if (rc < 0) {
      ADDB(RGW_ADDB_REQUEST_ID, addb_logger.get_id(),
           RGW_ADDB_FUNC_OPEN_MOBJ, RGW_ADDB_PHASE_ERROR);
      ldpp_dout(dpp, LOG_ERROR) <<__func__ << ": ERROR: get_bucket_dir_ent failed: rc=" << rc << dendl;
      return rc;
    }
  }

  if (meta.layout_id == 0) {
    ADDB(RGW_ADDB_REQUEST_ID, addb_logger.get_id(),
         RGW_ADDB_FUNC_OPEN_MOBJ, RGW_ADDB_PHASE_DONE);
    ldpp_dout(dpp, LOG_ERROR) <<__func__ << ": ERROR: did not find motr obj details." << dendl;
    return -ENOENT;
  }

  M0_ASSERT(mobj == nullptr);
  mobj = new m0_obj();
  memset(mobj, 0, sizeof *mobj);
  m0_obj_init(mobj, &store->container.co_realm, &meta.oid, store->conf.mc_layout_id);

  struct m0_op *op = nullptr;
  mobj->ob_attr.oa_layout_id = meta.layout_id;
  mobj->ob_attr.oa_pver      = meta.pver;
  mobj->ob_entity.en_flags  |= M0_ENF_GEN_DI;
  if (!meta.is_composite)
    mobj->ob_entity.en_flags |= M0_ENF_META; 
  ldpp_dout(dpp, 20) <<__func__ << ": key=" << this->get_key().to_str() << ", meta:oid=[0x" << std::hex << meta.oid.u_hi
                                 << ":0x" << meta.oid.u_lo << "], meta:pvid=[0x" << std::hex << meta.pver.f_container
                                 << ":0x" << meta.pver.f_key << "], meta:layout_id=0x" << std::hex << meta.layout_id << dendl;
  rc = m0_entity_open(&mobj->ob_entity, &op);
  if (rc != 0) {
    ADDB(RGW_ADDB_REQUEST_ID, addb_logger.get_id(),
         RGW_ADDB_FUNC_OPEN_MOBJ, RGW_ADDB_PHASE_ERROR);
    ldpp_dout(dpp, LOG_ERROR) <<__func__ << ": ERROR: m0_entity_open() failed: rc=" << rc << dendl;
    this->close_mobj();
    return rc;
  }

  ADDB(RGW_ADDB_REQUEST_TO_MOTR_ID, addb_logger.get_id(), m0_sm_id_get(&op->op_sm));
  m0_op_launch(&op, 1);
  rc = m0_op_wait(op, M0_BITS(M0_OS_FAILED, M0_OS_STABLE), M0_TIME_NEVER) ?:
       m0_rc(op);
  m0_op_fini(op);
  m0_op_free(op);

  if (rc < 0) {
    ADDB(RGW_ADDB_REQUEST_ID, addb_logger.get_id(),
         RGW_ADDB_FUNC_OPEN_MOBJ, RGW_ADDB_PHASE_ERROR);
    ldpp_dout(dpp, LOG_ERROR) <<__func__ << ": ERROR: failed to open motr object: rc=" << rc << dendl;
    this->close_mobj();
    return rc;
  }

  ADDB(RGW_ADDB_REQUEST_ID, addb_logger.get_id(),
       RGW_ADDB_FUNC_OPEN_MOBJ, RGW_ADDB_PHASE_DONE);
  ldpp_dout(dpp, 20) <<__func__ << ": exit. rc=" << rc << dendl;

  return 0;
}

int MotrObject::delete_mobj(const DoutPrefixProvider *dpp)
{
  int rc;
  char fid_str[M0_FID_STR_LEN];
  snprintf(fid_str, ARRAY_SIZE(fid_str), U128X_F, U128_P(&meta.oid));

  ADDB(RGW_ADDB_REQUEST_ID, addb_logger.get_id(),
       RGW_ADDB_FUNC_DELETE_MOBJ, RGW_ADDB_PHASE_START);

  if (!meta.oid.u_hi || !meta.oid.u_lo) {
    ldpp_dout(dpp, LOG_ERROR) <<__func__ << ": ERROR: invalid motr object oid=" << fid_str << dendl;
    ADDB(RGW_ADDB_REQUEST_ID, addb_logger.get_id(),
         RGW_ADDB_FUNC_DELETE_MOBJ, RGW_ADDB_PHASE_ERROR);
    return -EINVAL;
  }
  ldpp_dout(dpp, 20) <<__func__ << ": deleting motr object oid=" << fid_str << dendl;

  // Open the object.
  if (mobj == nullptr) {
    rc = this->open_mobj(dpp);
    if (rc < 0) {
      ADDB(RGW_ADDB_REQUEST_ID, addb_logger.get_id(),
           RGW_ADDB_FUNC_DELETE_MOBJ, RGW_ADDB_PHASE_ERROR);
      return rc;
    }
  }

  // Create an DELETE op and execute it (sync version).
  struct m0_op *op = nullptr;
  mobj->ob_entity.en_flags |= (M0_ENF_META | M0_ENF_GEN_DI);
  //mobj->ob_entity.en_flags |= (M0_ENF_META | M0_ENF_DI);
  rc = m0_entity_delete(&mobj->ob_entity, &op);
  if (rc != 0) {
    ldpp_dout(dpp, LOG_ERROR) <<__func__ << ": ERROR: m0_entity_delete() failed. rc=" << rc << dendl;
    ADDB(RGW_ADDB_REQUEST_ID, addb_logger.get_id(),
         RGW_ADDB_FUNC_DELETE_MOBJ, RGW_ADDB_PHASE_ERROR);
    return rc;
  }

  ADDB(RGW_ADDB_REQUEST_TO_MOTR_ID, addb_logger.get_id(), m0_sm_id_get(&op->op_sm));
  m0_op_launch(&op, 1);
  rc = m0_op_wait(op, M0_BITS(M0_OS_FAILED, M0_OS_STABLE), M0_TIME_NEVER) ?:
       m0_rc(op);
  m0_op_fini(op);
  m0_op_free(op);

  if (rc < 0) {
    ldpp_dout(dpp, LOG_ERROR) <<__func__ << ": ERROR: failed to open motr object. rc=" << rc << dendl;
    ADDB(RGW_ADDB_REQUEST_ID, addb_logger.get_id(),
         RGW_ADDB_FUNC_DELETE_MOBJ, RGW_ADDB_PHASE_ERROR);
    return rc;
  }

  ADDB(RGW_ADDB_REQUEST_ID, addb_logger.get_id(),
       RGW_ADDB_FUNC_DELETE_MOBJ, RGW_ADDB_PHASE_DONE);

  this->close_mobj();

  return 0;
}

void MotrObject::close_mobj()
{
  if (mobj == nullptr)
    return;
  m0_obj_fini(mobj);
  delete mobj; mobj = nullptr;
}

int MotrObject::write_mobj(const DoutPrefixProvider *dpp, bufferlist&& in_buffer, uint64_t offset)
{
  int rc;
  uint32_t flags = M0_OOF_FULL;
  int64_t bs, left;
  struct m0_op *op;
  char *start, *p;
  struct m0_bufvec buf;
  struct m0_bufvec attr;
  struct m0_indexvec ext;
  bool last_io = false;

  bufferlist data = std::move(in_buffer);

  ADDB(RGW_ADDB_REQUEST_ID, addb_logger.get_id(),
       RGW_ADDB_FUNC_WRITE_MOBJ, RGW_ADDB_PHASE_START);

  left = data.length();
  if (left == 0) {
    ADDB(RGW_ADDB_REQUEST_ID, addb_logger.get_id(),
         RGW_ADDB_FUNC_WRITE_MOBJ, RGW_ADDB_PHASE_DONE);

    return 0;
  }

  processed_bytes += left;
  int64_t available_data = 0;
  if (io_ctxt.accumulated_buffer_list.size() > 0) {
    // We are in data accumulation mode
    available_data = io_ctxt.total_bufer_sz;
  }
  bs = this->get_optimal_bs(chunk_io_sz);
  if (bs < chunk_io_sz)
    chunk_io_sz = bs;

  int64_t remaining_bytes = expected_obj_size - processed_bytes;
  // Check if this is the last io of the original object size
  if (remaining_bytes <= 0)
    last_io = true;

  ldpp_dout(dpp, 20) <<__func__ << ": Incoming data=" << left << " bs=" << bs << dendl;
  if ((left + available_data) < bs) {
    // Determine if there are any further chunks/bytes from socket to be processed
    if (remaining_bytes > 0) {
      if (io_ctxt.accumulated_buffer_list.size() == 0) {
        // Save offset
        io_ctxt.start_offset = offset;
      }
      // Append current buffer to the list of accumulated buffers
      ldpp_dout(dpp, 20) <<__func__ << " More incoming data (" <<  remaining_bytes 
          << " bytes) in-flight. Accumulating buffer..." << dendl;
      io_ctxt.accumulated_buffer_list.push_back(std::move(data));
      io_ctxt.total_bufer_sz += left;
      return 0;
    } else {
      // This is last IO. Check if we have previously accumulated buffers.
      // If not, simply use in_buffer/data
      if (io_ctxt.accumulated_buffer_list.size() > 0) {
        // Append last buffer
        io_ctxt.accumulated_buffer_list.push_back(std::move(data));
        io_ctxt.total_bufer_sz += left;
      }
    }
  } else if ((left + available_data) == bs)  {
    // Ready to write data to Motr. Add it to accumulated buffer
    if (io_ctxt.accumulated_buffer_list.size() > 0) {
      io_ctxt.accumulated_buffer_list.push_back(std::move(data));
      io_ctxt.total_bufer_sz += left;
    } // else, simply use in_buffer
  }

  rc = m0_bufvec_empty_alloc(&buf, 1) ?:
       m0_bufvec_alloc(&attr, 1, 1) ?:
       m0_indexvec_alloc(&ext, 1);
  if (rc != 0) {
    ldpp_dout(dpp, LOG_ERROR) <<__func__ << ": ERROR: buffer allocation failed, rc=" << rc << dendl;
    ADDB(RGW_ADDB_REQUEST_ID, addb_logger.get_id(),
         RGW_ADDB_FUNC_WRITE_MOBJ, RGW_ADDB_PHASE_ERROR);
    goto out;
  }

  ldpp_dout(dpp, 20) <<__func__ << ": left=" << left << " bs=" << bs << dendl;
  if (io_ctxt.accumulated_buffer_list.size() > 0) {
    // We have IO buffers accumulated. Transform it into single buffer.
    data.clear();
    for(auto &buffer: io_ctxt.accumulated_buffer_list) {
      data.claim_append(std::move(buffer));
    }
    offset = io_ctxt.start_offset;
    left = data.length();
    bs = this->get_optimal_bs(left);
    ldpp_dout(dpp, 20) <<__func__ << ": Accumulated data=" << left << " bs=" << bs << dendl;
    io_ctxt.accumulated_buffer_list.clear();
  } else {
    // No accumulated buffers.
    ldpp_dout(dpp, 20) <<__func__<< ": Data=" << left << " bs=" << bs << dendl;
  }

  start = data.c_str();
  for (p = start; left > 0; left -= bs, p += bs, offset += bs) {
    if (left < bs && last_io) {
      bs = this->get_optimal_bs(left, true);
      flags |= M0_OOF_LAST;
    }

    if (left < bs && last_io) {
      ldpp_dout(dpp, 20) <<__func__ << " Data ="<< left << ", bs=" << bs << ", Padding [" << (bs - left)
          << "] bytes to data" << dendl;
      data.append_zero(bs - left);
      p = data.c_str();
    }
    buf.ov_buf[0] = p;
    buf.ov_vec.v_count[0] = bs;
    ext.iv_index[0] = offset;
    ext.iv_vec.v_count[0] = bs;
    attr.ov_vec.v_count[0] = 0;

    ldpp_dout(dpp, 20) <<__func__ << ": Write data bytes=[" << bs << "], at offset=[" << offset << "]" << dendl;
    op = nullptr;
    this->mobj->ob_entity.en_flags |= M0_ENF_GEN_DI;
    //this->mobj->ob_entity.en_flags |= M0_ENF_DI;
    rc = m0_obj_op(this->mobj, M0_OC_WRITE, &ext, &buf, &attr, 0, flags, &op);
    if (rc != 0) {
      ldpp_dout(dpp, LOG_ERROR) <<__func__ << ": ERROR: write failed, m0_obj_op rc="<< rc << dendl;
      ADDB(RGW_ADDB_REQUEST_ID, addb_logger.get_id(),
           RGW_ADDB_FUNC_WRITE_MOBJ, RGW_ADDB_PHASE_ERROR);
      goto out;
    }
    ADDB(RGW_ADDB_REQUEST_TO_MOTR_ID, addb_logger.get_id(), m0_sm_id_get(&op->op_sm));
    m0_op_launch(&op, 1);
    rc = m0_op_wait(op, M0_BITS(M0_OS_FAILED, M0_OS_STABLE), M0_TIME_NEVER) ?:
         m0_rc(op);
    m0_op_fini(op);
    m0_op_free(op);
    if (rc != 0) {
      ldpp_dout(dpp, 0) <<__func__ << ": write failed, m0_op_wait rc="<< rc << dendl;
      ADDB(RGW_ADDB_REQUEST_ID, addb_logger.get_id(),
           RGW_ADDB_FUNC_WRITE_MOBJ, RGW_ADDB_PHASE_ERROR);
      goto out;
    }
  }

  ADDB(RGW_ADDB_REQUEST_ID, addb_logger.get_id(),
       RGW_ADDB_FUNC_WRITE_MOBJ, RGW_ADDB_PHASE_DONE);
out:
  m0_indexvec_free(&ext);
  m0_bufvec_free(&attr);
  m0_bufvec_free2(&buf);
  // Reset io_ctxt state
  io_ctxt.start_offset = 0;
  io_ctxt.total_bufer_sz = 0;
  return rc;
}

int MotrObject::read_mobj(const DoutPrefixProvider* dpp, int64_t start, int64_t end, RGWGetDataCB* cb)
{
  int rc;
  uint32_t flags = 0;
  unsigned bs, skip;
  int64_t left = end + 1, off;
  uint64_t req_id;
  struct m0_op *op;
  struct m0_bufvec buf;
  struct m0_bufvec attr;
  struct m0_indexvec ext;

  req_id = addb_logger.get_id();
  ADDB(RGW_ADDB_REQUEST_ID, addb_logger.get_id(),
       RGW_ADDB_FUNC_READ_MOBJ, RGW_ADDB_PHASE_START);

  ldpp_dout(dpp, 20) <<__func__ << ": start=" << start << " end=" << end << dendl;

  rc = m0_bufvec_empty_alloc(&buf, 1) ? :
       m0_bufvec_alloc(&attr, 1, 1) ? :
       m0_indexvec_alloc(&ext, 1);
  if (rc < 0) {
    ldpp_dout(dpp, LOG_ERROR) <<__func__ << ": ERROR: vecs alloc failed: rc="<< rc << dendl;
    ADDB(RGW_ADDB_REQUEST_ID, addb_logger.get_id(),
         RGW_ADDB_FUNC_READ_MOBJ, RGW_ADDB_PHASE_ERROR);
    goto out;
  }

  bs = this->get_optimal_bs(left);

  for (off = 0; left > 0; left -= bs, off += bs) {
    if (left < bs)
      bs = this->get_optimal_bs(left); // multiple of groups

    if (start >= off + bs)
      continue; // to the next block

    // At the last parity group we must read up to the last
    // object's unit and provide the M0_OOF_LAST flag, so
    // that in case of degraded read mode, libmotr could
    // know which units to use for the data recovery.
    if ((size_t)off + bs >= obj_size) {
      bs = roundup(obj_size - off, get_unit_sz());
      flags |= M0_OOF_LAST;
      ldpp_dout(dpp, 20) <<__func__ << ": off=" << off << " bs=" << bs << " obj_size=" << obj_size << dendl;
    } else if (left < bs) {
      // Somewhere in the middle of the object.
      bs = this->get_optimal_bs(left, true); // multiple of units
    }

    // Skip reading the units which are not requested.
    if (start > off) {
      skip = rounddown(start, get_unit_sz()) - off;
      off += skip;
      bs -= skip;
      left -= skip;
    }

    // Read from Motr.
    ldpp_dout(dpp, 20) <<__func__ << ": off=" << off << " bs=" << bs << dendl;
    bufferlist bl;
    buf.ov_buf[0] = bl.append_hole(bs).c_str();
    buf.ov_vec.v_count[0] = bs;
    ext.iv_index[0] = off;
    ext.iv_vec.v_count[0] = bs;
    attr.ov_vec.v_count[0] = 0;

    op = nullptr;
    this->mobj->ob_entity.en_flags |= M0_ENF_GEN_DI;
    //this->mobj->ob_entity.en_flags |= M0_ENF_DI;
    rc = m0_obj_op(this->mobj, M0_OC_READ, &ext, &buf, &attr, 0, flags, &op);
    if (rc != 0) {
      ldpp_dout(dpp, LOG_ERROR) <<__func__ << ": ERROR: motr op failed: rc=" << rc << dendl;
      ADDB(RGW_ADDB_REQUEST_ID, addb_logger.get_id(),
           RGW_ADDB_FUNC_READ_MOBJ, RGW_ADDB_PHASE_ERROR);
      goto out;
    }

    ADDB(RGW_ADDB_REQUEST_TO_MOTR_ID, addb_logger.get_id(), m0_sm_id_get(&op->op_sm));
    m0_op_launch(&op, 1);
    rc = m0_op_wait(op, M0_BITS(M0_OS_FAILED, M0_OS_STABLE), M0_TIME_NEVER) ?:
         m0_rc(op);
    m0_op_fini(op);
    m0_op_free(op);
    if (rc != 0) {
      ldpp_dout(dpp, LOG_ERROR) <<__func__ << ": ERROR: m0_op_wait failed: rc=" << rc << dendl;
      ADDB(RGW_ADDB_REQUEST_ID, addb_logger.get_id(),
           RGW_ADDB_FUNC_READ_MOBJ, RGW_ADDB_PHASE_ERROR);
      goto out;
    }

    // Call `cb` to process returned data.
    skip = 0;
    if (start > off)
      skip = start - off;
    if(cb) {
      ldpp_dout(dpp, 20) <<__func__ << ": return data, skip=" << skip
                         << " bs=" << bs << " left=" << left << dendl;
      cb->handle_data(bl, skip, (left < bs ? left : bs) - skip);
      if (rc != 0) {
        ldpp_dout(dpp, LOG_ERROR) <<__func__ << ": ERROR: handle_data failed rc=" << rc << dendl;
        goto out;
      }
    }

    addb_logger.set_id(req_id);
  }

  ADDB(RGW_ADDB_REQUEST_ID, addb_logger.get_id(),
       RGW_ADDB_FUNC_READ_MOBJ, RGW_ADDB_PHASE_DONE);
out:
  m0_indexvec_free(&ext);
  m0_bufvec_free(&attr);
  m0_bufvec_free2(&buf);
  this->close_mobj();

  return rc;
}

int MotrObject::fetch_null_obj(const DoutPrefixProvider *dpp, std::string& key, bufferlist *bl_out)
{
  int rc = 0;
  // Read the null index entry
  string tenant_bkt_name = get_bucket_name(this->get_bucket()->get_tenant(), this->get_bucket()->get_name());
  string bucket_index_iname = "motr.rgw.bucket.index." + tenant_bkt_name;
  bufferlist bl;
  rgw_bucket_dir_entry ent_null;

  key = this->get_name() + '\a';

  // Check entry in the cache
  if (this->store->get_obj_meta_cache()->get(dpp, key, bl)) {
    rc = this->store->do_idx_op_by_name(bucket_index_iname, M0_IC_GET, key, bl);
    if (rc < 0)
      return rc;

    this->store->get_obj_meta_cache()->put(dpp, key, bl);
  }

  bufferlist& blr = bl;
  auto iter = blr.cbegin();
  ent_null.decode(iter);

  if (bl_out != NULL) {
    bl_out->clear();
    bl_out->append(bl);
  }

  ldpp_dout(dpp, 20) <<__func__ << ": key="<< key <<", rc="<< rc << dendl;
  return rc;
}

std::string MotrObject::get_key_str()
{
  if (!this->get_key().have_instance() ||
       this->get_key().have_null_instance())
    return this->get_name() + '\a';
  else
    return this->get_key().name + '\a' + this->get_key().instance;
}

// Find the latest one among the two first records. Versioned records are
// ordered by mtime (latest first), null-record (if available) is always
// the first. So we should compare their mtime and return the latest one.
int MotrObject::fetch_latest_obj(const DoutPrefixProvider *dpp, bufferlist& bl_out)
{
  int max = 2;
  vector<string> keys(max);
  vector<bufferlist> vals(max);
  Bucket *bucket = this->get_bucket();
  string tenant_bkt_name = get_bucket_name(bucket->get_tenant(), bucket->get_name());
  string bucket_index_iname = "motr.rgw.bucket.index." + tenant_bkt_name;

  keys[0] = this->get_name() + '\a';
  ldpp_dout(dpp, LOG_DEBUG) <<__func__ << ": DEBUG: name=" << keys[0] << dendl;
  int rc = store->next_query_by_name(bucket_index_iname, keys, vals);
  if (rc < 0) {
    ldpp_dout(dpp, LOG_ERROR) <<__func__ << ": ERROR: NEXT query failed. rc=" << rc << dendl;
    return rc;
  }

  // no entries returned.
  if (rc == 0) {
    ldpp_dout(dpp, LOG_INFO) <<__func__ <<": INFO: No entries found" << dendl;
    return -ENOENT;
  }

  rgw_bucket_dir_entry ent, null_ent;
  for (auto&& bl: vals) {
    if (bl.length() == 0)
      break;

    auto iter = bl.cbegin();
    ent.decode(iter);
    rgw_obj_key key(ent.key);
    ldpp_dout(dpp, LOG_DEBUG) <<__func__ << ": DEBUG: key=" << key.to_str()
                       << " is_current=" << ent.is_current() << dendl;
    if (key.name != this->get_name())
      break;

    if (null_ent.key.empty())
      null_ent = ent;
    else if (null_ent.meta.mtime > ent.meta.mtime)
      break;

    bl_out = bl;
  }

  return bl_out.length() == 0 ? -ENOENT : 0;
}

int MotrObject::get_bucket_dir_ent(const DoutPrefixProvider *dpp, rgw_bucket_dir_entry& ent)
{
  int rc = 0;
  Bucket *bucket = this->get_bucket();
  string tenant_bkt_name = get_bucket_name(bucket->get_tenant(), bucket->get_name());
  string bucket_index_iname = "motr.rgw.bucket.index." + tenant_bkt_name;
  bufferlist bl;
  bufferlist::const_iterator iter;
  rgw_obj_key key;
  std::string obj_key = this->get_key_str();

  if (this->have_instance()) {
    // Check entry in the cache
    if (this->store->get_obj_meta_cache()->get(dpp, obj_key, bl) == 0) {
      bufferlist& blr = bl;
      iter = blr.cbegin();
      ent.decode(iter);
      goto out;
    }
    // Cache miss.
    rc = this->store->do_idx_op_by_name(bucket_index_iname, M0_IC_GET, obj_key, bl);
    if (rc < 0) {
      ldpp_dout(dpp, LOG_ERROR) <<__func__ << ": ERROR: do_idx_op_by_name failed to get object's entry: rc="
                        << rc << dendl;
      return rc;
    }
  } else {
    rc = this->fetch_latest_obj(dpp, bl);
    if (rc < 0) {
      ldpp_dout(dpp, LOG_ERROR) <<__func__ << ": ERROR: fetch_latest_obj() failed, rc=" << rc << dendl;
      return rc;
    }
  }

  iter = bl.cbegin();
  ent.decode(iter);
  key.set(ent.key);
  obj_key = key.name + '\a' + key.instance;

  // Set the instance value as "null" to show
  // the VersionId field in the GET/HEAD object response
  if (this->get_key().have_null_instance())
    ent.key.instance = "null";

  // Put into the cache
  this->store->get_obj_meta_cache()->put(dpp, obj_key, bl);

out:
  if (rc == 0) {
    decode(attrs, iter);
    meta.decode(iter);
    ldpp_dout(dpp, 20) <<__func__ << ": key=" << obj_key
                       << " lid=0x" << std::hex << meta.layout_id << dendl;
    char fid_str[M0_FID_STR_LEN];
    snprintf(fid_str, ARRAY_SIZE(fid_str), U128X_F, U128_P(&meta.oid));
    ldpp_dout(dpp, 70) <<__func__ << ": oid=" << fid_str << dendl;
  } else
    ldpp_dout(dpp, 0) <<__func__ << ": rc=" << rc << dendl;

  return rc;
}

int MotrObject::update_version_entries(const DoutPrefixProvider *dpp, bool set_is_latest)
{
  Bucket *bucket = this->get_bucket();
  string tenant_bkt_name = get_bucket_name(bucket->get_tenant(), bucket->get_name());
  string bucket_index_iname = "motr.rgw.bucket.index." + tenant_bkt_name;

  ldpp_dout(dpp, 20) <<__func__ << ": name=" << this->get_name()
                     << " set_is_latest=" << set_is_latest << dendl;
  rgw_bucket_dir_entry ent;
  bufferlist bl;

  int rc = this->fetch_latest_obj(dpp, bl);
  // no entries returned.
  if (rc == -ENOENT) {
    ldpp_dout(dpp, LOG_ERROR) <<__func__ << ": ERROR: No entries found" << dendl;
    return 0;
  }
  if (rc < 0) {
    ldpp_dout(dpp, LOG_ERROR) <<__func__ << ": ERROR: fetch_latest_obj() failed, rc=" << rc << dendl;
    return rc;
  }

  rgw::sal::Attrs attrs;
  MotrObjectMeta meta;

  auto iter = bl.cbegin();
  ent.decode(iter);
  decode(attrs, iter);
  meta.decode(iter);

  // In case of (delete-object flow) we are setting set_is_latest=true,
  // to set is-latest flag for the previous latest version.
  // In case of (put-object flow) set_is_latest is false (default value),
  // to unset is-latest flag for the previous latest version.
  if (!ent.is_current() && !set_is_latest)
    return 0; // nothing to unset, it's already not latest

  if (set_is_latest) {
    // delete-object flow
    // set is-latest=true for delete-marker/ normal object.
    if (ent.is_delete_marker())
      ent.flags = rgw_bucket_dir_entry::FLAG_DELETE_MARKER;
    else
      ent.flags = rgw_bucket_dir_entry::FLAG_VER | rgw_bucket_dir_entry::FLAG_CURRENT;
  } else {
    // put-object flow, set is-latest=false for delete-marker/ normal object.
    if (ent.is_delete_marker())
      ent.flags = rgw_bucket_dir_entry::FLAG_DELETE_MARKER | rgw_bucket_dir_entry::FLAG_VER;
    else
      ent.flags = rgw_bucket_dir_entry::FLAG_VER;
  }
  rgw_obj_key objkey(ent.key);
  string key = objkey.name + '\a' + objkey.instance;

  // Remove from the cache.
  store->get_obj_meta_cache()->remove(dpp, key);

  ldpp_dout(dpp, 20) <<__func__ << ": update key=" << key << dendl;
  bufferlist ent_bl;
  ent.encode(ent_bl);
  encode(attrs, ent_bl);
  meta.encode(ent_bl);

  return store->do_idx_op_by_name(bucket_index_iname, M0_IC_PUT, key, ent_bl);
}

// Scan object_nnn_part_index to get all parts then open their motr objects.
// TODO: all parts are opened in the POC. But for a large object, for example
// a 5GB object will have about 300 parts (for default 15MB part). A better
// way of managing opened object may be needed.
int MotrObject::get_part_objs(const DoutPrefixProvider* dpp,
                              std::map<int, std::unique_ptr<MotrObject>>& part_objs)
{
  int rc;
  int max_parts = 1000;
  int marker = 0;
  uint64_t off = 0;
  bool truncated = false;
  std::unique_ptr<rgw::sal::MultipartUpload> upload;

  string tenant_bkt_name = get_bucket_name(bucket->get_tenant(), bucket->get_name());
  string upload_id;
  rc = store->get_upload_id(tenant_bkt_name, this->get_key_str(), upload_id);
  if (rc < 0) {
    ldpp_dout(dpp, LOG_ERROR) <<__func__ << ": ERROR: get_upload_id failed. rc=" << rc << dendl;
    return rc;
  }

  upload = this->get_bucket()->get_multipart_upload(this->get_name(), upload_id);

  do {
    rc = upload->list_parts(dpp, store->ctx(), max_parts, marker, &marker, &truncated);
    if (rc == -ENOENT) {
      rc = -ERR_NO_SUCH_UPLOAD;
    }
    if (rc < 0)
      return rc;

    std::map<uint32_t, std::unique_ptr<MultipartPart>>& parts = upload->get_parts();
    for (auto part_iter = parts.begin(); part_iter != parts.end(); ++part_iter) {

      MultipartPart *mpart = part_iter->second.get();
      MotrMultipartPart *mmpart = static_cast<MotrMultipartPart *>(mpart);
      uint32_t part_num = mmpart->get_num();
      uint64_t part_size = mmpart->get_size();

      string part_obj_name = this->get_bucket()->get_name() + "." +
 	                     this->get_key().to_str() +
	                     ".part." + std::to_string(part_num);
      std::unique_ptr<rgw::sal::Object> obj;
      obj = this->bucket->get_object(rgw_obj_key(part_obj_name));
      std::unique_ptr<rgw::sal::MotrObject> mobj(static_cast<rgw::sal::MotrObject *>(obj.release()));

      ldpp_dout(dpp, 20) <<__func__ << ": off=" << off << ", size=" << part_size << dendl;
      mobj->part_off = off;
      mobj->part_size = part_size;
      mobj->set_obj_size(part_size);
      mobj->part_num = part_num;
      mobj->meta = mmpart->meta;

      part_objs.emplace(part_num, std::move(mobj));

      off += part_size;
    }
  } while (truncated);

  return 0;
}

int MotrObject::open_part_objs(const DoutPrefixProvider* dpp,
                               std::map<int, std::unique_ptr<MotrObject>>& part_objs)
{
  //for (auto& iter: part_objs) {
  for (auto iter = part_objs.begin(); iter != part_objs.end(); ++iter) {
    MotrObject* obj = static_cast<MotrObject *>(iter->second.get());
    ldpp_dout(dpp, 20) <<__func__ << ": name=" << obj->get_name() << dendl;
    int rc = obj->open_mobj(dpp);
    if (rc < 0)
      return rc;
  }

  return 0;
}

int MotrObject::delete_part_objs(const DoutPrefixProvider* dpp,
                                 uint64_t* size_rounded) {
  string version_id = this->get_instance();
  std::unique_ptr<rgw::sal::MultipartUpload> upload;
  upload = this->get_bucket()->get_multipart_upload(this->get_name(), string());
  std::unique_ptr<rgw::sal::MotrMultipartUpload> mupload(static_cast<rgw::sal::MotrMultipartUpload *>(upload.release()));
  return mupload->delete_parts(dpp, version_id, size_rounded);
}

int MotrObject::read_multipart_obj(const DoutPrefixProvider* dpp,
                                   int64_t off, int64_t end, RGWGetDataCB* cb,
				   std::map<int, std::unique_ptr<MotrObject>>& part_objs)
{
  int64_t cursor = off;

  ldpp_dout(dpp, 20) <<__func__ << ": off=" << off << " end=" << end << dendl;

  // Find the parts which are in the (off, end) range and
  // read data from it. Note: `end` argument is inclusive.
  for (auto iter = part_objs.begin(); iter != part_objs.end(); ++iter) {
    MotrObject* obj = static_cast<MotrObject *>(iter->second.get());
    int64_t part_off = obj->part_off;
    int64_t part_size = obj->part_size;
    int64_t part_end = obj->part_off + obj->part_size - 1;
    ldpp_dout(dpp, 20) <<__func__ << ": part_off=" << part_off
                                          << " part_end=" << part_end << dendl;
    if (part_end < off)
      continue;

    int64_t local_off = cursor - obj->part_off;
    int64_t local_end = part_end < end? part_size - 1 : end - part_off;
    ldpp_dout(dpp, 20) <<__func__ << ": name=" << obj->get_name()
                                          << " local_off=" << local_off
                                          << " local_end=" << local_end << dendl;
    int rc = obj->read_mobj(dpp, local_off, local_end, cb);
    if (rc < 0)
        return rc;

    cursor = part_end + 1;
    if (cursor > end)
      break;
  }

  return 0;
}

unsigned MotrObject::get_unit_sz()
{
  uint64_t lid = M0_OBJ_LAYOUT_ID(meta.layout_id);
  return m0_obj_layout_id_to_unit_size(lid);
}

// The optimal bs will be rounded up to the unit size, if last is true,
// so use M0_OOF_LAST flag to avoid RMW for the last block.
// Otherwise, bs will be rounded up to the group size.
unsigned MotrObject::get_optimal_bs(unsigned len, bool last)
{
  struct m0_pool_version *pver;

  pver = m0_pool_version_find(&store->instance->m0c_pools_common,
                              &mobj->ob_attr.oa_pver);
  M0_ASSERT(pver != nullptr);
  struct m0_pdclust_attr *pa = &pver->pv_attr;
  unsigned unit_sz = get_unit_sz();
  unsigned grp_sz  = unit_sz * pa->pa_N;

  // bs should be max 4-times pool-width deep counting by 1MB units, or
  // 8-times deep counting by 512K units, 16-times deep by 256K units,
  // and so on. Several units to one target will be aggregated to make
  // fewer network RPCs, disk i/o operations and BE transactions.
  // For unit sizes of 32K or less, the depth is 128, which
  // makes it 32K * 128 == 4MB - the maximum amount per target when
  // the performance is still good on LNet (which has max 1MB frames).
  // TODO: it may be different on libfabric, should be re-measured.
  unsigned depth = 128 / ((unit_sz + 0x7fff) / 0x8000);
  if (depth == 0)
    depth = 1;
  // P * N / (N + K + S) - number of data units to span the pool-width
  unsigned max_bs = depth * unit_sz * pa->pa_P * pa->pa_N /
                                     (pa->pa_N + pa->pa_K + pa->pa_S);
  max_bs = roundup(max_bs, grp_sz); // multiple of group size
  if (len >= max_bs)
    return max_bs;
  else if (last)
    return roundup(len, unit_sz);
  else
    return roundup(len, grp_sz);
}

void MotrAtomicWriter::cleanup()
{
  m0_indexvec_free(&ext);
  m0_bufvec_free(&attr);
  m0_bufvec_free2(&buf);
  acc_data.clear();
  obj.close_mobj();
}

unsigned MotrAtomicWriter::populate_bvec(unsigned len, bufferlist::iterator &bi)
{
  unsigned i, l, done = 0;
  const char *data;

  for (i = 0; i < MAX_BUFVEC_NR && len > 0; ++i) {
    l = bi.get_ptr_and_advance(len, &data);
    buf.ov_buf[i] = (char*)data;
    buf.ov_vec.v_count[i] = l;
    ext.iv_index[i] = acc_off;
    ext.iv_vec.v_count[i] = l;
    attr.ov_vec.v_count[i] = 0;
    acc_off += l;
    len -= l;
    done += l;
  }
  buf.ov_vec.v_nr = i;
  ext.iv_vec.v_nr = i;

  return done;
}

int MotrAtomicWriter::write(bool last)
{
  int rc;
  uint32_t flags = M0_OOF_FULL;
  int64_t bs, done, left;
  struct m0_op *op;
  bufferlist::iterator bi;

  left = acc_data.length();

  addb_logger.set_id(req_id);

  ADDB(RGW_ADDB_REQUEST_ID, addb_logger.get_id(),
       RGW_ADDB_FUNC_WRITE, RGW_ADDB_PHASE_START);

  if (!obj.is_opened()) {
    // Create a composite object is HSM_ENABLED flag is set.
    if (store->hsm_enabled)
      rc = obj.create_hsm_enabled_mobj(dpp, left);
    else
      rc = obj.create_mobj(dpp, left);
    if (rc == -EEXIST)
      rc = obj.open_mobj(dpp);
    if (rc != 0) {
      char fid_str[M0_FID_STR_LEN];
      snprintf(fid_str, ARRAY_SIZE(fid_str), U128X_F, U128_P(&obj.meta.oid));
      ldpp_dout(dpp, LOG_ERROR) <<__func__ << ": ERROR: failed to create/open motr object "
                        << fid_str << " (" << obj.get_bucket()->get_name()
                        << "/" << obj.get_key().to_str() << "): rc=" << rc
                        << dendl;
      goto err;
    }
  }

  bs = obj.get_optimal_bs(left, last);
  ldpp_dout(dpp, 20) <<__func__ << ": left=" << left << " bs=" << bs
                               << " last=" << last << dendl;
  bi = acc_data.begin();
  while (left > 0) {
    if (left < bs) {
      if (!last)
        break; // accumulate more data
      bs = obj.get_optimal_bs(left, last);
    }
    if (left < bs) { // align data to unit-size
      ldpp_dout(dpp, 20) <<__func__ << " Padding [" << (bs - left) << "] bytes" << dendl;
      acc_data.append_zero(bs - left);
      auto off = bi.get_off();
      bufferlist tmp;
      acc_data.splice(off, bs, &tmp);
      acc_data.clear();
      acc_data.append(tmp.c_str(), bs); // make it a single buf
      bi = acc_data.begin();
    }
    ldpp_dout(dpp, 20) <<__func__ << ": left=" << left << " bs=" << bs << dendl;
    done = this->populate_bvec(bs, bi);

    if (last)
      flags |= M0_OOF_LAST;

    op = nullptr;
    obj.mobj->ob_entity.en_flags |= M0_ENF_GEN_DI;
    rc = m0_obj_op(obj.mobj, M0_OC_WRITE, &ext, &buf, &attr, 0, flags, &op);
    if (rc != 0) {
      ldpp_dout(dpp, LOG_ERROR) <<__func__ << ": ERROR: write failed, m0_obj_op rc="<< rc << dendl;
      ADDB(RGW_ADDB_REQUEST_ID, addb_logger.get_id(),
           RGW_ADDB_FUNC_WRITE, RGW_ADDB_PHASE_ERROR);
      goto err;
    }

    ADDB(RGW_ADDB_REQUEST_TO_MOTR_ID, addb_logger.get_id(), m0_sm_id_get(&op->op_sm));

    m0_op_launch(&op, 1);
    rc = m0_op_wait(op, M0_BITS(M0_OS_FAILED, M0_OS_STABLE), M0_TIME_NEVER) ?:
         m0_rc(op);
    m0_op_fini(op);
    m0_op_free(op);
    if (rc != 0) {
      ldpp_dout(dpp, LOG_ERROR) <<__func__ << ": ERROR: write failed, m0_op_wait rc="<< rc << dendl;
      goto err;
    }

    total_data_size += left < done ? left : done;
    left            -= left < done ? left : done;
  }

  if (last) {
    acc_data.clear();
  } else if (bi.get_remaining() < acc_data.length()) {
    // Clear from the accumulator what has been written already.
    // XXX Optimise this, if possible, to avoid copying.
    ldpp_dout(dpp, 0) <<__func__ << ": cleanup "<< acc_data.length() -
                                                  bi.get_remaining()
                                << " bytes from the accumulator" << dendl;
    bufferlist tmp;
    bi.copy(bi.get_remaining(), tmp);
    acc_data.clear();
    acc_data.append(std::move(tmp));
  }

  ADDB(RGW_ADDB_REQUEST_ID, addb_logger.get_id(),
       RGW_ADDB_FUNC_WRITE, RGW_ADDB_PHASE_DONE);
  return 0;

err:
  ADDB(RGW_ADDB_REQUEST_ID, addb_logger.get_id(),
       RGW_ADDB_FUNC_WRITE, RGW_ADDB_PHASE_ERROR);
  this->cleanup();
  return rc;
}

// Accumulate enough data first to make a reasonable decision about the
// optimal unit size for a new object, or bs for existing object (32M seems
// enough for 4M units in 8+2 parity groups, a common config on wide pools),
// and then launch the write operations.
int MotrAtomicWriter::process(bufferlist&& data, uint64_t offset)
{
  if (data.length() == 0) { // last call, flush data
    int rc = 0;
    if (acc_data.length() != 0)
      rc = this->write(true);
    this->cleanup();
    return rc;
  }

  if (acc_data.length() == 0)
    acc_off = offset;

  acc_data.append(std::move(data));
  if (acc_data.length() < MAX_ACC_SIZE)
    return 0;

  return this->write();
}

int MotrObject::remove_null_obj(const DoutPrefixProvider *dpp)
{
  int rc;
  string tenant_bkt_name = get_bucket_name(this->get_bucket()->get_tenant(), this->get_bucket()->get_name());
  string bucket_index_iname = "motr.rgw.bucket.index." + tenant_bkt_name;
  std::string obj_type = "simple object";
  rgw_bucket_dir_entry old_ent;
  bufferlist old_check_bl;
  std::string null_obj_key;

  rc = this->fetch_null_obj(dpp, null_obj_key, &old_check_bl);
  if (rc == -ENOENT) {
    ldpp_dout(dpp, 0) <<__func__ << ": Nothing to remove" << dendl;
    return 0;
  }
  if (rc < 0) {
    ldpp_dout(dpp, LOG_ERROR) <<__func__ << ": ERROR: Failed to fetch null reference key, rc=" << rc << dendl;
    return rc;
  }

  if (old_check_bl.length() > 0) {
    auto ent_iter = old_check_bl.cbegin();
    old_ent.decode(ent_iter);
    rgw::sal::Attrs attrs;
    decode(attrs, ent_iter);
    this->meta.decode(ent_iter);
    this->set_instance(std::move(old_ent.key.instance));
    if (old_ent.meta.category == RGWObjCategory::MultiMeta)
      obj_type = "multipart object";
    ldpp_dout(dpp, 20) <<__func__ << ": Old " << obj_type << " exists" << dendl;
    rc = this->remove_mobj_and_index_entry(dpp, old_ent, null_obj_key,
                                           bucket_index_iname, tenant_bkt_name);
    if (rc == 0) {
      ldpp_dout(dpp, 20) <<__func__ << ": Old " << obj_type << " ["
        << this->get_name() <<  "] deleted succesfully" << dendl;
    } else {
      ldpp_dout(dpp, LOG_ERROR) << __func__ <<": ERROR: Failed to delete old " << obj_type << " ["
        << this->get_name() <<  "]. Error=" << rc << dendl;
      // TODO: This will be handled during GC
    }
  }

  return rc;
}

int MotrAtomicWriter::complete(size_t accounted_size, const std::string& etag,
                       ceph::real_time *mtime, ceph::real_time set_mtime,
                       std::map<std::string, bufferlist>& attrs,
                       ceph::real_time delete_at,
                       const char *if_match, const char *if_nomatch,
                       const std::string *user_data,
                       rgw_zone_set *zones_trace, bool *canceled,
                       optional_yield y)
{
  int rc = 0;

  addb_logger.set_id(req_id);

  if (acc_data.length() != 0) { // check again, just in case
    rc = this->write(true);
    this->cleanup();
    if (rc != 0)
      return rc;
  }

  bufferlist bl;
  rgw_bucket_dir_entry ent;

  // Set rgw_bucket_dir_entry. Some of the member of this structure may not
  // apply to motr. For example the storage_class.
  //
  // Checkout AtomicObjectProcessor::complete() in rgw_putobj_processor.cc
  // and RGWRados::Object::Write::write_meta() in rgw_rados.cc for what and
  // how to set the dir entry. Only set the basic ones for POC, no ACLs and
  // other attrs.
  obj.get_key().get_index_key(&ent.key);
  ent.meta.size = total_data_size;
  ent.meta.accounted_size = total_data_size;
  ent.meta.mtime = real_clock::is_zero(set_mtime)? ceph::real_clock::now() : set_mtime;
  ent.meta.etag = etag;
  ent.meta.owner = owner.to_str();
  ent.meta.owner_display_name = obj.get_bucket()->get_owner()->get_display_name();
  uint64_t size_rounded = 0;
  // For 0kb Object layout_id will not be available.
  if (ent.meta.size != 0)
    size_rounded = roundup(ent.meta.size, obj.get_unit_sz());

  RGWBucketInfo &info = obj.get_bucket()->get_info();

  // Set version and current flag in case of both versioning enabled and suspended case.
  if (info.versioned())
    ent.flags = rgw_bucket_dir_entry::FLAG_VER | rgw_bucket_dir_entry::FLAG_CURRENT;

  ldpp_dout(dpp, 20) <<__func__ << ": key=" << obj.get_key().to_str() << ", meta:oid=[0x" << std::hex << obj.meta.oid.u_hi
                                 << ":0x" << obj.meta.oid.u_lo << "], meta:pvid=[0x" << std::hex << obj.meta.pver.f_container
                                 << ":0x" << obj.meta.pver.f_key << "], meta:layout_id=0x" << std::hex << obj.meta.layout_id
                                 << " etag=" << etag << " user_data=" << user_data << dendl;
  if (user_data)
    ent.meta.user_data = *user_data;

  ent.encode(bl);

  if (info.obj_lock_enabled() && info.obj_lock.has_rule()) {
    auto iter = attrs.find(RGW_ATTR_OBJECT_RETENTION);
    if (iter == attrs.end()) {
      real_time lock_until_date = info.obj_lock.get_lock_until_date(ent.meta.mtime);
      string mode = info.obj_lock.get_mode();
      RGWObjectRetention obj_retention(mode, lock_until_date);
      bufferlist retention_bl;
      obj_retention.encode(retention_bl);
      attrs[RGW_ATTR_OBJECT_RETENTION] = retention_bl;
    }
  }
  encode(attrs, bl);
  obj.meta.encode(bl);

  // Update existing object version entries in a bucket,
  // in case of both versioning enabled and suspended.
  if (info.versioned()) {
    // get the list of all versioned objects with the same key and
    // unset their FLAG_CURRENT later, if do_idx_op_by_name() is successful.
    // Note: without distributed lock on the index - it is possible that 2
    // CURRENT entries would appear in the bucket. For example, consider the
    // following scenario when two clients are trying to add the new object
    // version concurrently:
    //   client 1: reads all the CURRENT entries
    //   client 2: updates the index and sets the new CURRENT
    //   client 1: updates the index and sets the new CURRENT
    // At the step (1) client 1 would not see the new current record from step (2),
    // so it won't update it. As a result, two CURRENT version entries will appear
    // in the bucket.
    // TODO: update the current version (unset the flag) and insert the new current
    // version can be launched in one motr op. This requires change at do_idx_op()
    // and do_idx_op_by_name().
    rc = obj.update_version_entries(dpp);
    if (rc < 0)
      return rc;
  }

  string tenant_bkt_name = get_bucket_name(obj.get_bucket()->get_tenant(), obj.get_bucket()->get_name());
  // Insert an entry into bucket index.
  string bucket_index_iname = "motr.rgw.bucket.index." + tenant_bkt_name;

  if (!info.versioning_enabled()) {
    std::unique_ptr<rgw::sal::Object> old_obj = obj.get_bucket()->get_object(rgw_obj_key(obj.get_name()));
    rgw::sal::MotrObject *old_mobj = static_cast<rgw::sal::MotrObject *>(old_obj.get());
    rc = old_mobj->remove_null_obj(dpp);
    if (rc < 0) {
      ldpp_dout(dpp, LOG_ERROR) <<__func__ << ": ERROR: Failed to overwrite null object, rc : " << rc << dendl;
      return rc;
    }
  }

  string obj_key = obj.get_key_str();
  rc = store->do_idx_op_by_name(bucket_index_iname, M0_IC_PUT, obj_key, bl);
  if (rc != 0) {
    // TODO: handle this object leak via gc.
    ldpp_dout(dpp, LOG_ERROR) <<__func__ << ": ERROR: index operation failed, rc="<< rc << dendl;
    return rc;
  }
  store->get_obj_meta_cache()->put(dpp, obj_key, bl);

  // Add object size and count in bucket stats entry.
  rc = update_bucket_stats(dpp, store, owner.to_str(), tenant_bkt_name,
                           total_data_size, size_rounded);
  if (rc != 0) {
    ldpp_dout(dpp, LOG_ERROR) <<__func__ << ": ERROR: Failed stats additon for the bucket/obj = "
      << tenant_bkt_name << "/" << obj.get_name() << ", rc=" << rc << dendl;
    return rc;
  }
  ldpp_dout(dpp, 70) <<__func__ << ": Stats added successfully for the bucket/obj = "
    << tenant_bkt_name << "/" << obj.get_name() << ", rc=" << rc << dendl;

  // TODO: We need to handle the object leak caused by parallel object upload by
  // making use of background gc, which is currently not enabled for motr.
  return rc;
}

int MotrMultipartUpload::delete_parts(const DoutPrefixProvider *dpp, std::string version_id, uint64_t* size_rounded)
{
  int rc;
  int max_parts = 1000;
  uint64_t total_size = 0, total_size_rounded = 0;
  int marker = 0;
  bool truncated = false;

  this->set_version_id(version_id);
  // Scan all parts and delete the corresponding motr objects.
  do {
    rc = this->list_parts(dpp, store->ctx(), max_parts, marker, &marker, &truncated);
    if (rc == -ENOENT) {
      truncated = false;
      rc = 0;
    }
    if (rc < 0)
      return rc;

    std::map<uint32_t, std::unique_ptr<MultipartPart>>& parts = this->get_parts();
    for (auto part_iter = parts.begin(); part_iter != parts.end(); ++part_iter) {

      MultipartPart *mpart = part_iter->second.get();
      MotrMultipartPart *mmpart = static_cast<MotrMultipartPart *>(mpart);
      uint32_t part_num = mmpart->get_num();
      total_size += mmpart->get_size();
      total_size_rounded += mmpart->get_size_rounded();

      // If it is a composite object, as no part objects are created,
      // no need to delete part objects.
      if (this->hsm_enabled)
        continue;

      // Delete the part object. Note that the part object is  not
      // inserted into bucket index, only the corresponding motr object
      // needs to be delete. That is why we don't call
      // MotrObject::delete_object().
      string part_obj_name = bucket->get_name() + "." +
 	                     mp_obj.get_key() +
	                     ".part." + std::to_string(part_num);
      std::unique_ptr<rgw::sal::Object> obj;
      obj = this->bucket->get_object(rgw_obj_key(part_obj_name));
      std::unique_ptr<rgw::sal::MotrObject> mobj(static_cast<rgw::sal::MotrObject *>(obj.release()));
      mobj->meta = mmpart->meta;
      rc = mobj->delete_mobj(dpp);
      if (rc < 0) {
        ldpp_dout(dpp, LOG_ERROR) <<__func__ << ": ERROR: failed to delete object from Motr. rc=" << rc << dendl;
        return rc;
      }
    }
  } while (truncated);

  string tenant_bkt_name = get_bucket_name(bucket->get_tenant(), bucket->get_name());
  string upload_id = get_upload_id();
  string key_name;

  if (upload_id.length() == 0) {
    key_name = this->get_key() + '\a';
    if (version_id != "null")
      key_name += version_id;
    rc = store->get_upload_id(tenant_bkt_name, key_name, upload_id);
    if (rc < 0) {
      ldpp_dout(dpp, LOG_ERROR) <<__func__ << ": ERROR: get_upload_id failed. rc=" << rc << dendl;
      return rc;
    }
  }
  if (size_rounded != nullptr)
    *size_rounded = total_size_rounded;

  if (get_upload_id().length()) {
    // Subtract size & object count if multipart is not completed.
    rc = update_bucket_stats(dpp, store,
                             bucket->get_acl_owner().get_id().to_str(), tenant_bkt_name,
                             total_size, total_size_rounded, 1, false);
    if (rc != 0) {
      ldpp_dout(dpp, LOG_ERROR) <<__func__ << ": ERROR: Failed stats substraction for the "
        << "bucket/obj=" << tenant_bkt_name << "/" << mp_obj.get_key()
        << ", rc=" << rc << dendl;
      return rc;
    }
    ldpp_dout(dpp, 70) <<__func__ << ": Stats subtracted successfully for the "
        << "bucket/obj=" << tenant_bkt_name << "/" << mp_obj.get_key()
        << ", rc=" << rc << dendl;
  }

  return rc;
}

int MotrMultipartUpload::abort(const DoutPrefixProvider *dpp, CephContext *cct,
                                RGWObjectCtx *obj_ctx)
{
  int rc;
  // Check if multipart upload exists
  bufferlist bl;
  std::unique_ptr<rgw::sal::Object> meta_obj;
  meta_obj = get_meta_obj();
  string tenant_bkt_name = get_bucket_name(meta_obj->get_bucket()->get_tenant(), meta_obj->get_bucket()->get_name());
  string bucket_multipart_iname =
      "motr.rgw.bucket." + tenant_bkt_name + ".multiparts.in-progress";
  rc = store->do_idx_op_by_name(bucket_multipart_iname,
                                  M0_IC_GET, meta_obj->get_key().to_str(), bl);
  if (rc < 0) {
    ldpp_dout(dpp, LOG_ERROR) <<__func__ << ": ERROR: failed to get multipart upload. rc=" << rc << dendl;
    return rc == -ENOENT ? -ERR_NO_SUCH_UPLOAD : rc;
  }

  // Scan all parts and delete the corresponding motr objects.
  rc = this->delete_parts(dpp);
  if (rc < 0)
    return rc;

  bl.clear();
  // Remove the upload from bucket multipart index.
  rc = store->do_idx_op_by_name(bucket_multipart_iname,
                                  M0_IC_DEL, meta_obj->get_key().to_str(), bl);
  if (rc != 0) {
    ldpp_dout(dpp, LOG_WARNING) <<__func__ << ": WARNING: index opration failed, M0_IC_DEL rc="<< rc << dendl;
  }
  return rc;
}

std::unique_ptr<rgw::sal::Object> MotrMultipartUpload::get_meta_obj()
{
  std::unique_ptr<rgw::sal::Object> obj = bucket->get_object(rgw_obj_key(get_meta(), string(), mp_ns));
  std::unique_ptr<rgw::sal::MotrObject> mobj(static_cast<rgw::sal::MotrObject *>(obj.release()));
  mobj->set_category(RGWObjCategory::MultiMeta);
  return mobj;
}

struct motr_multipart_upload_info
{
  rgw_placement_rule dest_placement;
  string upload_id;

  // A multipart upload is broken into initialisation, write
  // and completion phases, These phases are done in
  // separate s3 requests. And after a multipart upload is
  // initialised, an upload may break and then resumes.
  // Becasue of there RGW hold different in-memory
  // MotrMultipartUpload instances at different points of
  // time. To pass correct upload info among instances,
  // the info is stored as an entry in a Motr index.
  //
  // When HSM flag is set to true, a composite object is
  // created to store data instead of multiple 'part' objects.
  // OID and other metadata of this composite object are
  // needed for part writes, bucket entry update when
  // a upload is completed and other multipart upload ops
  // such as abort etc..
  //
  bool hsm_enabled = false;
  MotrObjectMeta meta;

  void encode(bufferlist& bl) const {
    ENCODE_START(1, 1, bl);
    encode(dest_placement, bl);
    encode(upload_id, bl);
    encode(hsm_enabled, bl);
    meta.encode(bl);
    ENCODE_FINISH(bl);
  }

  void decode(bufferlist::const_iterator& bl) {
    DECODE_START(1, bl);
    decode(dest_placement, bl);
    decode(upload_id, bl);
    decode(hsm_enabled, bl);
    meta.decode(bl);
    DECODE_FINISH(bl);
  }
};
WRITE_CLASS_ENCODER(motr_multipart_upload_info)

int MotrMultipartUpload::init(const DoutPrefixProvider *dpp, optional_yield y,
                              RGWObjectCtx* obj_ctx, ACLOwner& _owner,
			      rgw_placement_rule& dest_placement, rgw::sal::Attrs& attrs)
{
  int rc;
  std::string oid = mp_obj.get_key();

  owner = _owner;
  hsm_enabled = store->hsm_enabled;

  string tenant_bkt_name = get_bucket_name(bucket->get_tenant(), bucket->get_name());
  do {
    char buf[33];
    string tmp_obj_name;
    gen_rand_alphanumeric(store->ctx(), buf, sizeof(buf) - 1);
    std::string upload_id = MULTIPART_UPLOAD_ID_PREFIX; /* v2 upload id */
    upload_id.append(buf);

    mp_obj.init(oid, upload_id);
    tmp_obj_name = mp_obj.get_meta();

    std::unique_ptr<rgw::sal::Object> sal_obj;
    sal_obj = bucket->get_object(rgw_obj_key(tmp_obj_name, string(), mp_ns));
    std::unique_ptr<rgw::sal::MotrObject> obj(static_cast<rgw::sal::MotrObject *>(sal_obj.release()));
    // the meta object will be indexed with 0 size, we c
    obj->set_in_extra_data(true);
    obj->set_hash_source(oid);

    // The composite object is created in multipart upload initialisation
    // phase so the later multipart upload operations (part writes etc.)
    // can get its meta from upload info.
    if (store->hsm_enabled) {
      rc = obj->create_hsm_enabled_mobj(dpp, MAX_ACC_SIZE);
      if (rc < 0) {
        ldpp_dout(dpp, 20) <<__func__ << ": failed to create a composite object " << dendl;
        return rc;
      }
    }

    motr_multipart_upload_info upload_info;
    upload_info.dest_placement = dest_placement;
    upload_info.upload_id = upload_id;
    upload_info.hsm_enabled = store->hsm_enabled;
    upload_info.meta = obj->meta;
    bufferlist mpbl;
    encode(upload_info, mpbl);

    // Create an initial entry in the bucket. The entry will be
    // updated when multipart upload is completed, for example,
    // size, etag etc.
    bufferlist bl;
    rgw_bucket_dir_entry ent;
    obj->get_key().get_index_key(&ent.key);
    ent.meta.owner = owner.get_id().to_str();
    ent.meta.category = RGWObjCategory::MultiMeta;
    ent.meta.mtime = ceph::real_clock::now();
    ent.meta.user_data.assign(mpbl.c_str(), mpbl.c_str() + mpbl.length());
    ent.encode(bl);
    req_state *s = (req_state *) obj_ctx->get_private();
    bufferlist tags_bl;
    ldpp_dout(dpp, 20) <<__func__ << ": Parse tag values for object: " << obj->get_key().to_str() << dendl;
    int r = parse_tags(dpp, tags_bl, s);
    if (r < 0) {
      ldpp_dout(dpp, LOG_ERROR) <<__func__ << ": ERROR: Parsing object tags failed rc=" << r << dendl;
      return r;
    }
    attrs[RGW_ATTR_TAGS] = tags_bl;
    encode(attrs, bl);
    // Insert an entry into bucket multipart index so it is not shown
    // when listing a bucket.
    string bucket_multipart_iname =
      "motr.rgw.bucket." + tenant_bkt_name + ".multiparts.in-progress";
    rc = store->do_idx_op_by_name(bucket_multipart_iname,
                                  M0_IC_PUT, obj->get_key().to_str(), bl);
  } while (rc == -EEXIST);

  if (rc < 0) {
    ldpp_dout(dpp, LOG_ERROR) <<__func__ << ": ERROR: index opration failed, M0_IC_PUT rc="<< rc << dendl;
    return rc;
  }

  // Add one to the object_count of the current bucket stats
  // Size will be added when parts are uploaded
  rc = update_bucket_stats(dpp, store, owner.get_id().to_str(), tenant_bkt_name, 0, 0, 1, true);
  if (rc != 0) {
    ldpp_dout(dpp, LOG_ERROR) <<__func__ << ": ERROR: Failed to update object count for the "
      << "bucket/obj=" << tenant_bkt_name << "/" << mp_obj.get_key()
      << ", rc=" << rc << dendl;
  }
  return rc;
}

int MotrMultipartUpload::list_parts(const DoutPrefixProvider *dpp, CephContext *cct,
				     int num_parts, int marker,
				     int *next_marker, bool *truncated,
				     bool assume_unsorted)
{
  int rc = 0;
  if (num_parts <= 0 or marker < 0)
    return rc;

  vector<string> key_vec(num_parts);
  vector<bufferlist> val_vec(num_parts);

  string tenant_bkt_name = get_bucket_name(bucket->get_tenant(), bucket->get_name());
  string upload_id = get_upload_id();

  if (upload_id.length() == 0) {
    std::unique_ptr<rgw::sal::Object> obj_ver = this->bucket->get_object(rgw_obj_key(this->get_key()));
    rgw::sal::MotrObject *mobj_ver = static_cast<rgw::sal::MotrObject *>(obj_ver.get());
    rgw_bucket_dir_entry ent;
    std::string key_name;

    // Get the object entry
    mobj_ver->set_instance(this->get_version_id());
    int ret_rc = mobj_ver->get_bucket_dir_ent(dpp, ent);
    if (ret_rc < 0)
      return ret_rc;

    if (!ent.is_delete_marker()) {
      rgw_obj_key key(ent.key);
      key_name = key.name + '\a' + key.instance;
      rc = store->get_upload_id(tenant_bkt_name, key_name, upload_id);
      if (rc < 0) {
        ldpp_dout(dpp, LOG_ERROR) <<__func__ << ": ERROR: get_upload_id failed. rc=" << rc << dendl;
        return rc;
      }
    }
  }

  string iname = "motr.rgw.bucket." + tenant_bkt_name + ".multiparts";
  ldpp_dout(dpp, 20) <<__func__ << ": object part index=" << iname << dendl;
  key_vec[0].clear();
  key_vec[0] = mp_obj.get_key() + "." + upload_id;
  string prefix = key_vec[0];
  char buf[32];
  snprintf(buf, sizeof(buf), ".%08d", marker + 1);
  key_vec[0].append(buf);
  rc = store->next_query_by_name(iname, key_vec, val_vec, prefix);
  if (rc < 0) {
    ldpp_dout(dpp, LOG_ERROR) <<__func__ << ": ERROR: NEXT query failed. rc=" << rc << dendl;
    return rc;
  }

  int last_num = 0;
  int part_cnt = 0;
  ldpp_dout(dpp, 20) <<__func__ << ": marker=" << marker << dendl;
  parts.clear();

  for (const auto& bl: val_vec) {
    if (bl.length() == 0)
      break;

    RGWUploadPartInfo info;
    auto iter = bl.cbegin();
    info.decode(iter);
    rgw::sal::Attrs attrs_dummy;
    decode(attrs_dummy, iter);
    MotrObjectMeta meta;
    meta.decode(iter);

    ldpp_dout(dpp, 20) <<__func__ << ": part_num=" << info.num
                                             << " part_size=" << info.size << dendl;
    ldpp_dout(dpp, 20) <<__func__ << ": key=" << mp_obj.get_key() << ", meta:oid=[0x" << std::hex << meta.oid.u_hi
                                   << ":0x" << std::hex << meta.oid.u_lo << "], meta:pvid=[0x" << std::hex
                                   << meta.pver.f_container << ":0x" << std::hex << meta.pver.f_key
                                   << "], meta:layout_id=0x" << std::hex << meta.layout_id << dendl;

    if ((int)info.num > marker) {
      last_num = info.num;
      parts.emplace(info.num, std::make_unique<MotrMultipartPart>(info, meta));
    }

    part_cnt++;
  }

  // Does it have more parts?
  if (truncated != NULL) {
    *truncated = part_cnt >= num_parts;
    ldpp_dout(dpp, 20) <<__func__ << ": truncated=" << *truncated << dendl;
  }

  if (next_marker)
    *next_marker = last_num;

  return 0;
}

// Heavily copy from rgw_sal_rados.cc
int MotrMultipartUpload::complete(const DoutPrefixProvider *dpp,
				   optional_yield y, CephContext* cct,
				   map<int, string>& part_etags,
				   list<rgw_obj_index_key>& remove_objs,
				   uint64_t& accounted_size, bool& compressed,
				   RGWCompressionInfo& cs_info, off_t& off,
				   std::string& tag, ACLOwner& owner,
				   uint64_t olh_epoch,
				   rgw::sal::Object* target_obj,
				   RGWObjectCtx* obj_ctx)
{
  char final_etag[CEPH_CRYPTO_MD5_DIGESTSIZE];
  char final_etag_str[CEPH_CRYPTO_MD5_DIGESTSIZE * 2 + 16];
  std::string etag;
  bufferlist etag_bl;
  MD5 hash;
  // Allow use of MD5 digest in FIPS mode for non-cryptographic purposes
  hash.SetFlags(EVP_MD_CTX_FLAG_NON_FIPS_ALLOW);
  bool truncated;
  int rc;

  ldpp_dout(dpp, 20) <<__func__ << ": enter" << dendl;
  int total_parts = 0;
  int handled_parts = 0;
  int max_parts = 1000;
  int marker = 0;
  uint64_t min_part_size = cct->_conf->rgw_multipart_min_part_size;
  auto etags_iter = part_etags.begin();
  rgw::sal::Attrs &attrs = target_obj->get_attrs();
  uint64_t prev_accounted_size = 0;

  do {
    ldpp_dout(dpp, 20) << __func__ << ": list_parts()" << dendl;
    rc = list_parts(dpp, cct, max_parts, marker, &marker, &truncated);
    if (rc == -ENOENT) {
      rc = -ERR_NO_SUCH_UPLOAD;
    }
    if (rc < 0)
      return rc;

    total_parts += parts.size();
    if (!truncated && total_parts != (int)part_etags.size()) {
      ldpp_dout(dpp, LOG_ERROR) <<__func__ << ": ERROR: total parts mismatch: have: " << total_parts
                        << " expected: " << part_etags.size() << dendl;
      rc = -ERR_INVALID_PART;
      return rc;
    }
    ldpp_dout(dpp, 20) << __func__ << ": parts.size()=" << parts.size() << dendl;

    for (auto obj_iter = parts.begin();
        etags_iter != part_etags.end() && obj_iter != parts.end();
        ++etags_iter, ++obj_iter, ++handled_parts) {
      MultipartPart *mpart = obj_iter->second.get();
      MotrMultipartPart *mmpart = static_cast<MotrMultipartPart *>(mpart);
      RGWUploadPartInfo *part = &mmpart->info;

      uint64_t part_size = part->accounted_size;
      ldpp_dout(dpp, 20) <<__func__ << ":  part_size=" << part_size << dendl;
      if (handled_parts < (int)part_etags.size() - 1 &&
          part_size < min_part_size) {
        rc = -ERR_TOO_SMALL;
        return rc;
      }

      char petag[CEPH_CRYPTO_MD5_DIGESTSIZE];
      if (etags_iter->first != (int)obj_iter->first) {
        ldpp_dout(dpp, LOG_ERROR) <<__func__ << ": ERROR: parts num mismatch: next requested: "
                          << etags_iter->first << " next uploaded: "
                          << obj_iter->first << dendl;
        rc = -ERR_INVALID_PART;
        return rc;
      }
      string part_etag = rgw_string_unquote(etags_iter->second);
      if (part_etag.compare(part->etag) != 0) {
        ldpp_dout(dpp, LOG_ERROR) <<__func__ << ": ERROR: etag mismatch: part: " << etags_iter->first
                          << " etag: " << etags_iter->second << dendl;
        rc = -ERR_INVALID_PART;
        return rc;
      }

      hex_to_buf(part->etag.c_str(), petag, CEPH_CRYPTO_MD5_DIGESTSIZE);
      hash.Update((const unsigned char *)petag, sizeof(petag));
      ldpp_dout(dpp, 20) <<__func__ << ": calc etag " << dendl;

      bool part_compressed = (part->cs_info.compression_type != "none");
      if ((handled_parts > 0) &&
          ((part_compressed != compressed) ||
            (cs_info.compression_type != part->cs_info.compression_type))) {
          ldpp_dout(dpp, LOG_ERROR) <<__func__ << ": ERROR: compression type was changed during multipart upload ("
                           << cs_info.compression_type << ">>" << part->cs_info.compression_type << ")" << dendl;
          rc = -ERR_INVALID_PART;
          return rc;
      }

      ldpp_dout(dpp, 20) <<__func__ << ": part compression" << dendl;
      if (part_compressed) {
        int64_t new_ofs; // offset in compression data for new part
        if (cs_info.blocks.size() > 0)
          new_ofs = cs_info.blocks.back().new_ofs + cs_info.blocks.back().len;
        else
          new_ofs = 0;
        for (const auto& block : part->cs_info.blocks) {
          compression_block cb;
          cb.old_ofs = block.old_ofs + cs_info.orig_size;
          cb.new_ofs = new_ofs;
          cb.len = block.len;
          cs_info.blocks.push_back(cb);
          new_ofs = cb.new_ofs + cb.len;
        }
        if (!compressed)
          cs_info.compression_type = part->cs_info.compression_type;
        cs_info.orig_size += part->cs_info.orig_size;
        compressed = true;
      }

      // Next part
      off += part_size;
      accounted_size += part->accounted_size;
      ldpp_dout(dpp, 20) <<__func__ << ": off=" << off << ", accounted_size=" << accounted_size << dendl;
    }

    // If the object is a composite object, add extents here.
    // As the composite object is created with only one layer and
    // all parts (extents) are written to the same layer, it makes
    // no difference that we add the extents after all parts are
    // written. At this moment, details of the parts are known and
    // extents can be added in batches.
    //
    // As the part size is usually 10s MB, no need to create an
    // extent for each part.
    if (hsm_enabled) {
      std::vector<std::pair<uint64_t, uint64_t>> exts;
      exts.emplace_back(std::pair<uint64_t, uint64_t>(off, accounted_size - prev_accounted_size));
      prev_accounted_size = accounted_size;
      rgw::sal::MotrObject *tmo = static_cast<rgw::sal::MotrObject *>(target_obj);
      rc = tmo->add_composite_layer_extents(dpp, this->meta.top_layer_oid, exts, true)? :
           tmo->add_composite_layer_extents(dpp, this->meta.top_layer_oid, exts, false);
      if (rc < 0)
        return rc;
    }

  } while (truncated);
  hash.Final((unsigned char *)final_etag);

  buf_to_hex((unsigned char *)final_etag, sizeof(final_etag), final_etag_str);
  snprintf(&final_etag_str[CEPH_CRYPTO_MD5_DIGESTSIZE * 2],
	   sizeof(final_etag_str) - CEPH_CRYPTO_MD5_DIGESTSIZE * 2,
           "-%lld", (long long)part_etags.size());
  etag = final_etag_str;
  ldpp_dout(dpp, 20) <<__func__ << ": calculated etag: " << etag << dendl;
  etag_bl.append(etag);
  attrs[RGW_ATTR_ETAG] = etag_bl;

  if (compressed) {
    // write compression attribute to full object
    bufferlist tmp;
    encode(cs_info, tmp);
    attrs[RGW_ATTR_COMPRESSION] = tmp;
  }

  // Read the object's the multipart_upload_info.
  // TODO: all those index name and key  constructions should be implemented as
  // member functions.
  bufferlist bl;
  std::unique_ptr<rgw::sal::Object> meta_obj;
  meta_obj = get_meta_obj();
  string tenant_bkt_name = get_bucket_name(meta_obj->get_bucket()->get_tenant(), meta_obj->get_bucket()->get_name());
  string bucket_multipart_iname =
      "motr.rgw.bucket." + tenant_bkt_name + ".multiparts.in-progress";
  rc = this->store->do_idx_op_by_name(bucket_multipart_iname,
                                      M0_IC_GET, meta_obj->get_key().to_str(), bl);
  ldpp_dout(dpp, 20) <<__func__ << ": read entry from bucket multipart index rc=" << rc << dendl;
  if (rc < 0)
    return rc == -ENOENT ? -ERR_NO_SUCH_UPLOAD : rc;

  rgw_bucket_dir_entry ent;
  bufferlist& blr = bl;
  auto ent_iter = blr.cbegin();
  ent.decode(ent_iter);

  motr_multipart_upload_info upload_info;
  bufferlist mpbl;
  mpbl.append(ent.meta.user_data.c_str(), ent.meta.user_data.size());
  auto mpbl_iter = mpbl.cbegin();
  upload_info.decode(mpbl_iter);

  rgw::sal::Attrs temp_attrs;
  decode(temp_attrs, ent_iter);
  // Add tag to attrs[RGW_ATTR_TAGS] key only if temp_attrs has tagging info
  if (temp_attrs.find(RGW_ATTR_TAGS) != temp_attrs.end()) {
    attrs[RGW_ATTR_TAGS] = temp_attrs[RGW_ATTR_TAGS];
  }

  // Update the dir entry and insert it to the bucket index so
  // the object will be seen when listing the bucket.
  bufferlist update_bl, old_check_bl;
  target_obj->get_key().get_index_key(&ent.key);  // Change to offical name :)
  ent.meta.size = off;
  ent.meta.accounted_size = accounted_size;
  ldpp_dout(dpp, 20) <<__func__ << ": obj size=" << ent.meta.size
                           << " obj accounted size=" << ent.meta.accounted_size << dendl;
  ent.meta.mtime = ceph::real_clock::now();
  ent.meta.etag = etag;

  ent.encode(update_bl);
  encode(attrs, update_bl);
  upload_info.meta.encode(update_bl);
  //MotrObjectMeta meta_dummy;
  //meta_dummy.encode(update_bl);

  ldpp_dout(dpp, 20) <<__func__ << ": target_obj name=" << target_obj->get_name()
                                  << " target_obj oid=" << target_obj->get_oid() << dendl;

  // Check for bucket versioning
  // Update existing object version entries in a bucket,
  // in case of both versioning enabled and suspended.
  std::unique_ptr<rgw::sal::Object> obj_ver = target_obj->get_bucket()->get_object(rgw_obj_key(target_obj->get_name()));
  rgw::sal::MotrObject *mobj_ver = static_cast<rgw::sal::MotrObject *>(obj_ver.get());

  RGWBucketInfo &info = target_obj->get_bucket()->get_info();
  if (info.versioned()) {
    rc = mobj_ver->update_version_entries(dpp);
    ldpp_dout(dpp, 20) <<__func__ << ": update_version_entries, rc=" << rc << dendl;
    if (rc < 0)
      return rc;
  }

  if (!info.versioning_enabled()) {
    int rc;
    rc = mobj_ver->remove_null_obj(dpp);
    if (rc < 0) {
      ldpp_dout(dpp, 0) <<__func__ << ": Failed to overwrite null object, rc : " << rc << dendl;
      return rc;
    }
    ent.key.instance = target_obj->get_instance();
    mobj_ver->set_instance(ent.key.instance);
  }

  string bucket_index_iname = "motr.rgw.bucket.index." + tenant_bkt_name;
  rgw::sal::MotrObject *tmo = static_cast<rgw::sal::MotrObject *>(target_obj);
  string tobj_key = tmo->get_key_str();
  rc = store->do_idx_op_by_name(bucket_index_iname, M0_IC_PUT, tobj_key, update_bl);
  if (rc < 0) {
    ldpp_dout(dpp, 0) <<__func__ << ": index operation failed, M0_IC_PUT rc=" << rc << dendl;
    return rc;
  }
  store->get_obj_meta_cache()->put(dpp, tobj_key, update_bl);

  ldpp_dout(dpp, 20) <<__func__ << ": remove from bucket multipart index " << dendl;
  return store->do_idx_op_by_name(bucket_multipart_iname,
                                  M0_IC_DEL, meta_obj->get_key().to_str(), bl);
}

int MotrMultipartUpload::get_info(const DoutPrefixProvider *dpp, optional_yield y, RGWObjectCtx* obj_ctx, rgw_placement_rule** rule, rgw::sal::Attrs* attrs)
{
  if (!rule && !attrs) {
    return 0;
  }

  if (rule) {
    if (!placement.empty()) {
      *rule = &placement;
      if (!attrs) {
        /* Don't need attrs, done */
        return 0;
      }
    } else {
      *rule = nullptr;
    }
  }

  std::unique_ptr<rgw::sal::Object> meta_obj;
  meta_obj = get_meta_obj();
  meta_obj->set_in_extra_data(true);

  // Read the object's the multipart_upload_info.
  ldpp_dout(dpp, 20) <<__func__ << "[sining]: read upload info " << dendl;
  bufferlist bl;
  string tenant_bkt_name = get_bucket_name(meta_obj->get_bucket()->get_tenant(), meta_obj->get_bucket()->get_name());
  string bucket_multipart_iname =
      "motr.rgw.bucket." + tenant_bkt_name + ".multiparts.in-progress";
  int rc = this->store->do_idx_op_by_name(bucket_multipart_iname,
                                          M0_IC_GET, meta_obj->get_key().to_str(), bl);
  if (rc < 0) {
    ldpp_dout(dpp, 0) <<__func__ << ": Failed to get multipart info. rc=" << rc << dendl;
    return rc == -ENOENT ? -ERR_NO_SUCH_UPLOAD : rc;
  }

  rgw_bucket_dir_entry ent;
  bufferlist& blr = bl;
  auto ent_iter = blr.cbegin();
  ent.decode(ent_iter);

  if (attrs) {
    bufferlist etag_bl;
    string& etag = ent.meta.etag;
    ldpp_dout(dpp, 20) <<__func__ << ": object's etag:  " << ent.meta.etag << dendl;
    etag_bl.append(etag.c_str(), etag.size());
    attrs->emplace(std::move(RGW_ATTR_ETAG), std::move(etag_bl));
    if (!rule || *rule != nullptr) {
      /* placement was cached; don't actually read */
      return 0;
    }
  }

  /* Decode multipart_upload_info */
  motr_multipart_upload_info upload_info;
  bufferlist mpbl;
  mpbl.append(ent.meta.user_data.c_str(), ent.meta.user_data.size());
  auto mpbl_iter = mpbl.cbegin();
  upload_info.decode(mpbl_iter);
  placement = upload_info.dest_placement;
  *rule = &placement;
  this->hsm_enabled = upload_info.hsm_enabled;
  this->meta = upload_info.meta;

  ldpp_dout(dpp, 0) <<__func__ << "[sining]: meta:oid=[0x" << std::hex
                     << meta.oid.u_hi << ":0x"
		     << meta.oid.u_lo << "]" << dendl;
  return 0;
}

std::unique_ptr<Writer> MotrMultipartUpload::get_writer(
				  const DoutPrefixProvider *dpp,
				  optional_yield y,
				  std::unique_ptr<rgw::sal::Object> _head_obj,
				  const rgw_user& owner, RGWObjectCtx& obj_ctx,
				  const rgw_placement_rule *ptail_placement_rule,
				  uint64_t part_num,
				  const std::string& part_num_str)
{
  if (hsm_enabled)
    return std::make_unique<MotrMultipartCompositeWriter>(dpp, y, this,
				 std::move(_head_obj), store, owner,
				 obj_ctx, ptail_placement_rule, part_num, part_num_str);
  else
    return std::make_unique<MotrMultipartWriter>(dpp, y, this,
				 std::move(_head_obj), store, owner,
				 obj_ctx, ptail_placement_rule, part_num, part_num_str);
}

int MotrMultipartWriter::store_part_info(const DoutPrefixProvider *dpp,
                                         RGWUploadPartInfo info,
                                         std::map<std::string, bufferlist>& attrs)
{
  uint64_t old_part_size = 0, old_part_size_rounded = 0;
  bool compressed;
  int rc = rgw_compression_info_from_attrset(attrs, compressed, info.cs_info);
  ldpp_dout(dpp, 20) <<__func__ << ": compression rc=" << rc << dendl;
  if (rc < 0) {
    ldpp_dout(dpp, LOG_ERROR) <<__func__ <<": ERROR: cannot get compression info" << dendl;
    return rc;
  }
  	
  ldpp_dout(dpp, 0) <<__func__ << "[sining]: encode info, attrs and meta" << dendl;
  bufferlist bl;
  encode(info, bl);
  encode(attrs, bl);
  part_obj->meta.encode(bl);

  //This is a MultipartComplete operation so this should always have valid upload id.
  string part = head_obj->get_name() + "." + upload_id;
  char buf[32];
  snprintf(buf, sizeof(buf), ".%08d", (int)part_num);
  part.append(buf);

  // Before updating object part index with entry for new part, check if
  // old part exists. Perform M0_IC_GET operation on object part index.
  ldpp_dout(dpp, 0) <<__func__ << "[sining]: check if the part info exists" << dendl;
  string tenant_bkt_name = get_bucket_name(head_obj->get_bucket()->get_tenant(),
                                           head_obj->get_bucket()->get_name());
  string iname = "motr.rgw.bucket." + tenant_bkt_name + ".multiparts";
  bufferlist old_part_check_bl;
  rc = store->do_idx_op_by_name(iname, M0_IC_GET, part, old_part_check_bl);
  if (rc == 0 && old_part_check_bl.length() > 0 && !part_obj->meta.is_composite) {
    // Old part exists. Try to delete it.
    RGWUploadPartInfo old_part_info;
    std::map<std::string, bufferlist> dummy_attr;
    string part_obj_name = head_obj->get_bucket()->get_name() + "." +
                          head_obj->get_key().to_str() +
                          ".part." + std::to_string(part_num);
    std::unique_ptr<MotrObject> old_part_obj =
        std::make_unique<MotrObject>(this->store, rgw_obj_key(part_obj_name), head_obj->get_bucket());
    if (old_part_obj == nullptr)
      return -ENOMEM;

    auto bl_iter = old_part_check_bl.cbegin();
    decode(old_part_info, bl_iter);
    decode(dummy_attr, bl_iter);
    old_part_obj->meta.decode(bl_iter);
    char oid_str[M0_FID_STR_LEN];
    snprintf(oid_str, ARRAY_SIZE(oid_str), U128X_F, U128_P(&old_part_obj->meta.oid));
    rgw::sal::MotrObject *old_mobj = static_cast<rgw::sal::MotrObject *>(old_part_obj.get());
    ldpp_dout(dpp, 20) <<__func__ << ": Old part with oid [" << oid_str << "] exists" << dendl;
    old_part_size = old_part_info.accounted_size;
    old_part_size_rounded = old_part_info.size_rounded;
    // Delete old object
    rc = old_mobj->delete_mobj(dpp);
    if (rc == 0) {
      ldpp_dout(dpp, 20) <<__func__ << ": Old part [" << part <<  "] deleted succesfully" << dendl;
    } else {
      ldpp_dout(dpp, 0) <<__func__ << ": Failed to delete old part [" << part <<  "], rc=" << rc << dendl;
      return rc;
    }
  }

  ldpp_dout(dpp, 0) <<__func__ << "[sining]: put part info into index" << dendl;
  rc = store->do_idx_op_by_name(iname, M0_IC_PUT, part, bl);
  if (rc < 0) {
    ldpp_dout(dpp, 0) <<__func__ << ": failed to add part obj in part index, rc=" << rc << dendl;
    return rc == -ENOENT ? -ERR_NO_SUCH_UPLOAD : rc;
  }
   
  // update size without changing the object count
  ldpp_dout(dpp, 0) <<__func__ << "[sining]: update bucket stats" << dendl;
  rc = update_bucket_stats(dpp, store,
                           head_obj->get_bucket()->get_acl_owner().get_id().to_str(),
                           tenant_bkt_name,
                           actual_part_size - old_part_size,
                           info.size_rounded - old_part_size_rounded, 0, true);
  if (rc != 0) {
    ldpp_dout(dpp, 20) <<__func__ << ": Failed stats update for the "
      << "obj/part=" << head_obj->get_key().to_str() << "/" << part_num
      << ", rc=" << rc << dendl;
    return rc;
  }
  ldpp_dout(dpp, 70) <<__func__ << ": Updated stats successfully for the "
      << "obj/part=" << head_obj->get_key().to_str() << "/" << part_num
      << ", rc=" << rc << dendl;

  return 0;
}	


int MotrMultipartWriter::prepare(optional_yield y)
{
  string part_obj_name = head_obj->get_bucket()->get_name() + "." +
                         head_obj->get_key().to_str() +
                         ".part." + std::to_string(part_num);
  ldpp_dout(dpp, 20) <<__func__ << ": bucket=" << head_obj->get_bucket()->get_name()
                     << "part_obj_name=" << part_obj_name << dendl;
  part_obj = std::make_unique<MotrObject>(this->store, rgw_obj_key(part_obj_name), head_obj->get_bucket());
  if (part_obj == nullptr)
    return -ENOMEM;

  // s3 client may retry uploading part, so the part may have already
  // been created.
  ldpp_dout(dpp, 20) <<__func__ << ": creating object for size=" << expected_part_size << dendl;
  int rc = part_obj->create_mobj(dpp, expected_part_size);
  if (rc == -EEXIST) {
    rc = part_obj->open_mobj(dpp);
    if (rc < 0)
      return rc;
  }
  return rc;
}

int MotrMultipartWriter::process(bufferlist&& data, uint64_t offset)
{
  int rc = part_obj->write_mobj(dpp, std::move(data), offset);
  if (rc == 0) {
    actual_part_size = part_obj->get_processed_bytes();
    ldpp_dout(dpp, 20) <<__func__ << ": actual_part_size=" << actual_part_size << dendl;
  }
  return rc;
}

int MotrMultipartWriter::complete(size_t accounted_size, const std::string& etag,
                       ceph::real_time *mtime, ceph::real_time set_mtime,
                       std::map<std::string, bufferlist>& attrs,
                       ceph::real_time delete_at,
                       const char *if_match, const char *if_nomatch,
                       const std::string *user_data,
                       rgw_zone_set *zones_trace, bool *canceled,
                       optional_yield y)
{
  // Should the dir entry(object metadata) be updated? For example
  // mtime.

  ldpp_dout(dpp, 20) <<__func__ << ": enter" << dendl;
  // Add an entry into object_nnn_part_index.
  bufferlist bl;
  RGWUploadPartInfo info;
  info.num = part_num;
  info.etag = etag;
  info.size = actual_part_size;
  uint64_t size_rounded = 0;
  //For 0kb Object layout_id will not be available.
  if(info.size != 0)
  {
    uint64_t lid = M0_OBJ_LAYOUT_ID(part_obj->meta.layout_id);
    uint64_t unit_sz = m0_obj_layout_id_to_unit_size(lid);
    size_rounded = roundup(info.size, unit_sz);
  }
  info.size_rounded = size_rounded;
  info.accounted_size = accounted_size;
  info.modified = real_clock::now();

  int rc = store_part_info(dpp, info, attrs);
  if (rc < 0) {
    ldpp_dout(dpp, 0) <<__func__ << ": failed to add part obj in part index, rc=" << rc << dendl;
    return rc == -ENOENT ? -ERR_NO_SUCH_UPLOAD : rc;
  }
   
  return 0;
}

// A few notes on implementating multipart upload using composite object.
// 1. Problems: (I) The s3 request to upload a part has a parameter 'part num'
//    but doesn't include the part's offset in the object. But a composite
//    object's extent has to be created with offset. We can't simply calculate
//    the part's offset as part_num * part_size as parts may be of different
//    sizes. (II) The native multipart upload implementation uses an index
//    to store part info. When using composite object, a part is managed as
//    an extent, so actually there exist 2 places storing part's details.
//    But the HSM app uses Motr's composite object APIs to manipulate the
//    composite object, which has no knowledge of MGW's part info index.
//    The part info in the index and composite object's extents will be
//    in a inconsistent state. S3 GET OBJ request will return wrong data
//    as it uses the part info index.
//
// 2. Solutions:
//    (I) Temporary solution for problem (I): only allow equal part size
//        (except the last part) for prototype.
//        Then the offset = part_num * part_size.
//        
//        Stated by AWS S3 doc, the part sizes are the same for every
//        part except for the last one (same or smaller). Maybe we can
//        make use of this fact to get the part size.
//
//    (II) Part info index is only used when uploading parts. When the
//        upload is completed, extents will be created according to
//        the part info stored in index. Reads from composite object
//        don't use part info index.

#define MOTR_MULTIPART_DEFAULT_PART_SIZE (15 * 1024 * 1024)

int MotrMultipartCompositeWriter::prepare(optional_yield y)
{
  ldpp_dout(dpp, 0) <<__func__ << "[sining]: enter" << dendl;

#if 0
  if (upload == nullptr)
    return -EINVAL;
  MotrMultipartUpload *mupload = static_cast<MotrMultipartUpload *>(upload);
  ldpp_dout(dpp, 0) <<__func__ << "[sining]: meta:oid=[0x" << std::hex
                     << mupload->get_motr_obj_meta().oid.u_hi << ":0x"
		     << mupload->get_motr_obj_meta().oid.u_lo << "]" << dendl;
  ldpp_dout(dpp, 0) <<__func__ << "[sining]: get_meta_obj" << dendl;
  std::unique_ptr<rgw::sal::Object> hobj = mupload->get_meta_obj();
  if (hobj == nullptr)
    return -ENOMEM;
  ldpp_dout(dpp, 0) <<__func__ << "[sining]: cast head obj to pobj" << dendl;
#endif

  // part_obj here is actually a clone of the composite object.
  RGWMPObj mp_obj(head_obj->get_key().name, upload_id);
  std::unique_ptr<rgw::sal::Object> hobj =
    head_obj->get_bucket()->get_object(rgw_obj_key(mp_obj.get_meta(), string(), mp_ns));
  std::unique_ptr<MotrObject> pobj(static_cast<rgw::sal::MotrObject *>(hobj.release()));
  part_obj = std::move(pobj);

  ldpp_dout(dpp, 0) <<__func__ << "[sining]: get motr obj meta" << dendl;
  MotrMultipartUpload *mupload = static_cast<MotrMultipartUpload *>(upload);
  part_obj->meta = mupload->get_motr_obj_meta();
  if (part_obj->meta.layout_id == 0)
    return -EINVAL;

  ldpp_dout(dpp, 20) <<__func__ << ": opening composite object" << dendl;
  return part_obj->open_mobj(dpp);
}

int MotrMultipartCompositeWriter::process(bufferlist&& data, uint64_t offset)
{
  uint64_t part_size = MOTR_MULTIPART_DEFAULT_PART_SIZE;
  uint64_t off_in_composite_obj = (part_num - 1)* part_size + offset;

  // MotrObject::write_obj() calculates the optimal bs according to
  // the ::chunk_io_sz. If its value is 0, it will lead to a crash
  // in ::write_mobj(). Set the value the same as the one when
  // creating the composite object.
  part_obj->set_chunk_io_sz(MAX_ACC_SIZE);
  int rc = part_obj->write_mobj(dpp, std::move(data), off_in_composite_obj);
  if (rc == 0) {
    actual_part_size = part_obj->get_processed_bytes();
    ldpp_dout(dpp, 20) <<__func__ << ": actual_part_size=" << actual_part_size << dendl;
  }
  return rc;
}

int MotrMultipartCompositeWriter::complete(size_t accounted_size, const std::string& etag,
                       ceph::real_time *mtime, ceph::real_time set_mtime,
                       std::map<std::string, bufferlist>& attrs,
                       ceph::real_time delete_at,
                       const char *if_match, const char *if_nomatch,
                       const std::string *user_data,
                       rgw_zone_set *zones_trace, bool *canceled,
                       optional_yield y)
{
  ldpp_dout(dpp, 20) <<__func__ << ": enter" << dendl;

  RGWUploadPartInfo info;
  info.num = part_num;
  info.etag = etag;
  info.size = actual_part_size;
  uint64_t size_rounded = 0;
  //For 0kb Object layout_id will not be available.
  ldpp_dout(dpp, 0) <<__func__ << "[sining]: round sizes" << dendl;
  if(info.size != 0) {
    uint64_t lid = M0_OBJ_LAYOUT_ID(part_obj->meta.layout_id);
    uint64_t unit_sz = m0_obj_layout_id_to_unit_size(lid);
    size_rounded = roundup(info.size, unit_sz);
  }
  info.size_rounded = size_rounded;
  info.accounted_size = accounted_size;
  info.modified = real_clock::now();

  ldpp_dout(dpp, 20) <<__func__ << "[sining]: store_part_info()" << dendl;
  int rc = this->store_part_info(dpp, info, attrs);
  if (rc < 0) {
    ldpp_dout(dpp, 0) <<__func__ << ": failed to add part obj in part index, rc=" << rc << dendl;
    return rc == -ENOENT ? -ERR_NO_SUCH_UPLOAD : rc;
  }
   
  return 0;
}

int MotrStore::get_upload_id(string tenant_bkt_name, string key_name, string& upload_id){
  int rc = 0;
  bufferlist bl;

  string index_name = "motr.rgw.bucket.index." + tenant_bkt_name;

  rc = this->do_idx_op_by_name(index_name, M0_IC_GET, key_name, bl);
  if (rc < 0) {
    //ldpp_dout(cctx, 0) << "ERROR: NEXT query failed." << rc << dendl;
    return rc;
  }

  rgw_bucket_dir_entry ent;
  bufferlist& blr = bl;
  auto ent_iter = blr.cbegin();
  ent.decode(ent_iter);

  motr_multipart_upload_info upload_info;
  bufferlist mpbl;
  mpbl.append(ent.meta.user_data.c_str(), ent.meta.user_data.size());
  auto mpbl_iter = mpbl.cbegin();
  upload_info.decode(mpbl_iter);

  upload_id.clear();
  upload_id.append(upload_info.upload_id);

  return rc;
}

std::unique_ptr<RGWRole> MotrStore::get_role(std::string name,
    std::string tenant,
    std::string path,
    std::string trust_policy,
    std::string max_session_duration_str,
    std::multimap<std::string,std::string> tags)
{
  RGWRole* p = nullptr;
  return std::unique_ptr<RGWRole>(p);
}

std::unique_ptr<RGWRole> MotrStore::get_role(std::string id)
{
  RGWRole* p = nullptr;
  return std::unique_ptr<RGWRole>(p);
}

int MotrStore::get_roles(const DoutPrefixProvider *dpp,
    optional_yield y,
    const std::string& path_prefix,
    const std::string& tenant,
    vector<std::unique_ptr<RGWRole>>& roles)
{
  return 0;
}

std::unique_ptr<RGWOIDCProvider> MotrStore::get_oidc_provider()
{
  RGWOIDCProvider* p = nullptr;
  return std::unique_ptr<RGWOIDCProvider>(p);
}

int MotrStore::get_oidc_providers(const DoutPrefixProvider *dpp,
    const std::string& tenant,
    vector<std::unique_ptr<RGWOIDCProvider>>& providers)
{
  return 0;
}

std::unique_ptr<MultipartUpload> MotrBucket::get_multipart_upload(const std::string& oid,
                                std::optional<std::string> upload_id,
                                ACLOwner owner, ceph::real_time mtime)
{
  return std::make_unique<MotrMultipartUpload>(store, this, oid, upload_id, owner, mtime);
}

std::unique_ptr<Writer> MotrStore::get_append_writer(const DoutPrefixProvider *dpp,
        optional_yield y,
        std::unique_ptr<rgw::sal::Object> _head_obj,
        const rgw_user& owner, RGWObjectCtx& obj_ctx,
        const rgw_placement_rule *ptail_placement_rule,
        const std::string& unique_tag,
        uint64_t position,
        uint64_t *cur_accounted_size) {
  return nullptr;
}

std::unique_ptr<Writer> MotrStore::get_atomic_writer(const DoutPrefixProvider *dpp,
        optional_yield y,
        std::unique_ptr<rgw::sal::Object> _head_obj,
        const rgw_user& owner, RGWObjectCtx& obj_ctx,
        const rgw_placement_rule *ptail_placement_rule,
        uint64_t olh_epoch,
        const std::string& unique_tag) {
  return std::make_unique<MotrAtomicWriter>(dpp, y,
                  std::move(_head_obj), this, owner, obj_ctx,
                  ptail_placement_rule, olh_epoch, unique_tag);
}

std::unique_ptr<User> MotrStore::get_user(const rgw_user &u)
{
  ldout(cctx, 20) <<__func__ << ": bucket's user:  " << u.to_str() << dendl;
  return std::make_unique<MotrUser>(this, u);
}

int MotrStore::get_user_by_access_key(const DoutPrefixProvider *dpp, const std::string &key, optional_yield y, std::unique_ptr<User> *user)
{
  int rc;
  User *u;
  bufferlist bl;
  RGWUserInfo uinfo;
  MotrAccessKey access_key;

  rc = do_idx_op_by_name(RGW_IAM_MOTR_ACCESS_KEY,
                           M0_IC_GET, key, bl);
  if (rc < 0){
    ldout(cctx, 0) <<__func__ << ": access key not found: rc=" << rc << dendl;
    return rc;
  }

  bufferlist& blr = bl;
  auto iter = blr.cbegin();
  access_key.decode(iter);

  uinfo.user_id.from_str(access_key.user_id);
  ldout(cctx, 0) <<__func__ << ": loading user: " << uinfo.user_id.id << dendl;
  rc = MotrUser().load_user_from_idx(dpp, this, uinfo, nullptr, nullptr);
  if (rc < 0) {
    ldout(cctx, LOG_ERROR) <<__func__ << ": ERROR: failed to load user: rc=" << rc << dendl;
    return rc;
  }
  u = new MotrUser(this, uinfo);
  if (!u)
    return -ENOMEM;

  user->reset(u);
  return 0;
}

int MotrStore::get_user_by_email(const DoutPrefixProvider *dpp, const std::string& email, optional_yield y, std::unique_ptr<User>* user)
{
  int rc;
  User *u;
  bufferlist bl;
  RGWUserInfo uinfo;
  MotrEmailInfo email_info;
  rc = do_idx_op_by_name(RGW_IAM_MOTR_EMAIL_KEY,
                           M0_IC_GET, email, bl);
  if (rc < 0) {
    ldout(cctx, LOG_ERROR) <<__func__ << ": ERROR: email Id not found: rc=" << rc << dendl;
    return rc;
  }
  auto iter = bl.cbegin();
  email_info.decode(iter);
  ldout(cctx, 0) <<__func__ << ": loading user: " << email_info.user_id << dendl;
  uinfo.user_id.from_str(email_info.user_id);
  rc = MotrUser().load_user_from_idx(dpp, this, uinfo, nullptr, nullptr);
  if (rc < 0) {
    ldout(cctx, LOG_ERROR) <<__func__ << ": ERROR: failed to load user: rc=" << rc << dendl;
    return rc;
  }
  u = new MotrUser(this, uinfo);
  if (!u)
    return -ENOMEM;

  user->reset(u);
  return 0;
}

int MotrStore::get_user_by_swift(const DoutPrefixProvider *dpp, const std::string& user_str, optional_yield y, std::unique_ptr<User>* user)
{
  /* Swift keys and subusers are not supported for now */
  return 0;
}

int MotrStore::store_access_key(const DoutPrefixProvider *dpp, optional_yield y, MotrAccessKey access_key)
{
  int rc;
  bufferlist bl;
  access_key.encode(bl);
  rc = do_idx_op_by_name(RGW_IAM_MOTR_ACCESS_KEY,
                                M0_IC_PUT, access_key.id, bl);
  if (rc < 0) {
    ldout(cctx, LOG_ERROR) <<__func__ << ": ERROR: failed to store key: rc=" << rc << dendl;
    return rc;
  }
  return rc;
}

int MotrStore::delete_access_key(const DoutPrefixProvider *dpp, optional_yield y, std::string access_key)
{
  int rc;
  bufferlist bl;
  rc = do_idx_op_by_name(RGW_IAM_MOTR_ACCESS_KEY,
                                M0_IC_DEL, access_key, bl);
  if (rc < 0) {
    ldout(cctx, LOG_ERROR) <<__func__ << ": ERROR: failed to delete key: rc=" << rc << dendl;
  }
  return rc;
}

int MotrStore::store_email_info(const DoutPrefixProvider *dpp, optional_yield y, MotrEmailInfo& email_info )
{
  int rc;
  bufferlist bl;
  email_info.encode(bl);
  rc = do_idx_op_by_name(RGW_IAM_MOTR_EMAIL_KEY,
                                M0_IC_PUT, email_info.email_id, bl);
  if (rc < 0) {
    ldout(cctx, LOG_ERROR) <<__func__ << ": ERROR: failed to store the user by email as key: rc=" << rc << dendl;
  }
  return rc;
}

int MotrStore::list_gc_objs(std::vector<std::unordered_map<std::string, std::string>>& gc_entries,
                                            std::vector<std::string>& inac_queues)
{
  auto gc = new MotrGC(cctx, this);
  int rc = gc->list(gc_entries, inac_queues);
  if (rc < 0) {
    ldout(cctx, LOG_ERROR) <<__func__ << ": ERROR: failed to list gc items: rc=" << rc << dendl;
  }
  delete gc;
  return rc;
}

std::unique_ptr<Object> MotrStore::get_object(const rgw_obj_key& k)
{
  return std::make_unique<MotrObject>(this, k);
}


int MotrStore::get_bucket(const DoutPrefixProvider *dpp, User* u, const rgw_bucket& b, std::unique_ptr<Bucket>* bucket, optional_yield y)
{
  int ret;
  Bucket* bp;

  bp = new MotrBucket(this, b, u);
  ret = bp->load_bucket(dpp, y);
  if (ret < 0) {
    delete bp;
    return ret;
  }

  bucket->reset(bp);
  return 0;
}

int MotrStore::get_bucket(User* u, const RGWBucketInfo& i, std::unique_ptr<Bucket>* bucket)
{
  Bucket* bp;

  bp = new MotrBucket(this, i, u);
  /* Don't need to fetch the bucket info, use the provided one */

  bucket->reset(bp);
  return 0;
}

int MotrStore::get_bucket(const DoutPrefixProvider *dpp, User* u, const std::string& tenant, const std::string& name, std::unique_ptr<Bucket>* bucket, optional_yield y)
{
  rgw_bucket b;

  b.tenant = tenant;
  b.name = name;

  return get_bucket(dpp, u, b, bucket, y);
}

bool MotrStore::is_meta_master()
{
  return true;
}

int MotrStore::forward_request_to_master(const DoutPrefixProvider *dpp, User* user, obj_version *objv,
    bufferlist& in_data,
    JSONParser *jp, req_info& info,
    optional_yield y)
{
  return 0;
}

std::string MotrStore::zone_unique_id(uint64_t unique_num)
{
  return "";
}

std::string MotrStore::zone_unique_trans_id(const uint64_t unique_num)
{
  return "";
}

int MotrStore::cluster_stat(RGWClusterStat& stats)
{
  return 0;
}

std::unique_ptr<Lifecycle> MotrStore::get_lifecycle(void)
{
  return 0;
}

std::unique_ptr<Completions> MotrStore::get_completions(void)
{
  return 0;
}

std::unique_ptr<Notification> MotrStore::get_notification(Object* obj, Object* src_obj, struct req_state* s,
    rgw::notify::EventType event_type, const string* object_name)
{
  return std::make_unique<MotrNotification>(obj, src_obj, event_type);
}

std::unique_ptr<Notification>  MotrStore::get_notification(const DoutPrefixProvider* dpp, Object* obj,
        Object* src_obj, RGWObjectCtx* rctx, rgw::notify::EventType event_type, rgw::sal::Bucket* _bucket,
        std::string& _user_id, std::string& _user_tenant, std::string& _req_id, optional_yield y)
{
  return std::make_unique<MotrNotification>(obj, src_obj, event_type);
}

int MotrStore::log_usage(const DoutPrefixProvider *dpp, map<rgw_user_bucket, RGWUsageBatch>& usage_info)
{
  return 0;
}

int MotrStore::log_op(const DoutPrefixProvider *dpp, string& oid, bufferlist& bl)
{
  return 0;
}

int MotrStore::register_to_service_map(const DoutPrefixProvider *dpp, const string& daemon_type,
    const map<string, string>& meta)
{
  return 0;
}

void MotrStore::get_ratelimit(RGWRateLimitInfo& bucket_ratelimit,
                              RGWRateLimitInfo& user_ratelimit,
                              RGWRateLimitInfo& anon_ratelimit)
{
  return;
}

void MotrStore::get_quota(RGWQuotaInfo& bucket_quota, RGWQuotaInfo& user_quota)
{
  // XXX: Not handled for the first pass
  return;
}

int MotrStore::set_buckets_enabled(const DoutPrefixProvider *dpp, vector<rgw_bucket>& buckets, bool enabled)
{
  return 0;
}

int MotrStore::get_sync_policy_handler(const DoutPrefixProvider *dpp,
    std::optional<rgw_zone_id> zone,
    std::optional<rgw_bucket> bucket,
    RGWBucketSyncPolicyHandlerRef *phandler,
    optional_yield y)
{
  return 0;
}

RGWDataSyncStatusManager* MotrStore::get_data_sync_manager(const rgw_zone_id& source_zone)
{
  return 0;
}

int MotrStore::read_all_usage(const DoutPrefixProvider *dpp, uint64_t start_epoch, uint64_t end_epoch,
    uint32_t max_entries, bool *is_truncated,
    RGWUsageIter& usage_iter,
    map<rgw_user_bucket, rgw_usage_log_entry>& usage)
{
  return -ENOENT;
}

int MotrStore::trim_all_usage(const DoutPrefixProvider *dpp, uint64_t start_epoch, uint64_t end_epoch)
{
  return 0;
}

int MotrStore::get_config_key_val(string name, bufferlist *bl)
{
  return 0;
}

int MotrStore::meta_list_keys_init(const DoutPrefixProvider *dpp, const string& section, const string& marker, void** phandle)
{
  return 0;
}

int MotrStore::meta_list_keys_next(const DoutPrefixProvider *dpp, void* handle, int max, list<string>& keys, bool* truncated)
{
  return 0;
}

void MotrStore::meta_list_keys_complete(void* handle)
{
  return;
}

std::string MotrStore::meta_get_marker(void* handle)
{
  return "";
}

int MotrStore::meta_remove(const DoutPrefixProvider *dpp, string& metadata_key, optional_yield y)
{
  return 0;
}
int MotrStore::list_users(const DoutPrefixProvider* dpp, const std::string& metadata_key,
                        std::string& marker, int max_entries, void *&handle,
                        bool* truncated, std::list<std::string>& users)
{
  int rc;
  bufferlist bl;
  if (max_entries <= 0 or max_entries > 1000) {
    max_entries = 1000;
  }
  vector<string> keys(max_entries + 1);
  vector<bufferlist> vals(max_entries + 1);

  if(!(marker.empty())){
    rc = do_idx_op_by_name(RGW_MOTR_USERS_IDX_NAME,
                                  M0_IC_GET, marker, bl);
    if (rc < 0) {
      ldpp_dout(dpp, LOG_ERROR) << ": ERROR: Invalid marker, rc=" << rc << dendl;
      return rc;
    }
    else {
      keys[0] = marker;
    }
  }

  rc = next_query_by_name(RGW_MOTR_USERS_IDX_NAME, keys, vals);
  if (rc < 0) {
    ldpp_dout(dpp, LOG_ERROR) <<__func__ <<": ERROR: NEXT query failed. rc=" << rc << dendl;
    return rc;
  }
  if (!(keys.back()).empty()) {
    *truncated = true;
    marker = keys.back();
  }
  for (int i = 0; i < int(keys.size()) - 1; i++) {
    if (keys[i].empty()) {
      break;
    }
    users.push_back(keys[i]);
  }
  return rc;
}

static void set_m0bufvec(struct m0_bufvec *bv, vector<uint8_t>& vec)
{
  *bv->ov_buf = reinterpret_cast<char*>(vec.data());
  *bv->ov_vec.v_count = vec.size();
}

// idx must be opened with open_motr_idx() beforehand
int MotrStore::do_idx_op(struct m0_idx *idx, enum m0_idx_opcode opcode,
                         vector<uint8_t>& key, vector<uint8_t>& val, bool update)
{
  int rc, rc_i;
  struct m0_bufvec k, v, *vp = &v;
  uint32_t flags = 0;
  struct m0_op *op = nullptr;

  ADDB(RGW_ADDB_REQUEST_ID, addb_logger.get_id(),
       RGW_ADDB_FUNC_DO_IDX_OP, RGW_ADDB_PHASE_START);
  rc = m0_bufvec_empty_alloc(&k, 1);
  if (rc != 0) {
    ldout(cctx, LOG_ERROR) <<__func__ <<": ERROR: failed to allocate key bufvec. rc=" << rc << dendl;
    ADDB(RGW_ADDB_REQUEST_ID, addb_logger.get_id(),
	 RGW_ADDB_FUNC_DO_IDX_OP,
	 RGW_ADDB_PHASE_ERROR);
    return -ENOMEM;
  }

  if (opcode == M0_IC_PUT || opcode == M0_IC_GET) {
    rc = m0_bufvec_empty_alloc(&v, 1);
    if (rc != 0) {
      ldout(cctx, LOG_ERROR) <<__func__ << ": ERROR: failed to allocate value bufvec, rc=" << rc << dendl;
      rc = -ENOMEM;
      ADDB(RGW_ADDB_REQUEST_ID, addb_logger.get_id(),
           RGW_ADDB_FUNC_DO_IDX_OP, RGW_ADDB_PHASE_ERROR);
      goto out;
    }
  }

  set_m0bufvec(&k, key);
  if (opcode == M0_IC_PUT)
    set_m0bufvec(&v, val);

  if (opcode == M0_IC_DEL)
    vp = nullptr;

  if (opcode == M0_IC_PUT && update)
    flags |= M0_OIF_OVERWRITE;

  rc = m0_idx_op(idx, opcode, &k, vp, &rc_i, flags, &op);
  if (rc != 0) {
    ldout(cctx, LOG_ERROR) <<__func__ << ": ERROR: failed to init index op: " << rc << dendl;
    ADDB(RGW_ADDB_REQUEST_ID, addb_logger.get_id(),
         RGW_ADDB_FUNC_DO_IDX_OP, RGW_ADDB_PHASE_ERROR);
    goto out;
  }

  ADDB(RGW_ADDB_REQUEST_TO_MOTR_ID, addb_logger.get_id(), m0_sm_id_get(&op->op_sm));
  m0_op_launch(&op, 1);
  rc = m0_op_wait(op, M0_BITS(M0_OS_FAILED, M0_OS_STABLE), M0_TIME_NEVER) ?:
       m0_rc(op);
  m0_op_fini(op);
  m0_op_free(op);

  if (rc != 0) {
    ldout(cctx, LOG_ERROR) <<__func__ << ": ERROR: op failed: " << rc << dendl;
    ADDB(RGW_ADDB_REQUEST_ID, addb_logger.get_id(),
         RGW_ADDB_FUNC_DO_IDX_OP, RGW_ADDB_PHASE_ERROR);
    goto out;
  }

  if (rc_i != 0) {
    ldout(cctx, LOG_ERROR) <<__func__ << ": ERROR: idx op failed: " << rc_i << dendl;
    rc = rc_i;
    ADDB(RGW_ADDB_REQUEST_ID, addb_logger.get_id(),
         RGW_ADDB_FUNC_DO_IDX_OP, RGW_ADDB_PHASE_ERROR);
    goto out;
  }

  if (opcode == M0_IC_GET) {
    val.resize(*v.ov_vec.v_count);
    memcpy(reinterpret_cast<char*>(val.data()), *v.ov_buf, *v.ov_vec.v_count);
  }

  ADDB(RGW_ADDB_REQUEST_ID, addb_logger.get_id(),
       RGW_ADDB_FUNC_DO_IDX_OP, RGW_ADDB_PHASE_DONE);
out:
  m0_bufvec_free2(&k);
  if (opcode == M0_IC_GET)
    m0_bufvec_free(&v); // cleanup buffer after GET
  else if (opcode == M0_IC_PUT)
    m0_bufvec_free2(&v);

  return rc;
}

// Retrieve a range of key/value pairs starting from keys[0].
int MotrStore::do_idx_next_op(struct m0_idx *idx,
                              vector<vector<uint8_t>>& keys,
                              vector<vector<uint8_t>>& vals)
{
  int rc;
  uint32_t i = 0;
  int nr_kvp = vals.size();
  int *rcs = new int[nr_kvp];
  struct m0_bufvec k, v;
  struct m0_op *op = nullptr;

  ADDB(RGW_ADDB_REQUEST_ID, addb_logger.get_id(),
       RGW_ADDB_FUNC_DO_IDX_NEXT_OP, RGW_ADDB_PHASE_START);

  rc = m0_bufvec_empty_alloc(&k, nr_kvp)?:
       m0_bufvec_empty_alloc(&v, nr_kvp);
  if (rc != 0) {
    ldout(cctx, LOG_ERROR) <<__func__ << ": ERROR: failed to allocate kv bufvecs" << dendl;
    ADDB(RGW_ADDB_REQUEST_ID, addb_logger.get_id(),
         RGW_ADDB_FUNC_DO_IDX_NEXT_OP, RGW_ADDB_PHASE_ERROR);
    return rc;
  }

  set_m0bufvec(&k, keys[0]);

  rc = m0_idx_op(idx, M0_IC_NEXT, &k, &v, rcs, 0, &op);
  if (rc != 0) {
    ldout(cctx, LOG_ERROR) <<__func__ << ": ERROR: failed to init index op: " << rc << dendl;
    ADDB(RGW_ADDB_REQUEST_ID, addb_logger.get_id(),
         RGW_ADDB_FUNC_DO_IDX_NEXT_OP, RGW_ADDB_PHASE_ERROR);
    goto out;
  }

  ADDB(RGW_ADDB_REQUEST_TO_MOTR_ID, addb_logger.get_id(), m0_sm_id_get(&op->op_sm));
  m0_op_launch(&op, 1);
  rc = m0_op_wait(op, M0_BITS(M0_OS_FAILED, M0_OS_STABLE), M0_TIME_NEVER) ?:
       m0_rc(op);
  m0_op_fini(op);
  m0_op_free(op);

  if (rc != 0) {
    ldout(cctx, LOG_ERROR) <<__func__ << ": ERROR: op failed: " << rc << dendl;
    ADDB(RGW_ADDB_REQUEST_ID, addb_logger.get_id(),
         RGW_ADDB_FUNC_DO_IDX_NEXT_OP, RGW_ADDB_PHASE_ERROR);
    goto out;
  }

  for (i = 0; i < v.ov_vec.v_nr; ++i) {
    if (rcs[i] < 0)
      break;

    vector<uint8_t>& key = keys[i];
    vector<uint8_t>& val = vals[i];
    key.resize(k.ov_vec.v_count[i]);
    val.resize(v.ov_vec.v_count[i]);
    memcpy(reinterpret_cast<char*>(key.data()), k.ov_buf[i], k.ov_vec.v_count[i]);
    memcpy(reinterpret_cast<char*>(val.data()), v.ov_buf[i], v.ov_vec.v_count[i]);
  }

  ADDB(RGW_ADDB_REQUEST_ID, addb_logger.get_id(),
       RGW_ADDB_FUNC_DO_IDX_NEXT_OP, RGW_ADDB_PHASE_DONE);
out:
  k.ov_vec.v_nr = i;
  v.ov_vec.v_nr = i;
  m0_bufvec_free(&k);
  m0_bufvec_free(&v); // cleanup buffer after GET

  delete []rcs;
  return rc ?: i;
}

// Retrieve a number of key/value pairs under the prefix starting
// from the marker at key_out[0].
int MotrStore::next_query_by_name(string idx_name,
                                  vector<string>& key_out,
                                  vector<bufferlist>& val_out,
                                  string prefix, string delim)
{
  unsigned nr_kvp = std::min(val_out.size(), 100UL);
  struct m0_idx idx = {};
  vector<vector<uint8_t>> keys(nr_kvp);
  vector<vector<uint8_t>> vals(nr_kvp);
  struct m0_uint128 idx_id;
  int i = 0, j, k = 0;

  index_name_to_motr_fid(idx_name, &idx_id);
  int rc = open_motr_idx(&idx_id, &idx);
  if (rc != 0) {
    ldout(cctx, LOG_ERROR) <<__func__ << ": ERROR: failed to open index: rc="
                   << rc << dendl;
    goto out;
  }

  // Only the first element for keys needs to be set for NEXT query.
  // The keys will be set will the returned keys from motr index.
  ldout(cctx, 20) <<__func__ << ": index=" << idx_name << " keys[0]=" << key_out[0]
                  << " prefix=" << prefix << " delim=" << delim  << dendl;
  keys[0].assign(key_out[0].begin(), key_out[0].end());
  for (i = 0; i < (int)val_out.size(); i += k, k = 0) {
    rc = do_idx_next_op(&idx, keys, vals);
    ldout(cctx, 20) <<__func__ << ": do_idx_next_op()=" << rc << dendl;
    if (rc < 0) {
      ldout(cctx, LOG_ERROR) <<__func__ << ": ERROR: NEXT query failed, rc=" << rc << dendl;
      goto out;
    } else if (rc == 0) {
      ldout(cctx, 20) <<__func__ << ": No more entries in the table." << dendl;
      goto out;
    }

    string dir;
    for (j = 0, k = 0; j < rc; ++j) {
      string key(keys[j].begin(), keys[j].end());
      size_t pos = std::string::npos;
      if (!delim.empty())
        pos = key.find(delim, prefix.length());
      if (pos != std::string::npos) { // DIR entry
        dir.assign(key, 0, pos + delim.length());
        if (dir.compare(0, prefix.length(), prefix) != 0)
          goto out;
        if (i + k == 0 || dir != key_out[i + k - 1]) // a new one
          key_out[i + k++] = dir;
        continue;
      }
      dir = "";
      if (key.compare(0, prefix.length(), prefix) != 0)
        goto out;
      key_out[i + k] = key;
      bufferlist& vbl = val_out[i + k];
      vbl.append(reinterpret_cast<char*>(vals[j].data()), vals[j].size());
      ++k;
    }

    if (rc < (int)nr_kvp) // there are no more keys to fetch
      break;

    string next_key;
    if (dir != "")
      next_key = dir + "\xff"; // skip all dir content in 1 step
    else
      next_key = key_out[i + k - 1] + " ";
    ldout(cctx, 0) <<__func__ << ": do_idx_next_op(): next_key=" << next_key << dendl;
    keys[0].assign(next_key.begin(), next_key.end());

    int keys_left = val_out.size() - (i + k);  // i + k gives next index.
    // Resizing keys & vals vector when `keys_left < batch size`.
    if (keys_left < (int)nr_kvp) {
      keys.resize(keys_left);
      vals.resize(keys_left);
    }
  }

out:
  m0_idx_fini(&idx);
  return rc < 0 ? rc : i + k;
}

int MotrStore::delete_motr_idx_by_name(string iname)
{
  struct m0_idx idx = {};
  struct m0_uint128 idx_id;
  struct m0_op *op = nullptr;

  ldout(cctx, 20) <<__func__ << ": iname=" << iname << dendl;

  ADDB(RGW_ADDB_REQUEST_ID, addb_logger.get_id(),
       RGW_ADDB_FUNC_DELETE_IDX_BY_NAME, RGW_ADDB_PHASE_START);

  index_name_to_motr_fid(iname, &idx_id);
  m0_idx_init(&idx, &container.co_realm, &idx_id);
  m0_entity_open(&idx.in_entity, &op);
  int rc = m0_entity_delete(&idx.in_entity, &op);
  if (rc < 0) {
    ldout(cctx, 0) <<__func__ <<": m0_entity_delete failed, rc=" << rc << dendl;
    ADDB(RGW_ADDB_REQUEST_ID, addb_logger.get_id(),
         RGW_ADDB_FUNC_DELETE_IDX_BY_NAME, RGW_ADDB_PHASE_ERROR);
    goto out;
  }

  ADDB(RGW_ADDB_REQUEST_TO_MOTR_ID, addb_logger.get_id(), m0_sm_id_get(&op->op_sm));
  m0_op_launch(&op, 1);

  ldout(cctx, 70) <<__func__ << ": waiting for op completion" << dendl;

  rc = m0_op_wait(op, M0_BITS(M0_OS_FAILED, M0_OS_STABLE), M0_TIME_NEVER) ?:
       m0_rc(op);
  m0_op_fini(op);
  m0_op_free(op);

  if (rc == -ENOENT) // race deletion??
    rc = 0;
  else if (rc < 0) {
    ldout(cctx, LOG_ERROR) <<__func__ << ": ERROR: index create failed. rc=" << rc << dendl;
    ADDB(RGW_ADDB_REQUEST_ID, addb_logger.get_id(),
         RGW_ADDB_FUNC_DELETE_IDX_BY_NAME, RGW_ADDB_PHASE_ERROR);
    goto out;
  }

  ldout(cctx, 20) <<__func__ << ": delete_motr_idx_by_name rc=" << rc << dendl;
  ADDB(RGW_ADDB_REQUEST_ID, addb_logger.get_id(),
       RGW_ADDB_FUNC_DELETE_IDX_BY_NAME, RGW_ADDB_PHASE_DONE);
out:
  ldout(cctx, 20) << "delete_motr_idx_by_name rc=" << rc << dendl;
  m0_idx_fini(&idx);
  return rc;
}

int MotrStore::open_motr_idx(struct m0_uint128 *id, struct m0_idx *idx)
{
  m0_idx_init(idx, &container.co_realm, id);
  return 0;
}

// The following marcos are from dix/fid_convert.h which are not exposed.
enum {
      M0_DIX_FID_DEVICE_ID_OFFSET   = 32,
      M0_DIX_FID_DIX_CONTAINER_MASK = (1ULL << M0_DIX_FID_DEVICE_ID_OFFSET)
                                      - 1,
};

// md5 is used here, a more robust way to convert index name to fid is
// needed to avoid collision.
void MotrStore::index_name_to_motr_fid(string iname, struct m0_uint128 *id)
{
  unsigned char md5[16];  // 128/8 = 16
  MD5 hash;

  // Allow use of MD5 digest in FIPS mode for non-cryptographic purposes
  hash.SetFlags(EVP_MD_CTX_FLAG_NON_FIPS_ALLOW);
  hash.Update((const unsigned char *)iname.c_str(), iname.length());
  hash.Final(md5);

  memcpy(&id->u_hi, md5, 8);
  memcpy(&id->u_lo, md5 + 8, 8);
  ldout(cctx, 20) <<__func__ << ": id = 0x" << std::hex << id->u_hi << ":0x" << std::hex << id->u_lo  << dendl;

  struct m0_fid *fid = (struct m0_fid*)id;
  m0_fid_tset(fid, m0_dix_fid_type.ft_id,
              fid->f_container & M0_DIX_FID_DIX_CONTAINER_MASK, fid->f_key);
  ldout(cctx, 20) <<__func__ << ": converted id = 0x" << std::hex << id->u_hi << ":0x" << std::hex << id->u_lo  << dendl;
}

int MotrStore::do_idx_op_by_name(string idx_name, enum m0_idx_opcode opcode,
                                 string key_str, bufferlist &bl, bool update)
{
  struct m0_idx idx = {};
  vector<uint8_t> key(key_str.begin(), key_str.end());
  vector<uint8_t> val;
  struct m0_uint128 idx_id;

  index_name_to_motr_fid(idx_name, &idx_id);
  int rc = open_motr_idx(&idx_id, &idx);
  if (rc != 0) {
    ldout(cctx, LOG_ERROR) <<__func__ << ": ERROR: failed to open index rc=" << rc << dendl;
    goto out;
  }

  if (opcode == M0_IC_PUT)
    val.assign(bl.c_str(), bl.c_str() + bl.length());

  ldout(cctx, 20) <<__func__ << ": op=" << (opcode == M0_IC_PUT ? "PUT" : "GET")
                 << " idx=" << idx_name << " key=" << key_str << dendl;
  rc = do_idx_op(&idx, opcode, key, val, update);
  if (rc == 0 && opcode == M0_IC_GET)
    // Append the returned value (blob) to the bufferlist.
    bl.append(reinterpret_cast<char*>(val.data()), val.size());
  if (rc < 0) {
    ldout(cctx, LOG_ERROR) <<__func__ << ": ERROR: index operation "<< opcode << " failed, rc=" << rc << dendl;
  }
out:
  m0_idx_fini(&idx);
  return rc;
}

int MotrStore::create_motr_idx_by_name(string iname)
{
  struct m0_idx idx = {};
  struct m0_uint128 id;

  ADDB(RGW_ADDB_REQUEST_ID, addb_logger.get_id(),
       RGW_ADDB_FUNC_CREATE_IDX_BY_NAME, RGW_ADDB_PHASE_START);

  index_name_to_motr_fid(iname, &id);
  m0_idx_init(&idx, &container.co_realm, &id);

  // create index or make sure it's created
  struct m0_op *op = nullptr;
  int rc = m0_entity_create(nullptr, &idx.in_entity, &op);
  if (rc != 0) {
    ldout(cctx, LOG_ERROR) <<__func__ << ": ERROR: m0_entity_create() failed, rc=" << rc << dendl;
    ADDB(RGW_ADDB_REQUEST_ID, addb_logger.get_id(),
         RGW_ADDB_FUNC_CREATE_IDX_BY_NAME, RGW_ADDB_PHASE_ERROR);
    goto out;
  }

  ADDB(RGW_ADDB_REQUEST_TO_MOTR_ID, addb_logger.get_id(), m0_sm_id_get(&op->op_sm));
  m0_op_launch(&op, 1);
  rc = m0_op_wait(op, M0_BITS(M0_OS_FAILED, M0_OS_STABLE), M0_TIME_NEVER) ?:
       m0_rc(op);
  m0_op_fini(op);
  m0_op_free(op);

  if (rc != 0 && rc != -EEXIST) {
    ldout(cctx, LOG_ERROR) <<__func__ << ": ERROR: index create failed, rc=" << rc << dendl;
    ADDB(RGW_ADDB_REQUEST_ID, addb_logger.get_id(),
         RGW_ADDB_FUNC_CREATE_IDX_BY_NAME, RGW_ADDB_PHASE_ERROR);
    goto out;
  }

  ADDB(RGW_ADDB_REQUEST_ID, addb_logger.get_id(),
       RGW_ADDB_FUNC_CREATE_IDX_BY_NAME, RGW_ADDB_PHASE_DONE);
out:
  m0_idx_fini(&idx);
  return rc;
}

// If a global index is checked (if it has been create) every time
// before they're queried (put/get), which takes 2 Motr operations to
// complete the query. As the global indices' name and FID are known
// already when MotrStore is created, we move the check and creation
// in newMotrStore().
// Similar method is used for per bucket/user index. For example,
// bucket instance index is created when creating the bucket.
int MotrStore::check_n_create_global_indices()
{
  int rc = 0;

  for (const auto& iname : motr_global_indices) {
    rc = create_motr_idx_by_name(iname);
    if (rc < 0 && rc != -EEXIST)
      break;
    rc = 0;
  }

  return rc;
}

std::string MotrStore::get_cluster_id(const DoutPrefixProvider* dpp,  optional_yield y)
{
  char id[M0_FID_STR_LEN];
  struct m0_confc *confc = m0_reqh2confc(&instance->m0c_reqh);

  m0_fid_print(id, ARRAY_SIZE(id), &confc->cc_root->co_id);
  return std::string(id);
}

int MotrStore::init_metadata_cache(const DoutPrefixProvider *dpp,
                                   CephContext *cct)
{
  this->obj_meta_cache = new MotrMetaCache(dpp, cct);
  this->get_obj_meta_cache()->set_enabled(use_cache);

  this->user_cache = new MotrMetaCache(dpp, cct);
  this->get_user_cache()->set_enabled(use_cache);

  this->bucket_inst_cache = new MotrMetaCache(dpp, cct);
  this->get_bucket_inst_cache()->set_enabled(use_cache);

  return 0;
}

} // namespace rgw::sal

extern "C" {

void *newMotrStore(CephContext *cct)
{
  int rc = -1;
  rgw::sal::MotrStore *store = new rgw::sal::MotrStore(cct);

  if (store) {
    store->conf.mc_is_oostore     = true;
    // XXX: these params should be taken from config settings and
    // cct somehow?
    store->instance = nullptr;
    const auto& proc_ep  = g_conf().get_val<std::string>("motr_my_endpoint");
    const auto& ha_ep    = g_conf().get_val<std::string>("motr_ha_endpoint");
    const auto& proc_fid = g_conf().get_val<std::string>("motr_my_fid");
    const auto& profile  = g_conf().get_val<std::string>("motr_profile_fid");
    const auto& admin_proc_ep  = g_conf().get_val<std::string>("motr_admin_endpoint");
    const auto& admin_proc_fid = g_conf().get_val<std::string>("motr_admin_fid");
    const bool addb_enabled = g_conf().get_val<bool>("motr_addb_enabled");
    const int init_flags = cct->get_init_flags();
    ldout(cct, LOG_INFO) << ": INFO: motr my endpoint: " << proc_ep << dendl;
    ldout(cct, LOG_INFO) << ": INFO: motr ha endpoint: " << ha_ep << dendl;
    ldout(cct, LOG_INFO) << ": INFO: motr my fid:      " << proc_fid << dendl;
    ldout(cct, LOG_INFO) << ": INFO: motr profile fid: " << profile << dendl;
    ldout(cct, LOG_INFO) << ": INFO: motr addb enabled: " << addb_enabled << dendl;
    store->conf.mc_local_addr  = proc_ep.c_str();
    store->conf.mc_process_fid = proc_fid.c_str();

    ldout(cct, LOG_INFO) << ": INFO: init flags:       " << init_flags << dendl;
    ldout(cct, LOG_INFO) << ": INFO: motr admin endpoint: " << admin_proc_ep << dendl;
    ldout(cct, LOG_INFO) << ": INFO: motr admin fid:   " << admin_proc_fid << dendl;

    // HACK this is so that radosge-admin uses a different client
    if (init_flags == 0) {
      store->conf.mc_process_fid = admin_proc_fid.c_str();
      store->conf.mc_local_addr  = admin_proc_ep.c_str();
    } else {
      store->conf.mc_process_fid = proc_fid.c_str();
      store->conf.mc_local_addr  = proc_ep.c_str();
    }
    store->conf.mc_ha_addr      = ha_ep.c_str();
    store->conf.mc_profile      = profile.c_str();
    store->conf.mc_is_addb_init = addb_enabled;

    ldout(cct, LOG_DEBUG) << ": DEBUG: motr profile fid:  " << store->conf.mc_profile << dendl;
    ldout(cct, LOG_DEBUG) << ": DEBUG: ha addr:  " << store->conf.mc_ha_addr << dendl;
    ldout(cct, LOG_DEBUG) << ": DEBUG: process fid:  " << store->conf.mc_process_fid << dendl;
    ldout(cct, LOG_DEBUG) << ": DEBUG: motr endpoint:  " << store->conf.mc_local_addr << dendl;
    ldout(cct, LOG_DEBUG) << ": DEBUG: motr addb enabled:  " << store->conf.mc_is_addb_init << dendl;

    store->conf.mc_tm_recv_queue_min_len =     64;
    store->conf.mc_max_rpc_msg_size      = 524288;
    store->conf.mc_idx_service_id  = M0_IDX_DIX;
    store->dix_conf.kc_create_meta = false;
    store->conf.mc_idx_service_conf = &store->dix_conf;

    if (!g_conf().get_val<bool>("motr_tracing_enabled")) {
      m0_trace_level_allow(M0_WARN); // allow errors and warnings in syslog anyway
      m0_trace_set_mmapped_buffer(false);
    }

    store->instance = nullptr;
    ldout(cct, 10) <<__func__ << "INFO: calling m0_client_init()" << rc << dendl;
    rc = m0_client_init(&store->instance, &store->conf, true);
    if (rc != 0) {
      ldout(cct, LOG_ERROR) <<__func__ << ": ERROR: m0_client_init() failed: " << rc << dendl;
      goto out;
    }
    rgw::sal::MotrADDBLogger::set_m0_instance(store->instance->m0c_motr);

    m0_container_init(&store->container, nullptr, &M0_UBER_REALM, store->instance);
    rc = store->container.co_realm.re_entity.en_sm.sm_rc;
    if (rc != 0) {
      ldout(cct, LOG_ERROR) <<__func__ << ": ERROR: m0_container_init() failed: " << rc << dendl;
      goto out;
    }

    rc = m0_ufid_init(store->instance, &ufid_gr);
    if (rc != 0) {
      ldout(cct, LOG_ERROR) <<__func__ << ": ERROR: m0_ufid_init() failed: " << rc << dendl;
      goto out;
    }

    // Create global indices if not yet.
    rc = store->check_n_create_global_indices();
    if (rc != 0) {
      ldout(cct, LOG_ERROR) <<__func__ << ": ERROR: check_n_create_global_indices() failed: " << rc << dendl;
      goto out;
    }

    store->hsm_enabled = g_conf().get_val<bool>("motr_hsm_enabled");
  }

out:
  if (rc != 0) {
    rgw::sal::MotrADDBLogger::set_m0_instance(nullptr);
    delete store;
    return nullptr;
  }
  return store;
}

}
