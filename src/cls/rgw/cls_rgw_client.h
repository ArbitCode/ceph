// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#pragma once

#include <boost/intrusive_ptr.hpp>
#include <boost/smart_ptr/intrusive_ref_counter.hpp>
#include "include/str_list.h"
#include "include/rados/librados.hpp"
#include "cls_rgw_ops.h"
#include "cls_rgw_const.h"
#include "common/RefCountedObj.h"
#include "common/strtol.h"
#include "include/compat.h"
#include "common/ceph_time.h"
#include "common/ceph_mutex.h"


class RGWGetDirHeader_CB : public boost::intrusive_ref_counter<RGWGetDirHeader_CB> {
public:
  virtual ~RGWGetDirHeader_CB() {}
  virtual void handle_response(int r, const rgw_bucket_dir_header& header) = 0;
};

class BucketIndexShardsManager {
private:
  // Per shard setting manager, for example, marker.
  std::map<int, std::string> value_by_shards;
public:
  const static std::string KEY_VALUE_SEPARATOR;
  const static std::string SHARDS_SEPARATOR;

  void add(int shard, const std::string& value) {
    value_by_shards[shard] = value;
  }

  const std::string& get(int shard, const std::string& default_value) const {
    auto iter = value_by_shards.find(shard);
    return (iter == value_by_shards.end() ? default_value : iter->second);
  }

  const std::map<int, std::string>& get() const {
    return value_by_shards;
  }
  std::map<int, std::string>& get() {
    return value_by_shards;
  }

  bool empty() const {
    return value_by_shards.empty();
  }

  void to_string(std::string *out) const {
    if (!out) {
      return;
    }
    out->clear();
    for (auto iter = value_by_shards.begin();
	 iter != value_by_shards.end(); ++iter) {
      if (out->length()) {
        // Not the first item, append a separator first
        out->append(SHARDS_SEPARATOR);
      }
      char buf[16];
      snprintf(buf, sizeof(buf), "%d", iter->first);
      out->append(buf);
      out->append(KEY_VALUE_SEPARATOR);
      out->append(iter->second);
    }
  }

  static bool is_shards_marker(const std::string& marker) {
    return marker.find(KEY_VALUE_SEPARATOR) != std::string::npos;
  }

  /*
   * convert from std::string. There are two options of how the std::string looks like:
   *
   * 1. Single shard, no shard id specified, e.g. 000001.23.1
   *
   * for this case, if passed shard_id >= 0, use this shard id, otherwise assume that it's a
   * bucket with no shards.
   *
   * 2. One or more shards, shard id specified for each shard, e.g., 0#00002.12,1#00003.23.2
   *
   */
  int from_string(std::string_view composed_marker, int shard_id) {
    value_by_shards.clear();
    std::vector<std::string> shards;
    get_str_vec(composed_marker, SHARDS_SEPARATOR.c_str(), shards);
    if (shards.size() > 1 && shard_id >= 0) {
      return -EINVAL;
    }
    for (auto iter = shards.begin(); iter != shards.end(); ++iter) {
      size_t pos = iter->find(KEY_VALUE_SEPARATOR);
      if (pos == std::string::npos) {
        if (!value_by_shards.empty()) {
          return -EINVAL;
        }
        if (shard_id < 0) {
          add(0, *iter);
        } else {
          add(shard_id, *iter);
        }
        return 0;
      }
      std::string shard_str = iter->substr(0, pos);
      std::string err;
      int shard = (int)strict_strtol(shard_str.c_str(), 10, &err);
      if (!err.empty()) {
        return -EINVAL;
      }
      add(shard, iter->substr(pos + 1));
    }
    return 0;
  }

  // trim the '<shard-id>#' prefix from a single shard marker if present
  static std::string get_shard_marker(const std::string& marker) {
    auto p = marker.find(KEY_VALUE_SEPARATOR);
    if (p == marker.npos) {
      return marker;
    }
    return marker.substr(p + 1);
  }
};

/* bucket index */
void cls_rgw_bucket_init_index(librados::ObjectWriteOperation& o);
void cls_rgw_bucket_init_index2(librados::ObjectWriteOperation& o);

void cls_rgw_bucket_set_tag_timeout(librados::ObjectWriteOperation& op,
                                    uint64_t timeout);

