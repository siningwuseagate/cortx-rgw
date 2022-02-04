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

#pragma once

extern "C" {
#include "motr/config.h"
#include "motr/client.h"
#include "fdmi/fdmi.h"
#include "fdmi/plugin_dock.h"
#include "fdmi/service.h"
#include "rpc/rpc_opcodes.h"
}

#include "rgw_sal.h"
#include "rgw_rados.h"
#include "rgw_notify.h"
#include "rgw_oidc_provider.h"
#include "rgw_role.h"
#include "rgw_multi.h"
#include "rgw_putobj_processor.h"

namespace rgw::sal {

class MotrStore;

// Global Motr indices
#define RGW_MOTR_USERS_IDX_NAME       "motr.rgw.users"
#define RGW_MOTR_BUCKET_INST_IDX_NAME "motr.rgw.bucket.instances"
#define RGW_MOTR_BUCKET_HD_IDX_NAME   "motr.rgw.bucket.headers"
//#define RGW_MOTR_BUCKET_ACL_IDX_NAME  "motr.rgw.bucket.acls"

// Notes for Motr FDMI:
// (1) Currently FDMI can only trigger events for index operation, not object
//     operation.
// (2) When an index op event is generated, the FDMI filter picks up the events
//     matching the pre-defined rules. The FDMI event contains the key/value of
//     the index operation. Currently, fdmi can only do sub string matching on
//     the value part, not key part, and can't specify which index to watch,
//     the filter rule is applied to all indices.
//     this 
// (3) The FDMI event is delivered to an FDMI application which defines a
//     callback function to process FDMI events.
//
// Implementation of object watch-notify using Motr FDMI. 
// (1) An FDMI filter is added (in Motr configuration yaml file) specifying
//     a matching sub-string (marker).
// (2) A notifier creates a set of indices which are used to trigger FDMI
//     events and to pass messages.
// (3) For an object, hash(obj_name) is used to pick which index to insert
//     a new key/value record,
//     key = unique_fid(obj_name),
//     value = marker (for fdmi filter) + notification msg.
//     If multiple rgw instances are trying to write the same objects at the same
//     time, we assume that Motr index is updated atomically.
// (4) An FDMI event with a special marker is generated and
//     picked up by the filter and delivered to our fdmi application, watcher.
//     The watcher defines the callback function processing the FDMI events.
// (5) The embedded notification message is then decoded to get object name
//     and index opcode for further action.
//
// How metadata cache use watch-notify:
// (1) MGW uses an index to store object metatadata.
// (2) As metadata has its own index, instead of creating a set of new
//     notification indices above, metadata cache makes use of the metadata
//     index to pass notification.
//     When an object's metadata is updated, the corresponding key/value pair
//     in the metadata index is updated. The value is attached with a
//     notification message containing the FDMI filter's matching sub-string.
// (3) When the watcher receives notification, the decoded info are sent to
//     cache layer.
// (4) Update cache item accordingly and release notification(fdmi record).
// (5) Note: if the size of metadata is big, the FDMI event size created will
//     be big too (key + notification + metadata), the overhead to transfer
//     the FDMI event is big, how will this impact the performance and
//     scalability of FDMI?

#define RGW_MOTR_CACHE_FDMI_FILTER_MARKER "rgw.motr.cache.fdmi.marker"

// The notification message sent from notifier to watcher.
class MotrWatchNotifyMsg {
protected:
  // The marker is used by FDMI to filter out the notification.
  std::string marker;
  std::string sender;

public:
  MotrWatchNotifyMsg(std::string _marker, std::string _sender) : marker(_marker), sender(_sender) {}
  MotrWatchNotifyMsg(std::string _marker) : marker(_marker) {}
  MotrWatchNotifyMsg() {}
  virtual ~MotrWatchNotifyMsg() = default;
  virtual void encode(bufferlist& bl) const {
    // Add raw marker string at the begining to let FDMI filter
    // catch it easily and make the header readable. Encoding marker
    // with sender like the uncommented code below also works (and has
    // been tested).
    uint32_t marker_len = marker.length();
    char len_str[16];
    snprintf(len_str, sizeof(len_str), "%08d", marker_len);
    std::string head = std::string(len_str) + '.' + marker;
    bl.append(head);

    ENCODE_START(2, 2, bl);
    //encode(marker, bl);
    encode(sender, bl);
    ENCODE_FINISH(bl);
  }
  virtual void decode(bufferlist::const_iterator& bl) {
    uint32_t marker_len;
    char len_str[16];
    bl.copy(8, (char *)len_str);
    marker_len = std::stoul(len_str, nullptr, 10);
    marker.clear();
    bl.copy(marker_len + 1, marker);
    marker.erase(0, 1);

    DECODE_START(2, bl);
    //decode(marker, bl);
    decode(sender, bl);
    DECODE_FINISH(bl);
  }
};
WRITE_CLASS_ENCODER(MotrWatchNotifyMsg)

// The notification sent by cache to inform changes on
// an object's metadata.
class MotrCacheNotif : public MotrWatchNotifyMsg {
protected:
  // Key of the cache item.
  std::string key;
  // Op to the cached item.
  int op;

public:
  MotrCacheNotif(std::string _maker, std::string _sender, const std::string& name, int cache_op) :
    MotrWatchNotifyMsg(_maker, _sender)
  {
    key.assign(name);
    op = cache_op;
  }
  MotrCacheNotif(std::string _marker) : MotrWatchNotifyMsg(_marker) {}
  MotrCacheNotif() {}

  virtual void encode(bufferlist& bl) const {
    MotrWatchNotifyMsg::encode(bl);

    ENCODE_START(2, 2, bl);
    encode(key, bl);
    encode(op, bl);
    ENCODE_FINISH(bl);
  };

  virtual void decode(bufferlist::const_iterator& bl) {
    MotrWatchNotifyMsg::decode(bl);

    DECODE_START(2, bl);
    decode(key, bl);
    decode(op, bl);
    DECODE_FINISH(bl);
  }

  std::string& get_key() { return key; }
  int get_op() { return op; }
  std::string& get_notifier() { return this->sender; }
};
WRITE_CLASS_ENCODER(MotrCacheNotif)

// MotrWatcher registers a FDMI filter and a FDMI callback function.
// When FDMI records (picked up by the filter) arrive, the callback function
// process the records and retrieve the information sent by the notifier.
enum {
	RGW_MOTR_WATCHER_OP_UPDATE = M0_CAS_PUT_FOP_OPCODE,
	RGW_MOTR_WATCHER_OP_DEL = M0_CAS_DEL_FOP_OPCODE
};
class MotrWatcher {
protected:
  CephContext *cctx{nullptr};
  const struct m0_fdmi_pd_ops *fdmi_dock_ops{nullptr};
  struct m0_fid fdmi_plugin_fid;
  struct m0_fdmi_plugin_ops fdmi_plugin_cb;

