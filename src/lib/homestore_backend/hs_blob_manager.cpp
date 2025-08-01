#include "hs_backend_config.hpp"
#include "hs_homeobject.hpp"
#include "replication_message.hpp"
#include "replication_state_machine.hpp"
#include "lib/homeobject_impl.hpp"
#include "lib/blob_route.hpp"
#include <homestore/homestore.hpp>
#include <homestore/blkdata_service.hpp>

SISL_LOGGING_DECL(blobmgr)

#define BLOG(level, trace_id, shard_id, blob_id, msg, ...)                                                             \
    LOG##level##MOD(blobmgr, "[traceID={},shardID=0x{:x},pg={},shard=0x{:x},blob={}] " msg, trace_id, shard_id,        \
                    (shard_id >> homeobject::shard_width), (shard_id & homeobject::shard_mask), blob_id,               \
                    ##__VA_ARGS__)

#define BLOGT(trace_id, shard_id, blob_id, msg, ...) BLOG(TRACE, trace_id, shard_id, blob_id, msg, ##__VA_ARGS__)
#define BLOGD(trace_id, shard_id, blob_id, msg, ...) BLOG(DEBUG, trace_id, shard_id, blob_id, msg, ##__VA_ARGS__)
#define BLOGI(trace_id, shard_id, blob_id, msg, ...) BLOG(INFO, trace_id, shard_id, blob_id, msg, ##__VA_ARGS__)
#define BLOGW(trace_id, shard_id, blob_id, msg, ...) BLOG(WARN, trace_id, shard_id, blob_id, msg, ##__VA_ARGS__)
#define BLOGE(trace_id, shard_id, blob_id, msg, ...) BLOG(ERROR, trace_id, shard_id, blob_id, msg, ##__VA_ARGS__)
#define BLOGC(trace_id, shard_id, blob_id, msg, ...) BLOG(CRITICAL, trace_id, shard_id, blob_id, msg, ##__VA_ARGS__)

namespace homeobject {

BlobError toBlobError(ReplServiceError const& e) {
    switch (e) {
    case ReplServiceError::BAD_REQUEST:
        [[fallthrough]];
    case ReplServiceError::CANCELLED:
        [[fallthrough]];
    case ReplServiceError::CONFIG_CHANGING:
        [[fallthrough]];
    case ReplServiceError::SERVER_ALREADY_EXISTS:
        [[fallthrough]];
    case ReplServiceError::SERVER_IS_JOINING:
        [[fallthrough]];
    case ReplServiceError::SERVER_IS_LEAVING:
        [[fallthrough]];
    case ReplServiceError::RESULT_NOT_EXIST_YET:
        [[fallthrough]];
    case ReplServiceError::TERM_MISMATCH:
        [[fallthrough]];
    case ReplServiceError::NO_SPACE_LEFT:
        return BlobError(BlobErrorCode::NO_SPACE_LEFT);
    case ReplServiceError::DATA_DUPLICATED:
        return BlobError(BlobErrorCode::REPLICATION_ERROR);
    case ReplServiceError::NOT_LEADER:
        return BlobError(BlobErrorCode::NOT_LEADER);
    case ReplServiceError::TIMEOUT:
        return BlobError(BlobErrorCode::TIMEOUT);
    case ReplServiceError::NOT_IMPLEMENTED:
        return BlobError(BlobErrorCode::UNSUPPORTED_OP);
    case ReplServiceError::OK:
        DEBUG_ASSERT(false, "Should not process OK!");
        [[fallthrough]];
    case ReplServiceError::FAILED:
        return BlobError(BlobErrorCode::UNKNOWN);
    default:
        return BlobError(BlobErrorCode::UNKNOWN);
    }
}

struct put_blob_req_ctx : public repl_result_ctx< BlobManager::Result< HSHomeObject::BlobInfo > > {
    uint32_t blob_header_idx_{0};

    // Unaligned buffer is good enough for header and key, since they will be explicity copied
    static intrusive< put_blob_req_ctx > make(uint32_t data_hdr_size) {
        return intrusive< put_blob_req_ctx >{new put_blob_req_ctx(data_hdr_size)};
    }

    put_blob_req_ctx(uint32_t data_hdr_size) : repl_result_ctx(0u /* header_extn_size */, sizeof(blob_id_t)) {
        uint32_t aligned_size = uint32_cast(sisl::round_up(data_hdr_size, io_align));
        sisl::io_blob_safe buf{aligned_size, io_align};
        new (buf.bytes()) HSHomeObject::BlobHeader();
        add_data_sg(std::move(buf));
        blob_header_idx_ = data_bufs_.size() - 1;
    }