void cls_rgw_bucket_update_stats(librados::ObjectWriteOperation& o,
                                 bool absolute,
                                 const std::map<RGWObjCategory, rgw_bucket_category_stats>& stats,
                                 const std::map<RGWObjCategory, rgw_bucket_category_stats>* dec_stats = nullptr);

void cls_rgw_bucket_prepare_op(librados::ObjectWriteOperation& o, RGWModifyOp op, const std::string& tag,
                               const cls_rgw_obj_key& key, const std::string& locator);

void cls_rgw_bucket_complete_op(librados::ObjectWriteOperation& o, RGWModifyOp op, const std::string& tag,
                                const rgw_bucket_entry_ver& ver,
                                const cls_rgw_obj_key& key,
                                const rgw_bucket_dir_entry_meta& dir_meta,
				const std::list<cls_rgw_obj_key> *remove_objs, bool log_op,
                                uint16_t bilog_op, const rgw_zone_set *zones_trace,
				const std::string& obj_locator = ""); // ignored if it's the empty string

void cls_rgw_remove_obj(librados::ObjectWriteOperation& o, std::list<std::string>& keep_attr_prefixes);
void cls_rgw_obj_store_pg_ver(librados::ObjectWriteOperation& o, const std::string& attr);
void cls_rgw_obj_check_attrs_prefix(librados::ObjectOperation& o, const std::string& prefix, bool fail_if_exist);
void cls_rgw_obj_check_mtime(librados::ObjectOperation& o, const ceph::real_time& mtime, bool high_precision_time, RGWCheckMTimeType type);

int cls_rgw_bi_get(librados::IoCtx& io_ctx, const std::string oid,
                   BIIndexType index_type, const cls_rgw_obj_key& key,
                   rgw_cls_bi_entry *entry);
int cls_rgw_bi_put(librados::IoCtx& io_ctx, const std::string oid, const rgw_cls_bi_entry& entry);
void cls_rgw_bi_put(librados::ObjectWriteOperation& op, const std::string oid, const rgw_cls_bi_entry& entry);
// Write the given array of index entries and update bucket stats accordingly.
// If existing entries may be overwritten, pass check_existing=true to decrement
// their stats first.
void cls_rgw_bi_put_entries(librados::ObjectWriteOperation& op,
                            std::vector<rgw_cls_bi_entry> entries,
                            bool check_existing);
int cls_rgw_bi_list(librados::IoCtx& io_ctx, const std::string& oid,
                   const std::string& name, const std::string& marker, uint32_t max,
                   std::list<rgw_cls_bi_entry> *entries, bool *is_truncated, bool reshardlog = false);

void cls_rgw_bucket_link_olh(librados::ObjectWriteOperation& op,
                            const cls_rgw_obj_key& key, const ceph::buffer::list& olh_tag,
                            bool delete_marker, const std::string& op_tag, const rgw_bucket_dir_entry_meta *meta,
                            uint64_t olh_epoch, ceph::real_time unmod_since, bool high_precision_time, bool log_op, const rgw_zone_set& zones_trace);
void cls_rgw_bucket_unlink_instance(librados::ObjectWriteOperation& op,
                                   const cls_rgw_obj_key& key, const std::string& op_tag,
                                   const std::string& olh_tag, uint64_t olh_epoch, bool log_op, uint16_t bilog_flags, const rgw_zone_set& zones_trace);
void cls_rgw_get_olh_log(librados::ObjectReadOperation& op, const cls_rgw_obj_key& olh, uint64_t ver_marker, const std::string& olh_tag, rgw_cls_read_olh_log_ret& log_ret, int& op_ret);
void cls_rgw_trim_olh_log(librados::ObjectWriteOperation& op, const cls_rgw_obj_key& olh, uint64_t ver, const std::string& olh_tag);
void cls_rgw_clear_olh(librados::ObjectWriteOperation& op, const cls_rgw_obj_key& olh, const std::string& olh_tag);