  // The list of notifiers that it doesn't want to receive notifications
  // from. For example, a metadata cache has a pair of notifier and watcher.
  // The watcher has to exclude the notifications sent by its friend notifier.
  std::list<std::string> excluded_notifiers;

public:
  MotrWatcher(CephContext *_cctx) : cctx(_cctx) {}
  MotrWatcher() {}
  virtual ~MotrWatcher() {}
  // Callback function of the watcher's user. For example, if watcher is
  // user by object metadata cache, this callback function is called to
  // trigger corresponding cache actions.
  virtual int watch_cb(uint32_t op, bufferlist& bl) = 0;
  int init_fdmi_plugin(const DoutPrefixProvider *dpp);

  void exclude_notifier(std::string notifier) {
    excluded_notifiers.push_back(notifier);
  }

  bool is_excluded_notifier(std::string& notifier) {
    auto iter = std::find(excluded_notifiers.begin(), excluded_notifiers.end(), notifier);
    return iter == excluded_notifiers.end()? false : true;
  }
};

class MotrMetaCache;
class MotrCacheWatcher : public MotrWatcher {
protected:
  MotrMetaCache *cache;

public:
  MotrCacheWatcher(CephContext *_cctx, MotrMetaCache* _cache) : MotrWatcher(_cctx), cache(_cache){}
  virtual int watch_cb(uint32_t op, bufferlist& bl) override;
};

class MotrStore;
class MotrNotifier {
protected:
  MotrStore *store;

  int nr_notif_indices;
  struct m0_fid *notif_indices;

  // instance is a random string, name + instance uniquely
  // identifier a notifier.
  std::string name;
  std::string instance;

public:
  MotrNotifier(MotrStore* _store, int _nr_indices, const std::string& _name) {
    store = _store;
    nr_notif_indices = _nr_indices;
    name.assign(_name);
  }
  int init(const DoutPrefixProvider *dpp);
  int notify(const DoutPrefixProvider *dpp, const std::string& key, MotrWatchNotifyMsg& msg);
  std::string& get_name() { return name; }
  std::string get_key() { return name + instance; }
};

// A simplified metadata cache implementation.
// Note: MotrObjMetaCache doesn't handle the IO operations to Motr. A proxy
// class can be added to handle cache and 'real' ops.
class MotrMetaCache
{
protected:
  // MGW re-uses ObjectCache to cache object's metadata as it has already
  // implemented a lru cache: (1) ObjectCache internally uses a map and lru
  // list to manage cache entry. POC uses object name, user name or bucket
  // name as the key to lookup and insert an entry. (2) ObjectCache::data is
  // a bufferlist and can be used to store any metadata structure, such as
  // object's bucket dir entry, user info or bucket instance.
  //
  // Note from RGW:
  // The Rados Gateway stores metadata and objects in an internal cache. This
  // should be kept consistent by the OSD's relaying notify events between
  // multiple watching RGW processes. In the event that this notification
  // protocol fails, bounding the length of time that any data in the cache will
  // be assumed valid will ensure that any RGW instance that falls out of sync
  // will eventually recover. This seems to be an issue mostly for large numbers
  // of RGW instances under heavy use. If you would like to turn off cache expiry,
  // set this value to zero.
  //
  // POC implemented a simple watch-notify menchanism using FDMI.
  //
  // Beaware: Motr object data is not cached in current POC as RGW!
  // RGW caches the first chunk (4MB by default).
  ObjectCache cache;
  MotrCacheWatcher *watcher{nullptr};
  MotrNotifier *notifier{nullptr};

public:
  // Lookup a cache entry.
  int get(const DoutPrefixProvider *dpp, const std::string& name, bufferlist& data);

  // Insert a cache entry.
  int put(const DoutPrefixProvider *dpp, const std::string& name,
          bufferlist& data, bool data_incl_notif);

  // Called when an object is deleted. Notification should be sent to other
  // RGW instances.
  int remove(const DoutPrefixProvider *dpp, const std::string& name);

  // Make the local cache entry invalid.
  void invalid(const DoutPrefixProvider *dpp, const std::string& name);

  void set_enabled(bool status);

  // As explained above, metadata cache doesn't use dedicated
  // indices to pass notification. Instead, it makes use of the
  // metada index. For example, bucket index for object metadata.
  // attach_cache_notifi() is to attach a notification a key/value pair
  // when an object's metadata is changed. The notification contains
  // fdmi filter string which will trigger a fdmi event. The fdmi event
  // will be monitored by watchers of other RGW instance. Once receiving
  // fdmi event, the callback function of the watcher is invoked to retrieve
  // the notification and to take action on cache item accordingly.
  int attach_cache_notif(const DoutPrefixProvider *dpp,
                         const std::string& name, int op,
		         bufferlist& bl);

  int init_watcher_notifier(const DoutPrefixProvider *dpp, CephContext *cctx,
                            MotrStore *store, int nr_indices, const std::string notifier_name)
  {
    watcher = new MotrCacheWatcher(cctx, this);
    notifier = new MotrNotifier(store, nr_indices, notifier_name);

    int rc = watcher->init_fdmi_plugin(dpp)? : notifier->init(dpp);
    if (rc == 0)
      watcher->exclude_notifier(notifier->get_key());
    return rc;
  }

  MotrMetaCache(const DoutPrefixProvider *dpp, CephContext *cctx) {
    cache.set_ctx(cctx);
  }

};

struct MotrUserInfo {
  RGWUserInfo info;
  obj_version user_version;
  rgw::sal::Attrs attrs;

  void encode(bufferlist& bl)  const
  {
    ENCODE_START(3, 3, bl);
    encode(info, bl);
    encode(user_version, bl);
    encode(attrs, bl);
    ENCODE_FINISH(bl);
  }

  void decode(bufferlist::const_iterator& bl)
  {
    DECODE_START(3, bl);
    decode(info, bl);
    decode(user_version, bl);
    decode(attrs, bl);
    DECODE_FINISH(bl);
  }
};
WRITE_CLASS_ENCODER(MotrUserInfo);

class MotrNotification : public Notification {
  public:
    MotrNotification(Object* _obj, Object* _src_obj, rgw::notify::EventType _type) :
        Notification(_obj, _src_obj, _type) {}
    ~MotrNotification() = default;

    virtual int publish_reserve(const DoutPrefixProvider *dpp, RGWObjTags* obj_tags = nullptr) override { return 0;}
    virtual int publish_commit(const DoutPrefixProvider* dpp, uint64_t size,
			       const ceph::real_time& mtime, const std::string& etag, const std::string& version) override { return 0; }
};

class MotrUser : public User {
  private:
    MotrStore         *store;
    struct m0_uint128  idxID = {0xe5ecb53640d4ecce, 0x6a156cd5a74aa3b8}; // MD5 of “motr.rgw.users“
    struct m0_idx      idx;