    void copy_user_key(std::string const& user_key) {
        std::memcpy((blob_header_buf().bytes() + sizeof(HSHomeObject::BlobHeader)), user_key.data(), user_key.size());
    }

    HSHomeObject::BlobHeader* blob_header() { return r_cast< HSHomeObject::BlobHeader* >(blob_header_buf().bytes()); }
    sisl::io_blob_safe& blob_header_buf() { return data_bufs_[blob_header_idx_]; }
};

BlobManager::AsyncResult< blob_id_t > HSHomeObject::_put_blob(ShardInfo const& shard, Blob&& blob, trace_id_t tid) {

    if (is_shutting_down()) {
        LOGI("service is being shut down");
        return folly::makeUnexpected(BlobErrorCode::SHUTTING_DOWN);
    }
    incr_pending_request_num();
    auto& pg_id = shard.placement_group;
    shared< homestore::ReplDev > repl_dev;
    blob_id_t new_blob_id;
    {
        auto hs_pg = get_hs_pg(pg_id);
        RELEASE_ASSERT(hs_pg, "PG not found, pg={}", pg_id);
        if (hs_pg->pg_state_.is_state_set(PGStateMask::DISK_DOWN)) {
            LOGW("failed to put blob for pg={}, pg is disk down and not leader", pg_id);
            decr_pending_request_num();
            return folly::makeUnexpected(BlobErrorCode::NOT_LEADER);
        }
        repl_dev = hs_pg->repl_dev_;
        const_cast< HS_PG* >(hs_pg)->durable_entities_update(
            [&new_blob_id](auto& de) { new_blob_id = de.blob_sequence_num.fetch_add(1, std::memory_order_relaxed); },
            false /* dirty */);

        DEBUG_ASSERT_LT(new_blob_id, std::numeric_limits< decltype(new_blob_id) >::max(),
                        "exhausted all available blob ids");
    }
    RELEASE_ASSERT(repl_dev != nullptr, "Repl dev instance null");
    BLOGD(tid, shard.id, new_blob_id, "Blob Put request: pg={}, group={}, shard=0x{:x}, length={}", pg_id,
          repl_dev->group_id(), shard.id, blob.body.size());

    if (!repl_dev->is_leader()) {
        BLOGW(tid, shard.id, new_blob_id, "failed to put blob for pg={}, not leader", pg_id);
        decr_pending_request_num();
        return folly::makeUnexpected(BlobError(BlobErrorCode::NOT_LEADER, repl_dev->get_leader_id()));
    }

    if (!repl_dev->is_ready_for_traffic()) {
        BLOGW(tid, shard.id, new_blob_id, "failed to put blob for pg={}, not ready for traffic", pg_id);
        decr_pending_request_num();
        return folly::makeUnexpected(BlobError(BlobErrorCode::RETRY_REQUEST));
    }

    // Create a put_blob request which allocates for header, key and blob_header, user_key. Data sgs are added later
    auto req = put_blob_req_ctx::make(sizeof(BlobHeader) + blob.user_key.size());
    req->header()->msg_type = ReplicationMessageType::PUT_BLOB_MSG;
    req->header()->payload_size = 0;
    req->header()->payload_crc = 0;
    req->header()->shard_id = shard.id;
    req->header()->pg_id = pg_id;
    req->header()->blob_id = new_blob_id;
    req->header()->seal();

    // Serialize blob_id as Replication key
    *(reinterpret_cast< blob_id_t* >(req->key_buf().bytes())) = new_blob_id;

    // Now we are in data section. Data section has 4 areas
    // 1. Blob header (information about the type of blob, size, hash etc)
    // 2. User Key (if present)
    // 3. Actual data blobs
    // 4. Any padding of zeros (to round off to nearest block size)

    // Blob Header section.
    auto const blob_size = blob.body.size();
    req->blob_header()->type = DataHeader::data_type_t::BLOB_INFO;
    req->blob_header()->shard_id = shard.id;
    req->blob_header()->blob_id = new_blob_id;
    req->blob_header()->hash_algorithm = BlobHeader::HashAlgorithm::CRC32;
    req->blob_header()->blob_size = blob_size;
    req->blob_header()->user_key_size = blob.user_key.size();
    req->blob_header()->object_offset = blob.object_off;

    // Append the user key information if present.
    if (!blob.user_key.empty()) { req->copy_user_key(blob.user_key); }

    // Set offset of actual data after the blob header and user key (rounded off)
    req->blob_header()->data_offset = req->blob_header_buf().size();

    // In case blob body is not aligned, create a new aligned buffer and copy the blob body.
    if (((r_cast< uintptr_t >(blob.body.cbytes()) % io_align) != 0) || ((blob_size % io_align) != 0)) {
        // If address or size is not aligned, create a separate aligned buffer and do expensive memcpy.
        sisl::io_blob_safe new_body = sisl::io_blob_safe(sisl::round_up(blob_size, io_align), io_align);
        std::memcpy(new_body.bytes(), blob.body.cbytes(), blob_size);
        blob.body = std::move(new_body);
    }

    // Compute the checksum of blob and metadata.
    compute_blob_payload_hash(req->blob_header()->hash_algorithm, blob.body.cbytes(), blob_size,
                              (uint8_t*)blob.user_key.data(), blob.user_key.size(), req->blob_header()->hash,
                              BlobHeader::blob_max_hash_len);
    req->blob_header()->seal();

    // Add blob body to the request
    req->add_data_sg(std::move(blob.body));

    // Check if any padding of zeroes needs to be added to be aligned to device block size.
    auto pad_len = sisl::round_up(req->data_sgs().size, repl_dev->get_blk_size()) - req->data_sgs().size;
    if (pad_len != 0) {
        sisl::io_blob_safe& zbuf = get_pad_buf(pad_len);
        req->add_data_sg(zbuf.bytes(), pad_len);
    }
    BLOGT(tid, req->blob_header()->shard_id, req->blob_header()->blob_id, "Put blob: header={} sgs={}",
          req->blob_header()->to_string(), req->data_sgs_string());

    repl_dev->async_alloc_write(req->cheader_buf(), req->ckey_buf(), req->data_sgs(), req, false /* part_of_batch */,
                                tid);
    return req->result().deferValue(
        [this, req, repl_dev, tid](const auto& result) -> BlobManager::AsyncResult< blob_id_t > {
            if (result.hasError()) {
                auto err = result.error();
                if (err.getCode() == BlobErrorCode::NOT_LEADER) { err.current_leader = repl_dev->get_leader_id(); }
                decr_pending_request_num();
                return folly::makeUnexpected(err);
            }
            auto blob_info = result.value();
            BLOGD(tid, blob_info.shard_id, blob_info.blob_id, "Blob Put request: Put blob success blkid={}",
                  blob_info.pbas.to_string());
            decr_pending_request_num();
            return blob_info.blob_id;
        });
}

bool HSHomeObject::local_add_blob_info(pg_id_t const pg_id, BlobInfo const& blob_info, trace_id_t tid) {
    auto hs_pg = get_hs_pg(pg_id);
    RELEASE_ASSERT(hs_pg != nullptr, "PG not found");
    shared< BlobIndexTable > index_table = hs_pg->index_table_;
    RELEASE_ASSERT(index_table != nullptr, "Index table not initialized");

    // Write to index table with key {shard id, blob id} and value {pba}.
    auto const [exist_already, status] = add_to_index_table(index_table, blob_info);
    BLOGT(tid, blob_info.shard_id, blob_info.blob_id, "blob put commit, exist_already={}, status={}, pbas={}",
          exist_already, status, blob_info.pbas.to_string());
    if (status != homestore::btree_status_t::success) {
        BLOGE(tid, blob_info.shard_id, blob_info.blob_id, "Failed to insert into index table, err {}",
              enum_name(status));
        return false;
    }
    if (!exist_already) {
        // The PG superblock (durable entities) will be persisted as part of HS_CLIENT Checkpoint, which is always
        // done ahead of the Index Checkpoint. Hence, if the index already has this entity, whatever durable
        // counters updated as part of the update would have been persisted already in PG superblock. So if we were
        // to increment now, it will be a duplicate increment, hence ignoring for cases where index already exist
        // for this blob put.

        // Update the durable counters. We need to update the blob_sequence_num here only for replay case, as the
        // number is already updated in the put_blob call.
        const_cast< HS_PG* >(hs_pg)->durable_entities_update([&blob_info](auto& de) {
            auto existing_blob_id = de.blob_sequence_num.load();
            auto next_blob_id = blob_info.blob_id + 1;
            while (next_blob_id > existing_blob_id &&
                   // we need update the blob_sequence_num to existing_blob_id+1 so that if leader changes, we can
                   // still get the up-to-date blob_sequence_num
                   !de.blob_sequence_num.compare_exchange_weak(existing_blob_id, next_blob_id)) {}
            de.active_blob_count.fetch_add(1, std::memory_order_relaxed);
            de.total_occupied_blk_count.fetch_add(blob_info.pbas.blk_count(), std::memory_order_relaxed);
        });
    } else {
        BLOGT(tid, blob_info.shard_id, blob_info.blob_id, "blob already exists in index table, skip it.");
    }
    return true;
}

void HSHomeObject::on_blob_put_commit(int64_t lsn, sisl::blob const& header, sisl::blob const& key,
                                      homestore::MultiBlkId const& pbas,
                                      cintrusive< homestore::repl_req_ctx >& hs_ctx) {
    LOGTRACEMOD(blobmgr, "blob put commit lsn={}, pbas={}", lsn, pbas.to_string());
    repl_result_ctx< BlobManager::Result< BlobInfo > >* ctx{nullptr};
    if (hs_ctx && hs_ctx->is_proposer()) {
        ctx = boost::static_pointer_cast< repl_result_ctx< BlobManager::Result< BlobInfo > > >(hs_ctx).get();
    }
    trace_id_t tid = hs_ctx ? hs_ctx->traceID() : 0;
    auto msg_header = r_cast< ReplicationMessageHeader const* >(header.cbytes());
    if (msg_header->corrupted()) {
        LOGE("replication message header is corrupted with crc error, lsn={}, traceID={}", lsn, tid);
        if (ctx) { ctx->promise_.setValue(folly::makeUnexpected(BlobError(BlobErrorCode::CHECKSUM_MISMATCH))); }
        return;
    }

    auto const blob_id = *(reinterpret_cast< blob_id_t* >(const_cast< uint8_t* >(key.cbytes())));
    auto const pg_id = msg_header->pg_id;

    BlobInfo blob_info;
    blob_info.shard_id = msg_header->shard_id;
    blob_info.blob_id = blob_id;
    blob_info.pbas = pbas;

    bool success = local_add_blob_info(pg_id, blob_info, tid);

    if (ctx) {
        ctx->promise_.setValue(success ? BlobManager::Result< BlobInfo >(blob_info)
                                       : folly::makeUnexpected(BlobError(BlobErrorCode::INDEX_ERROR)));
    }
}

BlobManager::AsyncResult< Blob > HSHomeObject::_get_blob(ShardInfo const& shard, blob_id_t blob_id, uint64_t req_offset,
                                                         uint64_t req_len, trace_id_t tid) const {
    if (is_shutting_down()) {
        LOGI("service is being shutdown");
        return folly::makeUnexpected(BlobErrorCode::SHUTTING_DOWN);
    }
    incr_pending_request_num();
    auto& pg_id = shard.placement_group;
    auto hs_pg = get_hs_pg(pg_id);
    RELEASE_ASSERT(hs_pg, "PG not found");
    auto repl_dev = hs_pg->repl_dev_;
    auto index_table = hs_pg->index_table_;

    RELEASE_ASSERT(repl_dev != nullptr, "Repl dev instance null");
    RELEASE_ASSERT(index_table != nullptr, "Index table instance null");

    if (!repl_dev->is_ready_for_traffic()) {
        LOGW("failed to get blob for pg={}, shardID=0x{:x},pg={},shard=0x{:x}, not ready for traffic", pg_id, shard.id,
             (shard.id >> homeobject::shard_width), (shard.id & homeobject::shard_mask));
        decr_pending_request_num();
        return folly::makeUnexpected(BlobError(BlobErrorCode::RETRY_REQUEST));
    }

    BLOGD(tid, shard.id, blob_id, "Blob Get request: pg={}, group={}, shard=0x{:x}, blob={}, offset={}, len={}", pg_id,
          repl_dev->group_id(), shard.id, blob_id, req_offset, req_len);
    auto r = get_blob_from_index_table(index_table, shard.id, blob_id);
    if (!r) {
        BLOGE(tid, shard.id, blob_id, "Blob not found in index during get blob");
        decr_pending_request_num();
        return folly::makeUnexpected(r.error());
    }

    return _get_blob_data(repl_dev, shard.id, blob_id, req_offset, req_len, r.value() /* blkid*/, tid);
}

BlobManager::AsyncResult< Blob > HSHomeObject::_get_blob_data(const shared< homestore::ReplDev >& repl_dev,
                                                              shard_id_t shard_id, blob_id_t blob_id,
                                                              uint64_t req_offset, uint64_t req_len,
                                                              const homestore::MultiBlkId& blkid,
                                                              trace_id_t tid) const {
    auto const total_size = blkid.blk_count() * repl_dev->get_blk_size();
    sisl::io_blob_safe read_buf{total_size, io_align};

    sisl::sg_list sgs;
    sgs.size = total_size;
    sgs.iovs.emplace_back(iovec{.iov_base = read_buf.bytes(), .iov_len = read_buf.size()});

    BLOGD(tid, shard_id, blob_id, "Reading from blkid={} to buf={}", blkid.to_string(), (void*)read_buf.bytes());
    return repl_dev->async_read(blkid, sgs, total_size)
        .thenValue([this, tid, blob_id, shard_id, req_len, req_offset, blkid, repl_dev,
                    read_buf = std::move(read_buf)](auto&& result) mutable -> BlobManager::AsyncResult< Blob > {
            if (result) {
                BLOGE(tid, shard_id, blob_id, "Failed to get blob: err={}", blob_id, shard_id, result.value());
                decr_pending_request_num();
                return folly::makeUnexpected(BlobError(BlobErrorCode::READ_FAILED));
            }

            BlobHeader const* header = r_cast< BlobHeader const* >(read_buf.cbytes());
            if (!header->valid()) {
                BLOGE(tid, shard_id, blob_id, "Invalid header found: [header={}]", header->to_string());
                decr_pending_request_num();
                return folly::makeUnexpected(BlobError(BlobErrorCode::READ_FAILED));
            }

            if (header->shard_id != shard_id) {
                BLOGE(tid, shard_id, blob_id, "Invalid shard_id in header: [header={}]", header->to_string());
                decr_pending_request_num();
                return folly::makeUnexpected(BlobError(BlobErrorCode::READ_FAILED));
            }

            // Metadata start offset is just after blob header
            std::string user_key = header->user_key_size
                ? std::string((const char*)(read_buf.bytes() + sizeof(BlobHeader)), (size_t)header->user_key_size)
                : std::string{};

            uint8_t const* blob_bytes = read_buf.bytes() + header->data_offset;
            uint8_t computed_hash[BlobHeader::blob_max_hash_len]{};
            compute_blob_payload_hash(header->hash_algorithm, blob_bytes, header->blob_size,
                                      uintptr_cast(user_key.data()), header->user_key_size, computed_hash,
                                      BlobHeader::blob_max_hash_len);
            if (std::memcmp(computed_hash, header->hash, BlobHeader::blob_max_hash_len) != 0) {
                BLOGE(tid, shard_id, blob_id, "Hash mismatch header, [header={}] [computed={:np}]", header->to_string(),
                      spdlog::to_hex(computed_hash, computed_hash + BlobHeader::blob_max_hash_len));
                decr_pending_request_num();
                return folly::makeUnexpected(BlobError(BlobErrorCode::CHECKSUM_MISMATCH));
            }

            if (req_offset + req_len > header->blob_size) {
                BLOGE(tid, shard_id, blob_id, "Invalid offset length requested in get blob offset={} len={} size={}",
                      req_offset, req_len, header->blob_size);
                decr_pending_request_num();
                return folly::makeUnexpected(BlobError(BlobErrorCode::INVALID_ARG));
            }

            // Copy the blob bytes from the offset. If request len is 0, take the
            // whole blob size else copy only the request length.
            auto res_len = req_len == 0 ? header->blob_size - req_offset : req_len;
            auto body = sisl::io_blob_safe(res_len);
            std::memcpy(body.bytes(), blob_bytes + req_offset, res_len);

            BLOGD(tid, shard_id, blob_id, "Blob get success: blkid={}", blkid.to_string());
            decr_pending_request_num();
            return Blob(std::move(body), std::move(user_key), header->object_offset, repl_dev->get_leader_id());
        });
}

homestore::ReplResult< homestore::blk_alloc_hints >
HSHomeObject::blob_put_get_blk_alloc_hints(sisl::blob const& header, cintrusive< homestore::repl_req_ctx >& hs_ctx) {
    repl_result_ctx< BlobManager::Result< BlobInfo > >* ctx{nullptr};
    if (hs_ctx && hs_ctx->is_proposer()) {
        ctx = boost::static_pointer_cast< repl_result_ctx< BlobManager::Result< BlobInfo > > >(hs_ctx).get();
    }
    auto tid = hs_ctx ? hs_ctx->traceID() : 0;

    auto msg_header = r_cast< ReplicationMessageHeader* >(const_cast< uint8_t* >(header.cbytes()));
    if (msg_header->corrupted()) {
        LOGE("traceID={}, shardID=0x{:x}, pg={}, shard=0x{:x}, replication message header is corrupted with crc error",
             tid, msg_header->shard_id, (msg_header->shard_id >> homeobject::shard_width),
             (msg_header->shard_id & homeobject::shard_mask));
        if (ctx) { ctx->promise_.setValue(folly::makeUnexpected(BlobError(BlobErrorCode::CHECKSUM_MISMATCH))); }
        return folly::makeUnexpected(homestore::ReplServiceError::FAILED);
    }

    auto hs_pg = get_hs_pg(msg_header->pg_id);
    if (hs_pg == nullptr) {
        LOGW("traceID={}, shardID=0x{:x}, pg={}, shard=0x{:x}, Received a blob_put on an unknown pg={}, underlying "
             "engine will "
             "retry this later",
             tid, msg_header->shard_id, (msg_header->shard_id >> homeobject::shard_width),
             (msg_header->shard_id & homeobject::shard_mask), msg_header->pg_id);
        if (ctx) { ctx->promise_.setValue(folly::makeUnexpected(BlobError(BlobErrorCode::UNKNOWN_PG))); }
        return folly::makeUnexpected(homestore::ReplServiceError::RESULT_NOT_EXIST_YET);
    }

    std::scoped_lock lock_guard(_shard_lock);
    auto shard_iter = _shard_map.find(msg_header->shard_id);
    if (shard_iter == _shard_map.end()) {
        LOGW("traceID={}, shardID=0x{:x}, pg={}, shard=0x{:x}, Received a blob_put on an unknown shard, underlying "
             "engine will retry this later",
             tid, msg_header->shard_id, (msg_header->shard_id >> homeobject::shard_width),
             (msg_header->shard_id & homeobject::shard_mask));
        if (ctx) { ctx->promise_.setValue(folly::makeUnexpected(BlobError(BlobErrorCode::UNKNOWN_SHARD))); }
        return folly::makeUnexpected(homestore::ReplServiceError::RESULT_NOT_EXIST_YET);
    }

    auto hs_shard = d_cast< HS_Shard* >((*shard_iter->second).get());

    homestore::blk_alloc_hints hints;
    hints.chunk_id_hint = hs_shard->sb_->p_chunk_id;
    if (hs_ctx->is_proposer()) { hints.reserved_blks = get_reserved_blks(); }
    BLOGD(tid, msg_header->shard_id, msg_header->blob_id, "Picked p_chunk_id={}, reserved_blks={}",
          hs_shard->sb_->p_chunk_id, get_reserved_blks());

    if (msg_header->blob_id != 0) {
        // check if the blob already exists, if yes, return the blk id
        auto r = get_blob_from_index_table(hs_pg->index_table_, msg_header->shard_id, msg_header->blob_id);
        if (r.hasValue()) {
            BLOGT(tid, msg_header->shard_id, msg_header->blob_id,
                  "Blob has already been persisted, blk_num={}, blk_count={}", r.value().blk_num(),
                  r.value().blk_count());
            hints.committed_blk_id = r.value();
        }
    }

    return hints;
}

BlobManager::NullAsyncResult HSHomeObject::_del_blob(ShardInfo const& shard, blob_id_t blob_id, trace_id_t tid) {
    if (is_shutting_down()) {
        LOGI("service is being shut down");
        return folly::makeUnexpected(BlobErrorCode::SHUTTING_DOWN);
    }
    incr_pending_request_num();
    BLOGT(tid, shard.id, blob_id, "deleting blob");
    auto& pg_id = shard.placement_group;
    auto hs_pg = get_hs_pg(pg_id);
    RELEASE_ASSERT(hs_pg, "PG not found");

    if (hs_pg->pg_state_.is_state_set(PGStateMask::DISK_DOWN)) {
        LOGW("failed to delete blob for pg={}, pg is disk down and not leader", pg_id);
        decr_pending_request_num();
        return folly::makeUnexpected(BlobErrorCode::NOT_LEADER);
    }

    auto repl_dev = hs_pg->repl_dev_;

    RELEASE_ASSERT(repl_dev != nullptr, "Repl dev instance null");
    BLOGD(tid, shard.id, blob_id, "Blob Delete request: pg={}, group={}, Shard={}, Blob={}", shard.placement_group,
          repl_dev->group_id(), shard.id, blob_id);

    if (!repl_dev->is_leader()) {
        BLOGW(tid, shard.id, blob_id, "failed to del blob, not leader");
        decr_pending_request_num();
        return folly::makeUnexpected(BlobError(BlobErrorCode::NOT_LEADER, repl_dev->get_leader_id()));
    }

    if (!repl_dev->is_ready_for_traffic()) {
        BLOGW(tid, shard.id, blob_id, "failed to del blob, not ready for traffic");
        decr_pending_request_num();
        return folly::makeUnexpected(BlobError(BlobErrorCode::RETRY_REQUEST));
    }

    // Create an unaligned header request unaligned
    auto req = repl_result_ctx< BlobManager::Result< BlobInfo > >::make(0u /* header_extn */,
                                                                        sizeof(blob_id_t) /* key_size */);
    req->header()->msg_type = ReplicationMessageType::DEL_BLOB_MSG;
    req->header()->payload_size = 0;
    req->header()->payload_crc = 0;
    req->header()->shard_id = shard.id;
    req->header()->pg_id = pg_id;
    req->header()->seal();

    // Populate the key
    std::memcpy(req->key_buf().bytes(), &blob_id, sizeof(blob_id_t));

    repl_dev->async_alloc_write(req->cheader_buf(), req->ckey_buf(), sisl::sg_list{}, req, false /* part_of_batch */,
                                tid);
    return req->result().deferValue(
        [this, repl_dev, tid](const auto& result) -> folly::Expected< folly::Unit, BlobError > {
            if (result.hasError()) {
                auto err = result.error();
                if (err.getCode() == BlobErrorCode::NOT_LEADER) { err.current_leader = repl_dev->get_leader_id(); }
                decr_pending_request_num();
                return folly::makeUnexpected(err);
            }
            auto blob_info = result.value();
            BLOGT(tid, blob_info.shard_id, blob_info.blob_id, "Delete blob successful");
            decr_pending_request_num();
            return folly::Unit();
        });
}

void HSHomeObject::on_blob_del_commit(int64_t lsn, sisl::blob const& header, sisl::blob const& key,
                                      cintrusive< homestore::repl_req_ctx >& hs_ctx) {
    repl_result_ctx< BlobManager::Result< BlobInfo > >* ctx{nullptr};
    if (hs_ctx && hs_ctx->is_proposer()) {
        ctx = boost::static_pointer_cast< repl_result_ctx< BlobManager::Result< BlobInfo > > >(hs_ctx).get();
    }
    auto tid = hs_ctx ? hs_ctx->traceID() : 0;
    auto msg_header = r_cast< ReplicationMessageHeader* >(const_cast< uint8_t* >(header.cbytes()));
    if (msg_header->corrupted()) {
        BLOGE(tid, msg_header->shard_id, *r_cast< blob_id_t const* >(key.cbytes()),
              "replication message header is corrupted with crc error, lsn={} header={}", lsn, msg_header->to_string());
        if (ctx) { ctx->promise_.setValue(folly::makeUnexpected(BlobError(BlobErrorCode::CHECKSUM_MISMATCH))); }
        return;
    }

    const auto pg_id = msg_header->pg_id;
    auto hs_pg = get_hs_pg(pg_id);
    RELEASE_ASSERT(hs_pg, "PG not found, pg={}", pg_id);
    auto index_table = hs_pg->index_table_;
    auto repl_dev = hs_pg->repl_dev_;
    RELEASE_ASSERT(index_table != nullptr, "Index table not intialized");
    RELEASE_ASSERT(repl_dev != nullptr, "Repl dev instance null");

    const auto shard_id = msg_header->shard_id;
    const auto blob_id = *r_cast< blob_id_t const* >(key.cbytes());

    BlobRouteKey index_key{BlobRoute{shard_id, blob_id}};
    BlobRouteValue index_value{tombstone_pbas};
    BlobRouteValue existing_value;

    homestore::BtreeSinglePutRequest update_req{&index_key, &index_value, homestore::btree_put_type::UPDATE,
                                                &existing_value};

    auto status = index_table->put(update_req);

    if (sisl_unlikely(status == homestore::btree_status_t::not_found)) {
        // in baseline resync case, the blob might have already been deleted at leader and follower does not receive
        // this blob, so when committing this log, not_found will happen.
        LOGW("shard_id={}, blob_id={} not found, probably already deleted, lsn={}", shard_id, blob_id, lsn);
    } else if (sisl_unlikely(status != homestore::btree_status_t::success)) {
        // if we return directly, for the perspective of statemachine, this lsn has been committed successfully. so
        // there will be no more deletion for this blob, and as a result , blob leak happens
        RELEASE_ASSERT(false, "fail to commit delete_blob log for pg={}, shard={}, blob={}, lsn={}", pg_id, shard_id,
                       blob_id, lsn);
    } else {
        LOGD("shard_id={}, blob_id={} has been moved to tombstone, lsn={}", shard_id, blob_id, lsn);

        auto existing_pbas = existing_value.pbas();
        if (sisl_likely(existing_pbas != tombstone_pbas)) {
            repl_dev->async_free_blks(lsn, existing_pbas).thenValue([hs_pg](auto&& err) {
                if (err) {
                    // even if async_free_blks fails, as the blob is already updated to tombstone, it will be gc
                    // eventually.
                    LOGE("Failed to free blocks for tombstoned blob, error={}", err.value());
                }
                const_cast< HS_PG* >(hs_pg)->durable_entities_update([](auto& de) {
                    de.active_blob_count.fetch_sub(1, std::memory_order_relaxed);
                    de.tombstone_blob_count.fetch_add(1, std::memory_order_relaxed);
                });
            });
        } else {
            LOGW("shard_id={}, blob_id={} already tombstoned, lsn={}", shard_id, blob_id, lsn);
        }
    }

    if (ctx) { ctx->promise_.setValue(BlobManager::Result< BlobInfo >({shard_id, blob_id, tombstone_pbas})); }
}

void HSHomeObject::compute_blob_payload_hash(BlobHeader::HashAlgorithm algorithm, const uint8_t* blob_bytes,
                                             size_t blob_size, const uint8_t* user_key_bytes, size_t user_key_size,
                                             uint8_t* hash_bytes, size_t hash_len) const {
    std::memset(hash_bytes, 0, hash_len);
    switch (algorithm) {
    case HSHomeObject::BlobHeader::HashAlgorithm::NONE: {
        break;
    }
    case HSHomeObject::BlobHeader::HashAlgorithm::CRC32: {
        auto hash32 = crc32_ieee(init_crc32, blob_bytes, blob_size);
        if (user_key_size != 0) { hash32 = crc32_ieee(hash32, user_key_bytes, user_key_size); }
        RELEASE_ASSERT(sizeof(uint32_t) <= hash_len, "Hash length invalid");
        std::memcpy(hash_bytes, r_cast< uint8_t* >(&hash32), sizeof(uint32_t));
        break;
    }
    default:
        RELEASE_ASSERT(false, "Hash not implemented");
    }
}

void HSHomeObject::on_blob_message_rollback(int64_t lsn, sisl::blob const& header, sisl::blob const& key,
                                            cintrusive< homestore::repl_req_ctx >& hs_ctx) {
    repl_result_ctx< BlobManager::Result< BlobInfo > >* ctx{nullptr};
    if (hs_ctx && hs_ctx->is_proposer()) {
        ctx = boost::static_pointer_cast< repl_result_ctx< BlobManager::Result< BlobInfo > > >(hs_ctx).get();
    }
    auto tid = hs_ctx ? hs_ctx->traceID() : 0;
    const ReplicationMessageHeader* msg_header = r_cast< const ReplicationMessageHeader* >(header.cbytes());
    if (msg_header->corrupted()) {
        LOGW("replication message header is corrupted with crc error, lsn={}, traceID={}", lsn, tid);
        if (ctx) { ctx->promise_.setValue(folly::makeUnexpected(BlobError(BlobErrorCode::CHECKSUM_MISMATCH))); }
        return;
    }

    switch (msg_header->msg_type) {
    case ReplicationMessageType::PUT_BLOB_MSG:
    case ReplicationMessageType::DEL_BLOB_MSG: {
        // TODO:: add rollback logic for put_blob and del_blob if necessary
        LOGI("traceID={}, lsn={}, mes_type={} is rollbacked", tid, lsn, msg_header->msg_type);
        if (ctx) { ctx->promise_.setValue(folly::makeUnexpected(BlobError(BlobErrorCode::ROLL_BACK))); }
        break;
    }
    default: {
        LOGW("traceID={}, lsn={}, mes_type={} should not happen in blob message rollback", tid, lsn,
             msg_header->msg_type);
        break;
    }
    }
}

} // namespace homeobject