// these overloads which call io_ctx.operate() should not be called in the rgw.
// rgw_rados_operate() should be called after the overloads w/o calls to io_ctx.operate()
#ifndef CLS_CLIENT_HIDE_IOCTX
int cls_rgw_bucket_link_olh(librados::IoCtx& io_ctx, const std::string& oid,
                            const cls_rgw_obj_key& key, const ceph::buffer::list& olh_tag,
                            bool delete_marker, const std::string& op_tag, const rgw_bucket_dir_entry_meta *meta,
                            uint64_t olh_epoch, ceph::real_time unmod_since, bool high_precision_time, bool log_op, const rgw_zone_set& zones_trace);
int cls_rgw_bucket_unlink_instance(librados::IoCtx& io_ctx, const std::string& oid,
                                   const cls_rgw_obj_key& key, const std::string& op_tag,
                                   const std::string& olh_tag, uint64_t olh_epoch, bool log_op,
                                   uint16_t bilog_flags, const rgw_zone_set& zones_trace);
int cls_rgw_get_olh_log(librados::IoCtx& io_ctx, std::string& oid, const cls_rgw_obj_key& olh, uint64_t ver_marker,
                        const std::string& olh_tag, rgw_cls_read_olh_log_ret& log_ret);
int cls_rgw_clear_olh(librados::IoCtx& io_ctx, std::string& oid, const cls_rgw_obj_key& olh, const std::string& olh_tag);
int cls_rgw_usage_log_trim(librados::IoCtx& io_ctx, const std::string& oid, const std::string& user, const std::string& bucket,
                           uint64_t start_epoch, uint64_t end_epoch);
#endif

void cls_rgw_bucket_list_op(librados::ObjectReadOperation& op,
                            const cls_rgw_obj_key& start_obj,
                            const std::string& filter_prefix,
			    const std::string& delimiter,
                            uint32_t num_entries,
                            bool list_versions,
                            rgw_cls_list_ret* result);

void cls_rgw_bilog_list(librados::ObjectReadOperation& op,
                        const std::string& marker, uint32_t max,
                        cls_rgw_bi_log_list_ret *pdata, int *ret = nullptr);

void cls_rgw_bilog_trim(librados::ObjectWriteOperation& op,
                        const std::string& start_marker,
                        const std::string& end_marker);

void cls_rgw_bucket_check_index(librados::ObjectReadOperation& op,
                                bufferlist& out);
// decode the response; may throw buffer::error
void cls_rgw_bucket_check_index_decode(const bufferlist& out,
                                       rgw_cls_check_index_ret& result);

void cls_rgw_bucket_rebuild_index(librados::ObjectWriteOperation& op);

void cls_rgw_bilog_start(librados::ObjectWriteOperation& op);
void cls_rgw_bilog_stop(librados::ObjectWriteOperation& op);

int cls_rgw_get_dir_header_async(librados::IoCtx& io_ctx, const std::string& oid,
                                 boost::intrusive_ptr<RGWGetDirHeader_CB> cb);

void cls_rgw_encode_suggestion(char op, rgw_bucket_dir_entry& dirent, ceph::buffer::list& updates);

void cls_rgw_suggest_changes(librados::ObjectWriteOperation& o, ceph::buffer::list& updates);

/* usage logging */
// these overloads which call io_ctx.operate() should not be called in the rgw.
// rgw_rados_operate() should be called after the overloads w/o calls to io_ctx.operate()
#ifndef CLS_CLIENT_HIDE_IOCTX
int cls_rgw_usage_log_read(librados::IoCtx& io_ctx, const std::string& oid, const std::string& user, const std::string& bucket,
                           uint64_t start_epoch, uint64_t end_epoch, uint32_t max_entries, std::string& read_iter,
			   std::map<rgw_user_bucket, rgw_usage_log_entry>& usage, bool *is_truncated);
#endif

void cls_rgw_usage_log_trim(librados::ObjectWriteOperation& op, const std::string& user, const std::string& bucket, uint64_t start_epoch, uint64_t end_epoch);

void cls_rgw_usage_log_clear(librados::ObjectWriteOperation& op);
void cls_rgw_usage_log_add(librados::ObjectWriteOperation& op, rgw_usage_log_info& info);