  public:
    MotrUser(MotrStore *_st, const rgw_user& _u) : User(_u), store(_st) { }
    MotrUser(MotrStore *_st, const RGWUserInfo& _i) : User(_i), store(_st) { }
    MotrUser(MotrStore *_st) : store(_st) { }
    MotrUser(MotrUser& _o) = default;
    MotrUser() {}

    virtual std::unique_ptr<User> clone() override {
      return std::unique_ptr<User>(new MotrUser(*this));
    }
    int list_buckets(const DoutPrefixProvider *dpp, const std::string& marker, const std::string& end_marker,
        uint64_t max, bool need_stats, BucketList& buckets, optional_yield y) override;
    virtual int create_bucket(const DoutPrefixProvider* dpp,
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
                            std::unique_ptr<Bucket>* bucket,
                            optional_yield y) override;
    virtual int read_attrs(const DoutPrefixProvider* dpp, optional_yield y) override;
    virtual int read_stats(const DoutPrefixProvider *dpp,
        optional_yield y, RGWStorageStats* stats,
        ceph::real_time *last_stats_sync = nullptr,
        ceph::real_time *last_stats_update = nullptr) override;
    virtual int read_stats_async(const DoutPrefixProvider *dpp, RGWGetUserStats_CB* cb) override;
    virtual int complete_flush_stats(const DoutPrefixProvider *dpp, optional_yield y) override;
    virtual int read_usage(const DoutPrefixProvider *dpp, uint64_t start_epoch, uint64_t end_epoch, uint32_t max_entries,
        bool* is_truncated, RGWUsageIter& usage_iter,
        std::map<rgw_user_bucket, rgw_usage_log_entry>& usage) override;
    virtual int trim_usage(const DoutPrefixProvider *dpp, uint64_t start_epoch, uint64_t end_epoch) override;

    virtual int load_user(const DoutPrefixProvider* dpp, optional_yield y) override;
    virtual int store_user(const DoutPrefixProvider* dpp, optional_yield y, bool exclusive, RGWUserInfo* old_info = nullptr) override;
    virtual int remove_user(const DoutPrefixProvider* dpp, optional_yield y) override;

    int create_user_info_idx();

    friend class MotrBucket;
};

class MotrBucket : public Bucket {
  private:
    MotrStore *store;
    RGWAccessControlPolicy acls;

  // RGWBucketInfo and other information that are shown when listing a bucket is
  // represented in struct MotrBucketInfo. The structure is encoded and stored
  // as the value of the global bucket instance index.
  // TODO: compare pros and cons of separating the bucket_attrs (ACLs, tag etc.)
  // into a different index.
  struct MotrBucketInfo {
    RGWBucketInfo info;

    obj_version bucket_version;
    ceph::real_time mtime;

    rgw::sal::Attrs bucket_attrs;

    void encode(bufferlist& bl)  const
    {
      ENCODE_START(4, 4, bl);
      encode(info, bl);
      encode(bucket_version, bl);
      encode(mtime, bl);
      encode(bucket_attrs, bl); //rgw_cache.h example for a map
      ENCODE_FINISH(bl);
    }

    void decode(bufferlist::const_iterator& bl)
    {
      DECODE_START(4, bl);
      decode(info, bl);
      decode(bucket_version, bl);
      decode(mtime, bl);
      decode(bucket_attrs, bl);
      DECODE_FINISH(bl);
    }
  };
  WRITE_CLASS_ENCODER(MotrBucketInfo);

  public:
    MotrBucket(MotrStore *_st)
      : store(_st),
      acls() {
      }

    MotrBucket(MotrStore *_st, User* _u)
      : Bucket(_u),
      store(_st),
      acls() {
      }

    MotrBucket(MotrStore *_st, const rgw_bucket& _b)
      : Bucket(_b),
      store(_st),
      acls() {
      }

    MotrBucket(MotrStore *_st, const RGWBucketEnt& _e)
      : Bucket(_e),
      store(_st),
      acls() {
      }

    MotrBucket(MotrStore *_st, const RGWBucketInfo& _i)
      : Bucket(_i),
      store(_st),
      acls() {
      }

    MotrBucket(MotrStore *_st, const rgw_bucket& _b, User* _u)
      : Bucket(_b, _u),
      store(_st),
      acls() {
      }

    MotrBucket(MotrStore *_st, const RGWBucketEnt& _e, User* _u)
      : Bucket(_e, _u),
      store(_st),
      acls() {
      }

    MotrBucket(MotrStore *_st, const RGWBucketInfo& _i, User* _u)
      : Bucket(_i, _u),
      store(_st),
      acls() {
      }

    ~MotrBucket() { }

