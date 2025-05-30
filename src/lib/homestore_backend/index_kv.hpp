#pragma once

#include <homestore/btree/btree_kv.hpp>
#include <homestore/index/index_internal.hpp>
#include <homestore/index_service.hpp>
#include "lib/blob_route.hpp"

namespace homeobject {

ENUM(INDEX_TYPE, uint32_t, BLOB_INDEX = 0, GC_BLOB_INDEX);

class BlobRouteKey : public homestore::BtreeKey {
private:
    BlobRoute key_;

public:
    BlobRouteKey() = default;
    BlobRouteKey(const BlobRoute key) : key_(key) {}
    BlobRouteKey(const BlobRouteKey& other) : BlobRouteKey(other.serialize(), true) {}
    BlobRouteKey(const homestore::BtreeKey& other) : BlobRouteKey(other.serialize(), true) {}
    BlobRouteKey(const sisl::blob& b, bool copy) :
            homestore::BtreeKey(), key_{*(r_cast< const BlobRoute* >(b.cbytes()))} {}

    ~BlobRouteKey() override = default;

    int compare(const homestore::BtreeKey& o) const override {
        const BlobRouteKey& other = s_cast< const BlobRouteKey& >(o);
        if (key_ < other.key_) {
            return -1;
        } else if (key_ > other.key_) {
            return 1;
        } else {
            return 0;
        }
    }

    sisl::blob serialize() const override { return key_.to_blob(); }
    uint32_t serialized_size() const override { return sizeof(key_); }
    static bool is_fixed_size() { return true; }
    static uint32_t get_fixed_size() { return (sizeof(key_)); }
    std::string to_string() const { return fmt::format("{}", key_); }

    void deserialize(const sisl::blob& b, bool copy) override { key_ = *(r_cast< const BlobRoute* >(b.cbytes())); }

    static uint32_t get_max_size() { return get_fixed_size(); }
    friend std::ostream& operator<<(std::ostream& os, const BlobRouteKey& k) {
        os << fmt::format("{}", k.key());
        return os;
    }

    BlobRoute key() const { return key_; }
};

class BlobRouteByChunkKey : public homestore::BtreeKey {
private:
    BlobRouteByChunk key_;

public:
    BlobRouteByChunkKey() = default;
    BlobRouteByChunkKey(const BlobRouteByChunk key) : key_(key) {}
    BlobRouteByChunkKey(const BlobRouteByChunkKey& other) : BlobRouteByChunkKey(other.serialize(), true) {}
    BlobRouteByChunkKey(const homestore::BtreeKey& other) : BlobRouteByChunkKey(other.serialize(), true) {}
    BlobRouteByChunkKey(const sisl::blob& b, bool copy) :
            homestore::BtreeKey(), key_{*(r_cast< const BlobRouteByChunk* >(b.cbytes()))} {}

    ~BlobRouteByChunkKey() override = default;

    int compare(const homestore::BtreeKey& o) const override {
        const BlobRouteByChunkKey& other = s_cast< const BlobRouteByChunkKey& >(o);
        if (key_ < other.key_) {
            return -1;
        } else if (key_ > other.key_) {
            return 1;
        } else {
            return 0;
        }
    }

    sisl::blob serialize() const override { return key_.to_blob(); }
    uint32_t serialized_size() const override { return sizeof(key_); }
    static bool is_fixed_size() { return true; }
    static uint32_t get_fixed_size() { return (sizeof(key_)); }
    std::string to_string() const { return fmt::format("{}", key_); }

    void deserialize(const sisl::blob& b, bool copy) override {
        key_ = *(r_cast< const BlobRouteByChunk* >(b.cbytes()));
    }

    static uint32_t get_max_size() { return get_fixed_size(); }
    friend std::ostream& operator<<(std::ostream& os, const BlobRouteByChunkKey& k) {
        os << fmt::format("{}", k.key());
        return os;
    }

    BlobRouteByChunk key() const { return key_; }
};

class BlobRouteValue : public homestore::BtreeValue {
public:
    BlobRouteValue() = default;
    BlobRouteValue(const homestore::MultiBlkId& pbas) : pbas_(pbas) {}
    BlobRouteValue(const BlobRouteValue& other) : homestore::BtreeValue() { pbas_ = other.pbas_; };
    BlobRouteValue(const sisl::blob& b, bool copy) : homestore::BtreeValue() { deserialize(b, copy); }
    BlobRouteValue(const homestore::BtreeValue& other) : BlobRouteValue(other.serialize(), true) {}
    virtual ~BlobRouteValue() = default;

    BlobRouteValue& operator=(const BlobRouteValue& other) {
        pbas_ = other.pbas_;
        return *this;
    }

    sisl::blob serialize() const override {
        auto& pba = const_cast< homestore::MultiBlkId& >(pbas_);
        return pba.serialize();
    }

    uint32_t serialized_size() const override { return pbas_.serialized_size(); }
    static uint32_t get_fixed_size() { return homestore::MultiBlkId::expected_serialized_size(1 /* num_pieces */); }

    void deserialize(const sisl::blob& b, bool copy) override { pbas_.deserialize(b, copy); }
    std::string to_string() const override { return fmt::format("{}", pbas_.to_string()); }
    friend std::ostream& operator<<(std::ostream& os, const BlobRouteValue& v) {
        os << v.pbas().to_string();
        return os;
    }

    homestore::MultiBlkId pbas() const { return pbas_; }

private:
    homestore::MultiBlkId pbas_;
};

} // namespace homeobject

namespace fmt {
template <>
struct formatter< homeobject::BlobRouteKey > {
    template < typename ParseContext >
    constexpr auto parse(ParseContext& ctx) {
        return ctx.begin();
    }

    template < typename FormatContext >
    auto format(homeobject::BlobRouteKey const& r, FormatContext& ctx) {
        return format_to(ctx.out(), "{}", r.key());
    }
};

template <>
struct formatter< homeobject::BlobRouteByChunkKey > {
    template < typename ParseContext >
    constexpr auto parse(ParseContext& ctx) {
        return ctx.begin();
    }

    template < typename FormatContext >
    auto format(homeobject::BlobRouteByChunkKey const& r, FormatContext& ctx) {
        return format_to(ctx.out(), "{}", r.key());
    }
};
} // namespace fmt