/* garbage collection */
void cls_rgw_gc_set_entry(librados::ObjectWriteOperation& op, uint32_t expiration_secs, cls_rgw_gc_obj_info& info);
void cls_rgw_gc_defer_entry(librados::ObjectWriteOperation& op, uint32_t expiration_secs, const std::string& tag);
void cls_rgw_gc_remove(librados::ObjectWriteOperation& op, const std::vector<std::string>& tags);
void cls_rgw_gc_list(librados::ObjectReadOperation& op, const std::string& marker,
                     uint32_t max, bool expired_only, bufferlist& bl);
int cls_rgw_gc_list_decode(const bufferlist& bl,
                           std::list<cls_rgw_gc_obj_info>& entries,
                           bool *truncated, std::string& next_marker);

/* lifecycle */
void cls_rgw_lc_get_head(librados::ObjectReadOperation& op, bufferlist& bl);
int cls_rgw_lc_get_head_decode(const bufferlist& bl, cls_rgw_lc_obj_head& head);
void cls_rgw_lc_put_head(librados::ObjectWriteOperation& op, const cls_rgw_lc_obj_head& head);
void cls_rgw_lc_get_next_entry(librados::ObjectReadOperation& op, const std::string& marker, bufferlist& bl);
int cls_rgw_lc_get_next_entry_decode(const bufferlist& bl, cls_rgw_lc_entry& entry);
void cls_rgw_lc_rm_entry(librados::ObjectWriteOperation& op, const cls_rgw_lc_entry& entry);
void cls_rgw_lc_set_entry(librados::ObjectWriteOperation& op, const cls_rgw_lc_entry& entry);
void cls_rgw_lc_get_entry(librados::ObjectReadOperation& op, const std::string& marker, bufferlist& bl);
int cls_rgw_lc_get_entry_decode(const bufferlist& bl, cls_rgw_lc_entry& entry);
void cls_rgw_lc_list(librados::ObjectReadOperation& op,
                     const std::string& marker, uint32_t max_entries,
                     bufferlist& bl);
int cls_rgw_lc_list_decode(const bufferlist& bl, std::vector<cls_rgw_lc_entry>& entries);

/* multipart */
void cls_rgw_mp_upload_part_info_update(librados::ObjectWriteOperation& op, const std::string& part_key, const RGWUploadPartInfo& info);

/* resharding */
void cls_rgw_reshard_add(librados::ObjectWriteOperation& op,
			 const cls_rgw_reshard_entry& entry,
			 const bool create_only);
void cls_rgw_reshard_remove(librados::ObjectWriteOperation& op, const cls_rgw_reshard_entry& entry);
// these overloads which call io_ctx.operate() should not be called in the rgw.
// rgw_rados_operate() should be called after the overloads w/o calls to io_ctx.operate()
#ifndef CLS_CLIENT_HIDE_IOCTX
int cls_rgw_reshard_list(librados::IoCtx& io_ctx, const std::string& oid, std::string& marker, uint32_t max,
                         std::list<cls_rgw_reshard_entry>& entries, bool* is_truncated);
int cls_rgw_reshard_get(librados::IoCtx& io_ctx, const std::string& oid, cls_rgw_reshard_entry& entry);
#endif

// If writes to the bucket index should be blocked during resharding, fail with
// the given error code. RGWRados::guard_reshard() calls this in a loop to retry
// the write until the reshard completes.
//
// As of the T release, all index write ops in cls_rgw perform this check
// themselves. RGW can stop issuing this call in the T+2 (V) release once it
// knows that OSDs are running T at least. The call can be safely removed from
// cls_rgw in the T+4 (X) release.
void cls_rgw_guard_bucket_resharding(librados::ObjectOperation& op, int ret_err);

void cls_rgw_set_bucket_resharding(librados::ObjectWriteOperation& op,
                                   cls_rgw_reshard_status status);
void cls_rgw_clear_bucket_resharding(librados::ObjectWriteOperation& op);
void cls_rgw_get_bucket_resharding(librados::ObjectReadOperation& op,
                                   bufferlist& out);
// decode the entry; may throw buffer::error
void cls_rgw_get_bucket_resharding_decode(const bufferlist& out,
                                          cls_rgw_bucket_instance_entry& entry);

// Try to remove all reshard log entries from the bucket index. Return success
// if any entries were removed, and -ENODATA once they're all gone.
void cls_rgw_bucket_reshard_log_trim(librados::ObjectWriteOperation& op);