    virtual std::unique_ptr<Object> get_object(const rgw_obj_key& k) override;
    virtual int list(const DoutPrefixProvider *dpp, ListParams&, int, ListResults&, optional_yield y) override;
    virtual int remove_bucket(const DoutPrefixProvider *dpp, bool delete_children, bool forward_to_master, req_info* req_info, optional_yield y) override;
    virtual int remove_bucket_bypass_gc(int concurrent_max, bool
        keep_index_consistent,
        optional_yield y, const
        DoutPrefixProvider *dpp) override;
    virtual RGWAccessControlPolicy& get_acl(void) override { return acls; }
    virtual int set_acl(const DoutPrefixProvider *dpp, RGWAccessControlPolicy& acl, optional_yield y) override;
    virtual int load_bucket(const DoutPrefixProvider *dpp, optional_yield y) override;
    int link_user(const DoutPrefixProvider* dpp, User* new_user, optional_yield y);
    int unlink_user(const DoutPrefixProvider* dpp, User* new_user, optional_yield y);
    int create_bucket_index();
    int create_multipart_indices();
    virtual int read_stats(const DoutPrefixProvider *dpp, int shard_id,
        std::string *bucket_ver, std::string *master_ver,
        std::map<RGWObjCategory, RGWStorageStats>& stats,
        std::string *max_marker = nullptr,
        bool *syncstopped = nullptr) override;
    virtual int read_stats_async(const DoutPrefixProvider *dpp, int shard_id, RGWGetBucketStats_CB* ctx) override;
    virtual int sync_user_stats(const DoutPrefixProvider *dpp, optional_yield y) override;
    virtual int update_container_stats(const DoutPrefixProvider *dpp) override;
    virtual int check_bucket_shards(const DoutPrefixProvider *dpp) override;
    virtual int chown(const DoutPrefixProvider *dpp, User* new_user, User* old_user, optional_yield y, const std::string* marker = nullptr) override;
    virtual int put_info(const DoutPrefixProvider *dpp, bool exclusive, ceph::real_time mtime) override;
    virtual bool is_owner(User* user) override;
    virtual int check_empty(const DoutPrefixProvider *dpp, optional_yield y) override;
    virtual int check_quota(const DoutPrefixProvider *dpp, RGWQuotaInfo& user_quota, RGWQuotaInfo& bucket_quota, uint64_t obj_size, optional_yield y, bool check_size_only = false) override;
    virtual int merge_and_store_attrs(const DoutPrefixProvider *dpp, Attrs& attrs, optional_yield y) override;
    virtual int try_refresh_info(const DoutPrefixProvider *dpp, ceph::real_time *pmtime) override;
    virtual int read_usage(const DoutPrefixProvider *dpp, uint64_t start_epoch, uint64_t end_epoch, uint32_t max_entries,
        bool *is_truncated, RGWUsageIter& usage_iter,
        std::map<rgw_user_bucket, rgw_usage_log_entry>& usage) override;
    virtual int trim_usage(const DoutPrefixProvider *dpp, uint64_t start_epoch, uint64_t end_epoch) override;
    virtual int remove_objs_from_index(const DoutPrefixProvider *dpp, std::list<rgw_obj_index_key>& objs_to_unlink) override;
    virtual int check_index(const DoutPrefixProvider *dpp, std::map<RGWObjCategory, RGWStorageStats>& existing_stats, std::map<RGWObjCategory, RGWStorageStats>& calculated_stats) override;
    virtual int rebuild_index(const DoutPrefixProvider *dpp) override;
    virtual int set_tag_timeout(const DoutPrefixProvider *dpp, uint64_t timeout) override;
    virtual int purge_instance(const DoutPrefixProvider *dpp) override;
    virtual std::unique_ptr<Bucket> clone() override {
      return std::make_unique<MotrBucket>(*this);
    }
    virtual std::unique_ptr<MultipartUpload> get_multipart_upload(const std::string& oid,
                                std::optional<std::string> upload_id=std::nullopt,
                                ACLOwner owner={}, ceph::real_time mtime=real_clock::now()) override;
    virtual int list_multiparts(const DoutPrefixProvider *dpp,
      const std::string& prefix,
      std::string& marker,
      const std::string& delim,
      const int& max_uploads,
      std::vector<std::unique_ptr<MultipartUpload>>& uploads,
      std::map<std::string, bool> *common_prefixes,
      bool *is_truncated) override;
    virtual int abort_multiparts(const DoutPrefixProvider *dpp, CephContext *cct) override;

    friend class MotrStore;
};

class MotrZone : public Zone {
  protected:
    MotrStore* store;
    RGWRealm *realm{nullptr};
    RGWZoneGroup *zonegroup{nullptr};
    RGWZone *zone_public_config{nullptr}; /* external zone params, e.g., entrypoints, log flags, etc. */
    RGWZoneParams *zone_params{nullptr}; /* internal zone params, e.g., rados pools */
    RGWPeriod *current_period{nullptr};
    rgw_zone_id cur_zone_id;

  public:
    MotrZone(MotrStore* _store) : store(_store) {
      realm = new RGWRealm();
      zonegroup = new RGWZoneGroup();
      zone_public_config = new RGWZone();
      zone_params = new RGWZoneParams();
      current_period = new RGWPeriod();
      cur_zone_id = rgw_zone_id(zone_params->get_id());

      // XXX: only default and STANDARD supported for now
      RGWZonePlacementInfo info;
      RGWZoneStorageClasses sc;
      sc.set_storage_class("STANDARD", nullptr, nullptr);
      info.storage_classes = sc;
      zone_params->placement_pools["default"] = info;
    }
    ~MotrZone() = default;

    virtual const RGWZoneGroup& get_zonegroup() override;
    virtual int get_zonegroup(const std::string& id, RGWZoneGroup& zonegroup) override;
    virtual const RGWZoneParams& get_params() override;
    virtual const rgw_zone_id& get_id() override;
    virtual const RGWRealm& get_realm() override;
    virtual const std::string& get_name() const override;
    virtual bool is_writeable() override;
    virtual bool get_redirect_endpoint(std::string* endpoint) override;
    virtual bool has_zonegroup_api(const std::string& api) const override;
    virtual const std::string& get_current_period_id() override;
};

class MotrLuaScriptManager : public LuaScriptManager {
  MotrStore* store;

  public:
  MotrLuaScriptManager(MotrStore* _s) : store(_s)
  {
  }
  virtual ~MotrLuaScriptManager() = default;

  virtual int get(const DoutPrefixProvider* dpp, optional_yield y, const std::string& key, std::string& script) override { return -ENOENT; }
  virtual int put(const DoutPrefixProvider* dpp, optional_yield y, const std::string& key, const std::string& script) override { return -ENOENT; }
  virtual int del(const DoutPrefixProvider* dpp, optional_yield y, const std::string& key) override { return -ENOENT; }
};

class MotrOIDCProvider : public RGWOIDCProvider {
  MotrStore* store;
  public:
  MotrOIDCProvider(MotrStore* _store) : store(_store) {}
  ~MotrOIDCProvider() = default;

  virtual int store_url(const DoutPrefixProvider *dpp, const std::string& url, bool exclusive, optional_yield y) override { return 0; }
  virtual int read_url(const DoutPrefixProvider *dpp, const std::string& url, const std::string& tenant) override { return 0; }
  virtual int delete_obj(const DoutPrefixProvider *dpp, optional_yield y) override { return 0;}

  void encode(bufferlist& bl) const {
    RGWOIDCProvider::encode(bl);
  }
  void decode(bufferlist::const_iterator& bl) {
    RGWOIDCProvider::decode(bl);
  }
};

class MotrObject : public Object {
  private:
    MotrStore *store;
    RGWAccessControlPolicy acls;
    /* XXX: to be removed. Till Dan's patch comes, a placeholder
     * for RGWObjState
     */
    RGWObjState *state;

    RGWObjCategory category;

    // If this object is pat of a multipart uploaded one.
    // TODO: do it in another class? MotrPartObject : public MotrObject
    uint64_t part_off;
    uint64_t part_size;
    uint64_t part_num;

  public:

    // motr object metadata stored in index
    struct Meta {
      struct m0_uint128 oid = {};
      struct m0_fid pver = {};
      uint64_t layout_id = 0;

      void encode(bufferlist& bl) const
      {
        ENCODE_START(5, 5, bl);
        encode(oid.u_hi, bl);
        encode(oid.u_lo, bl);
        encode(pver.f_container, bl);
        encode(pver.f_key, bl);
        encode(layout_id, bl);
        ENCODE_FINISH(bl);
      }

      void decode(bufferlist::const_iterator& bl)
      {
        DECODE_START(5, bl);
        decode(oid.u_hi, bl);
        decode(oid.u_lo, bl);
        decode(pver.f_container, bl);
        decode(pver.f_key, bl);
        decode(layout_id, bl);
        DECODE_FINISH(bl);
      }
    };

    struct m0_obj     *mobj = NULL;
    Meta               meta;

    struct MotrReadOp : public ReadOp {
      private:
        MotrObject* source;
        RGWObjectCtx* rctx;

	// The set of part objects if the source is
	// a multipart uploaded object.
        std::map<int, std::unique_ptr<MotrObject>> part_objs;

      public:
        MotrReadOp(MotrObject *_source, RGWObjectCtx *_rctx);

        virtual int prepare(optional_yield y, const DoutPrefixProvider* dpp) override;
        virtual int read(int64_t off, int64_t end, bufferlist& bl, optional_yield y, const DoutPrefixProvider* dpp) override;
        virtual int iterate(const DoutPrefixProvider* dpp, int64_t off, int64_t end, RGWGetDataCB* cb, optional_yield y) override;
        virtual int get_attr(const DoutPrefixProvider* dpp, const char* name, bufferlist& dest, optional_yield y) override;
    };

    struct MotrDeleteOp : public DeleteOp {
      private:
        MotrObject* source;
        RGWObjectCtx* rctx;

      public:
        MotrDeleteOp(MotrObject* _source, RGWObjectCtx* _rctx);

        virtual int delete_obj(const DoutPrefixProvider* dpp, optional_yield y) override;
    };

    MotrObject() = default;

    MotrObject(MotrStore *_st, const rgw_obj_key& _k)
      : Object(_k), store(_st), acls(), state(NULL) {}
    MotrObject(MotrStore *_st, const rgw_obj_key& _k, Bucket* _b)
      : Object(_k, _b), store(_st), acls(), state(NULL) {}

    MotrObject(MotrObject& _o) = default;

    virtual ~MotrObject();

    virtual int delete_object(const DoutPrefixProvider* dpp,
        RGWObjectCtx* obj_ctx,
        optional_yield y,
        bool prevent_versioning = false) override;
    virtual int delete_obj_aio(const DoutPrefixProvider* dpp, RGWObjState* astate, Completions* aio,
        bool keep_index_consistent, optional_yield y) override;
    virtual int copy_object(RGWObjectCtx& obj_ctx, User* user,
        req_info* info, const rgw_zone_id& source_zone,
        rgw::sal::Object* dest_object, rgw::sal::Bucket* dest_bucket,
        rgw::sal::Bucket* src_bucket,
        const rgw_placement_rule& dest_placement,
        ceph::real_time* src_mtime, ceph::real_time* mtime,
        const ceph::real_time* mod_ptr, const ceph::real_time* unmod_ptr,
        bool high_precision_time,
        const char* if_match, const char* if_nomatch,
        AttrsMod attrs_mod, bool copy_if_newer, Attrs& attrs,
        RGWObjCategory category, uint64_t olh_epoch,
        boost::optional<ceph::real_time> delete_at,
        std::string* version_id, std::string* tag, std::string* etag,
        void (*progress_cb)(off_t, void *), void* progress_data,
        const DoutPrefixProvider* dpp, optional_yield y) override;
    virtual RGWAccessControlPolicy& get_acl(void) override { return acls; }
    virtual int set_acl(const RGWAccessControlPolicy& acl) override { acls = acl; return 0; }
    virtual void set_atomic(RGWObjectCtx* rctx) const override;
    virtual void set_prefetch_data(RGWObjectCtx* rctx) override;
    virtual void set_compressed(RGWObjectCtx* rctx) override;

    virtual int get_obj_state(const DoutPrefixProvider* dpp, RGWObjectCtx* rctx, RGWObjState **state, optional_yield y, bool follow_olh = true) override;
    virtual int set_obj_attrs(const DoutPrefixProvider* dpp, RGWObjectCtx* rctx, Attrs* setattrs, Attrs* delattrs, optional_yield y, rgw_obj* target_obj = NULL) override;
    virtual int get_obj_attrs(RGWObjectCtx* rctx, optional_yield y, const DoutPrefixProvider* dpp, rgw_obj* target_obj = NULL) override;
    virtual int modify_obj_attrs(RGWObjectCtx* rctx, const char* attr_name, bufferlist& attr_val, optional_yield y, const DoutPrefixProvider* dpp) override;
    virtual int delete_obj_attrs(const DoutPrefixProvider* dpp, RGWObjectCtx* rctx, const char* attr_name, optional_yield y) override;
    virtual bool is_expired() override;
    virtual void gen_rand_obj_instance_name() override;
    virtual std::unique_ptr<Object> clone() override {
      return std::unique_ptr<Object>(new MotrObject(*this));
    }
    virtual MPSerializer* get_serializer(const DoutPrefixProvider *dpp, const std::string& lock_name) override;
    virtual int transition(RGWObjectCtx& rctx,
        Bucket* bucket,
        const rgw_placement_rule& placement_rule,
        const real_time& mtime,
        uint64_t olh_epoch,
        const DoutPrefixProvider* dpp,
        optional_yield y) override;
    virtual bool placement_rules_match(rgw_placement_rule& r1, rgw_placement_rule& r2) override;
    virtual int dump_obj_layout(const DoutPrefixProvider *dpp, optional_yield y, Formatter* f, RGWObjectCtx* obj_ctx) override;

    /* Swift versioning */
    virtual int swift_versioning_restore(RGWObjectCtx* obj_ctx,
        bool& restored,
        const DoutPrefixProvider* dpp) override;
    virtual int swift_versioning_copy(RGWObjectCtx* obj_ctx,
        const DoutPrefixProvider* dpp,
        optional_yield y) override;

    /* OPs */
    virtual std::unique_ptr<ReadOp> get_read_op(RGWObjectCtx *) override;
    virtual std::unique_ptr<DeleteOp> get_delete_op(RGWObjectCtx*) override;

    /* OMAP */
    virtual int omap_get_vals(const DoutPrefixProvider *dpp, const std::string& marker, uint64_t count,
        std::map<std::string, bufferlist> *m,
        bool* pmore, optional_yield y) override;
    virtual int omap_get_all(const DoutPrefixProvider *dpp, std::map<std::string, bufferlist> *m,
        optional_yield y) override;
    virtual int omap_get_vals_by_keys(const DoutPrefixProvider *dpp, const std::string& oid,
        const std::set<std::string>& keys,
        Attrs* vals) override;
    virtual int omap_set_val_by_key(const DoutPrefixProvider *dpp, const std::string& key, bufferlist& val,
        bool must_exist, optional_yield y) override;
  private:
    //int read_attrs(const DoutPrefixProvider* dpp, Motr::Object::Read &read_op, optional_yield y, rgw_obj* target_obj = nullptr);

  public:
    bool is_opened() { return mobj != NULL; }
    int create_mobj(const DoutPrefixProvider *dpp, uint64_t sz);
    int open_mobj(const DoutPrefixProvider *dpp);
    int delete_mobj(const DoutPrefixProvider *dpp);
    void close_mobj();
    int write_mobj(const DoutPrefixProvider *dpp, bufferlist&& data, uint64_t offset);
    int read_mobj(const DoutPrefixProvider* dpp, int64_t off, int64_t end, RGWGetDataCB* cb);
    unsigned get_optimal_bs(unsigned len);

    int get_part_objs(const DoutPrefixProvider *dpp,
                      std::map<int, std::unique_ptr<MotrObject>>& part_objs);
    int open_part_objs(const DoutPrefixProvider* dpp,
                       std::map<int, std::unique_ptr<MotrObject>>& part_objs);
    int read_multipart_obj(const DoutPrefixProvider* dpp,
                           int64_t off, int64_t end, RGWGetDataCB* cb,
                           std::map<int, std::unique_ptr<MotrObject>>& part_objs);
    int delete_part_objs(const DoutPrefixProvider* dpp);
    void set_category(RGWObjCategory _category) {category = _category;}
    int get_bucket_dir_ent(const DoutPrefixProvider *dpp, rgw_bucket_dir_entry& ent);
    int update_version_entries(const DoutPrefixProvider *dpp);
};

// A placeholder locking class for multipart upload.
// TODO: implement it using Motr object locks.
class MPMotrSerializer : public MPSerializer {

  public:
    MPMotrSerializer(const DoutPrefixProvider *dpp, MotrStore* store, MotrObject* obj, const std::string& lock_name) {}

    virtual int try_lock(const DoutPrefixProvider *dpp, utime_t dur, optional_yield y) override {return 0; }
    virtual int unlock() override { return 0;}
};

class MotrAtomicWriter : public Writer {
  protected:
  rgw::sal::MotrStore* store;
  const rgw_user& owner;
  const rgw_placement_rule *ptail_placement_rule;
  uint64_t olh_epoch;
  const std::string& unique_tag;
  MotrObject obj;
  uint64_t total_data_size; // for total data being uploaded
  bufferlist acc_bl;  // accumulated data
  uint64_t   acc_off; // accumulated data offset

  struct m0_bufvec buf;
  struct m0_bufvec attr;
  struct m0_indexvec ext;

  public:
  MotrAtomicWriter(const DoutPrefixProvider *dpp,
          optional_yield y,
          std::unique_ptr<rgw::sal::Object> _head_obj,
          MotrStore* _store,
          const rgw_user& _owner, RGWObjectCtx& obj_ctx,
          const rgw_placement_rule *_ptail_placement_rule,
          uint64_t _olh_epoch,
          const std::string& _unique_tag);
  ~MotrAtomicWriter() = default;

  // prepare to start processing object data
  virtual int prepare(optional_yield y) override;

  // Process a bufferlist
  virtual int process(bufferlist&& data, uint64_t offset) override;

  int write();

  // complete the operation and make its result visible to clients
  virtual int complete(size_t accounted_size, const std::string& etag,
                       ceph::real_time *mtime, ceph::real_time set_mtime,
                       std::map<std::string, bufferlist>& attrs,
                       ceph::real_time delete_at,
                       const char *if_match, const char *if_nomatch,
                       const std::string *user_data,
                       rgw_zone_set *zones_trace, bool *canceled,
                       optional_yield y) override;

  unsigned populate_bvec(unsigned len, bufferlist::iterator &bi);
  void cleanup();
};

class MotrMultipartWriter : public Writer {
protected:
  rgw::sal::MotrStore* store;

  // Head object.
  std::unique_ptr<rgw::sal::Object> head_obj;

  // Part parameters.
  const uint64_t part_num;
  const std::string part_num_str;
  std::unique_ptr<MotrObject> part_obj;
  uint64_t actual_part_size = 0;

public:
  MotrMultipartWriter(const DoutPrefixProvider *dpp,
		       optional_yield y, MultipartUpload* upload,
		       std::unique_ptr<rgw::sal::Object> _head_obj,
		       MotrStore* _store,
		       const rgw_user& owner, RGWObjectCtx& obj_ctx,
		       const rgw_placement_rule *ptail_placement_rule,
		       uint64_t _part_num, const std::string& part_num_str) :
				  Writer(dpp, y), store(_store), head_obj(std::move(_head_obj)),
				  part_num(_part_num), part_num_str(part_num_str)
  {
  }
  ~MotrMultipartWriter() = default;

  // prepare to start processing object data
  virtual int prepare(optional_yield y) override;

  // Process a bufferlist
  virtual int process(bufferlist&& data, uint64_t offset) override;

  // complete the operation and make its result visible to clients
  virtual int complete(size_t accounted_size, const std::string& etag,
                       ceph::real_time *mtime, ceph::real_time set_mtime,
                       std::map<std::string, bufferlist>& attrs,
                       ceph::real_time delete_at,
                       const char *if_match, const char *if_nomatch,
                       const std::string *user_data,
                       rgw_zone_set *zones_trace, bool *canceled,
                       optional_yield y) override;
};

// The implementation of multipart upload in POC roughly follows the
// cortx-s3server's design. Parts are stored in separate Motr objects.
// s3server uses a few auxiliary Motr indices to manage multipart
// related metadata: (1) Bucket multipart index (bucket_nnn_multipart_index)
// which contains metadata that answers questions such as which objects have
// started  multipart upload and its upload id. This index is created during
// bucket creation. (2) Object part index (object_nnn_part_index) which stores
// metadata of a part's details (size, pvid, oid...). This index is created in
// MotrMultipartUpload::init(). (3) Extended metadata index
// (bucket_nnn_extended_metadata): once parts has been uploaded and their
// metadata saved in the part index, the user may issue multipart completion
// request. When processing the completion request, the parts are read from
// object part index and for each part an entry is created in extended index.
// The entry for the object is created in bucket (object list) index. The part
// index is deleted and an entry removed from bucket_nnn_multipart_index. Like
// bucket multipart index, bucket part extened metadata index is created during
// bucket creation.
//
// The extended metadata index is used mainly due to fault tolerant
// considerations (how to handle Motr service crash when uploading an object)
// and to avoid to create too many Motr indices (I am not sure I understand
// why many Motr indices is bad.). In our POC, to keep it simple, only 2
// indices are maintained: bucket multipart index and object_nnn_part_index.
//
//

class MotrMultipartPart : public MultipartPart {
protected:
  RGWUploadPartInfo info;

public:
  MotrObject::Meta  meta;

  MotrMultipartPart(RGWUploadPartInfo _info, MotrObject::Meta _meta) :
    info(_info), meta(_meta) {}
  virtual ~MotrMultipartPart() = default;

  virtual uint32_t get_num() { return info.num; }
  virtual uint64_t get_size() { return info.accounted_size; }
  virtual const std::string& get_etag() { return info.etag; }
  virtual ceph::real_time& get_mtime() { return info.modified; }

  RGWObjManifest& get_manifest() { return info.manifest; }

  friend class MotrMultipartUpload;
};

class MotrMultipartUpload : public MultipartUpload {
  MotrStore* store;
  RGWMPObj mp_obj;
  ACLOwner owner;
  ceph::real_time mtime;
  rgw_placement_rule placement;
  RGWObjManifest manifest;

public:
  MotrMultipartUpload(MotrStore* _store, Bucket* _bucket, const std::string& oid,
                      std::optional<std::string> upload_id, ACLOwner _owner, ceph::real_time _mtime) :
       MultipartUpload(_bucket), store(_store), mp_obj(oid, upload_id), owner(_owner), mtime(_mtime) {}
  virtual ~MotrMultipartUpload() = default;

  virtual const std::string& get_meta() const { return mp_obj.get_meta(); }
  virtual const std::string& get_key() const { return mp_obj.get_key(); }
  virtual const std::string& get_upload_id() const { return mp_obj.get_upload_id(); }
  virtual const ACLOwner& get_owner() const override { return owner; }
  virtual ceph::real_time& get_mtime() { return mtime; }
  virtual std::unique_ptr<rgw::sal::Object> get_meta_obj() override;
  virtual int init(const DoutPrefixProvider* dpp, optional_yield y, RGWObjectCtx* obj_ctx, ACLOwner& owner, rgw_placement_rule& dest_placement, rgw::sal::Attrs& attrs) override;
  virtual int list_parts(const DoutPrefixProvider* dpp, CephContext* cct,
			 int num_parts, int marker,
			 int* next_marker, bool* truncated,
			 bool assume_unsorted = false) override;
  virtual int abort(const DoutPrefixProvider* dpp, CephContext* cct,
		    RGWObjectCtx* obj_ctx) override;
  virtual int complete(const DoutPrefixProvider* dpp,
		       optional_yield y, CephContext* cct,
		       std::map<int, std::string>& part_etags,
		       std::list<rgw_obj_index_key>& remove_objs,
		       uint64_t& accounted_size, bool& compressed,
		       RGWCompressionInfo& cs_info, off_t& off,
		       std::string& tag, ACLOwner& owner,
		       uint64_t olh_epoch,
		       rgw::sal::Object* target_obj,
		       RGWObjectCtx* obj_ctx) override;
  virtual int get_info(const DoutPrefixProvider *dpp, optional_yield y, RGWObjectCtx* obj_ctx, rgw_placement_rule** rule, rgw::sal::Attrs* attrs = nullptr) override;
  virtual std::unique_ptr<Writer> get_writer(const DoutPrefixProvider *dpp,
			  optional_yield y,
			  std::unique_ptr<rgw::sal::Object> _head_obj,
			  const rgw_user& owner, RGWObjectCtx& obj_ctx,
			  const rgw_placement_rule *ptail_placement_rule,
			  uint64_t part_num,
			  const std::string& part_num_str) override;
  int delete_parts(const DoutPrefixProvider *dpp);
};

class MotrStore : public Store {
  private:
    std::string luarocks_path;
    MotrZone zone;
    RGWSyncModuleInstanceRef sync_module;

    MotrMetaCache* obj_meta_cache;
    MotrMetaCache* user_cache;
    MotrMetaCache* bucket_inst_cache;

  public:
    CephContext *cctx;
    struct m0_client   *instance;
    struct m0_container container;
    struct m0_realm     uber_realm;
    struct m0_config    conf = {};
    struct m0_idx_dix_config dix_conf = {};
    struct m0_reqh_service *fdmi_service;

    MotrStore(CephContext *c): zone(this), cctx(c) {}
    ~MotrStore() {
      delete obj_meta_cache;
      delete user_cache;
      delete bucket_inst_cache;
    }

    virtual const char* get_name() const override {
      return "motr";
    }

    virtual std::unique_ptr<User> get_user(const rgw_user& u) override;
    virtual std::string get_cluster_id(const DoutPrefixProvider* dpp,  optional_yield y) override;
    virtual int get_user_by_access_key(const DoutPrefixProvider *dpp, const std::string& key, optional_yield y, std::unique_ptr<User>* user) override;
    virtual int get_user_by_email(const DoutPrefixProvider *dpp, const std::string& email, optional_yield y, std::unique_ptr<User>* user) override;
    virtual int get_user_by_swift(const DoutPrefixProvider *dpp, const std::string& user_str, optional_yield y, std::unique_ptr<User>* user) override;
    virtual std::unique_ptr<Object> get_object(const rgw_obj_key& k) override;
    virtual int get_bucket(const DoutPrefixProvider *dpp, User* u, const rgw_bucket& b, std::unique_ptr<Bucket>* bucket, optional_yield y) override;
    virtual int get_bucket(User* u, const RGWBucketInfo& i, std::unique_ptr<Bucket>* bucket) override;
    virtual int get_bucket(const DoutPrefixProvider *dpp, User* u, const std::string& tenant, const std::string&name, std::unique_ptr<Bucket>* bucket, optional_yield y) override;
    virtual bool is_meta_master() override;
    virtual int forward_request_to_master(const DoutPrefixProvider *dpp, User* user, obj_version* objv,
        bufferlist& in_data, JSONParser *jp, req_info& info,
        optional_yield y) override;
    virtual Zone* get_zone() { return &zone; }
    virtual std::string zone_unique_id(uint64_t unique_num) override;
    virtual std::string zone_unique_trans_id(const uint64_t unique_num) override;
    virtual int cluster_stat(RGWClusterStat& stats) override;
    virtual std::unique_ptr<Lifecycle> get_lifecycle(void) override;
    virtual std::unique_ptr<Completions> get_completions(void) override;
    virtual std::unique_ptr<Notification> get_notification(rgw::sal::Object* obj, rgw::sal::Object* src_obj,
        struct req_state* s, rgw::notify::EventType event_type, const std::string* object_name=nullptr) override;
    virtual std::unique_ptr<Notification> get_notification(const DoutPrefixProvider* dpp, rgw::sal::Object* obj,
        rgw::sal::Object* src_obj, RGWObjectCtx* rctx, rgw::notify::EventType event_type, rgw::sal::Bucket* _bucket,
        std::string& _user_id, std::string& _user_tenant, std::string& _req_id, optional_yield y) override;
    virtual RGWLC* get_rgwlc(void) override { return NULL; }
    virtual RGWCoroutinesManagerRegistry* get_cr_registry() override { return NULL; }

    virtual int log_usage(const DoutPrefixProvider *dpp, std::map<rgw_user_bucket, RGWUsageBatch>& usage_info) override;
    virtual int log_op(const DoutPrefixProvider *dpp, std::string& oid, bufferlist& bl) override;
    virtual int register_to_service_map(const DoutPrefixProvider *dpp, const std::string& daemon_type,
        const std::map<std::string, std::string>& meta) override;
    virtual void get_quota(RGWQuotaInfo& bucket_quota, RGWQuotaInfo& user_quota) override;
    virtual int set_buckets_enabled(const DoutPrefixProvider *dpp, std::vector<rgw_bucket>& buckets, bool enabled) override;
    virtual uint64_t get_new_req_id() override { return 0; }
    virtual int get_sync_policy_handler(const DoutPrefixProvider *dpp,
        std::optional<rgw_zone_id> zone,
        std::optional<rgw_bucket> bucket,
        RGWBucketSyncPolicyHandlerRef *phandler,
        optional_yield y) override;
    virtual RGWDataSyncStatusManager* get_data_sync_manager(const rgw_zone_id& source_zone) override;
    virtual void wakeup_meta_sync_shards(std::set<int>& shard_ids) override { return; }
    virtual void wakeup_data_sync_shards(const DoutPrefixProvider *dpp, const rgw_zone_id& source_zone, std::map<int, std::set<std::string> >& shard_ids) override { return; }
    virtual int clear_usage(const DoutPrefixProvider *dpp) override { return 0; }
    virtual int read_all_usage(const DoutPrefixProvider *dpp, uint64_t start_epoch, uint64_t end_epoch,
        uint32_t max_entries, bool *is_truncated,
        RGWUsageIter& usage_iter,
        std::map<rgw_user_bucket, rgw_usage_log_entry>& usage) override;
    virtual int trim_all_usage(const DoutPrefixProvider *dpp, uint64_t start_epoch, uint64_t end_epoch) override;
    virtual int get_config_key_val(std::string name, bufferlist* bl) override;
    virtual int meta_list_keys_init(const DoutPrefixProvider *dpp, const std::string& section, const std::string& marker, void** phandle) override;
    virtual int meta_list_keys_next(const DoutPrefixProvider *dpp, void* handle, int max, std::list<std::string>& keys, bool* truncated) override;
    virtual void meta_list_keys_complete(void* handle) override;
    virtual std::string meta_get_marker(void *handle) override;
    virtual int meta_remove(const DoutPrefixProvider *dpp, std::string& metadata_key, optional_yield y) override;

    virtual const RGWSyncModuleInstanceRef& get_sync_module() { return sync_module; }
    virtual std::string get_host_id() { return ""; }

    virtual std::unique_ptr<LuaScriptManager> get_lua_script_manager() override;
    virtual std::unique_ptr<RGWRole> get_role(std::string name,
        std::string tenant,
        std::string path="",
        std::string trust_policy="",
        std::string max_session_duration_str="",
        std::multimap<std::string, std::string> tags={}) override;
    virtual std::unique_ptr<RGWRole> get_role(std::string id) override;
    virtual int get_roles(const DoutPrefixProvider *dpp,
        optional_yield y,
        const std::string& path_prefix,
        const std::string& tenant,
        std::vector<std::unique_ptr<RGWRole>>& roles) override;
    virtual std::unique_ptr<RGWOIDCProvider> get_oidc_provider() override;
    virtual int get_oidc_providers(const DoutPrefixProvider *dpp,
        const std::string& tenant,
        std::vector<std::unique_ptr<RGWOIDCProvider>>& providers) override;
    virtual std::unique_ptr<Writer> get_append_writer(const DoutPrefixProvider *dpp,
        optional_yield y,
        std::unique_ptr<rgw::sal::Object> _head_obj,
        const rgw_user& owner, RGWObjectCtx& obj_ctx,
        const rgw_placement_rule *ptail_placement_rule,
        const std::string& unique_tag,
        uint64_t position,
        uint64_t *cur_accounted_size) override;
    virtual std::unique_ptr<Writer> get_atomic_writer(const DoutPrefixProvider *dpp,
        optional_yield y,
        std::unique_ptr<rgw::sal::Object> _head_obj,
        const rgw_user& owner, RGWObjectCtx& obj_ctx,
        const rgw_placement_rule *ptail_placement_rule,
        uint64_t olh_epoch,
        const std::string& unique_tag) override;

    virtual void finalize(void) override;

    virtual CephContext *ctx(void) override {
      return cctx;
    }

    virtual const std::string& get_luarocks_path() const override {
      return luarocks_path;
    }

    virtual void set_luarocks_path(const std::string& path) override {
      luarocks_path = path;
    }

    int open_idx(struct m0_uint128 *id, bool create, struct m0_idx *out);
    void close_idx(struct m0_idx *idx) { m0_idx_fini(idx); }
    int do_idx_op(struct m0_idx *, enum m0_idx_opcode opcode,
      std::vector<uint8_t>& key, std::vector<uint8_t>& val, bool update = false);

    int do_idx_next_op(struct m0_idx *idx,
                       std::vector<std::vector<uint8_t>>& key_vec,
                       std::vector<std::vector<uint8_t>>& val_vec);
    int next_query_by_name(std::string idx_name, std::vector<std::string>& key_str_vec,
                                            std::vector<bufferlist>& val_bl_vec,
                                            std::string prefix="", std::string delim="");

    void index_name_to_motr_fid(std::string iname, struct m0_uint128 *fid);
    int open_motr_idx(struct m0_uint128 *id, struct m0_idx *idx);
    int create_motr_idx_by_name(std::string iname);
    int delete_motr_idx_by_name(std::string iname);
    int do_idx_op_by_name(std::string idx_name, enum m0_idx_opcode opcode,
                          std::string key_str, bufferlist &bl, bool update=true);
    int check_n_create_global_indices();

    int init_metadata_cache(const DoutPrefixProvider *dpp, CephContext *cct);
    MotrMetaCache* get_obj_meta_cache() {return obj_meta_cache;}
    MotrMetaCache* get_user_cache() {return user_cache;}
    MotrMetaCache* get_bucket_inst_cache() {return bucket_inst_cache;}

    int fdmi_service_start(struct m0_client *m0c);
    void fdmi_service_stop(struct m0_client *m0c);
};

} // namespace rgw::sal
