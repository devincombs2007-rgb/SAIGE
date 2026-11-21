#include "saige/shaderc/cache_key.hpp"

#include <cstdio>

namespace saige::shaderc {

namespace {

constexpr uint64_t fnv_prime = 1099511628211ull;

constexpr uint64_t avalanche(uint64_t x) noexcept
{
    // splitmix64 finalizer
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ull;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebull;
    x ^= x >> 31;
    return x;
}

} // namespace

CacheKeyBuilder::CacheKeyBuilder()
    : a_(14695981039346656037ull)
    , b_(0x6c62272e07bb0142ull)
{
    mix_u64(cache_key_version);
}

void CacheKeyBuilder::mix_bytes(std::string_view bytes)
{
    for (const char c : bytes) {
        const auto byte = static_cast<uint64_t>(static_cast<unsigned char>(c));
        a_ = (a_ ^ byte) * fnv_prime;
        b_ = (b_ ^ (byte + 0x9eu)) * 0x100000001b3ull;
        b_ ^= b_ >> 29;
    }
}

void CacheKeyBuilder::mix_u64(uint64_t v)
{
    char bytes[8];
    for (int i = 0; i < 8; ++i) {
        bytes[i] = static_cast<char>((v >> (i * 8)) & 0xFFu);
    }
    mix_bytes(std::string_view(bytes, sizeof(bytes)));
}

CacheKeyBuilder& CacheKeyBuilder::add(std::string_view field, std::string_view value)
{
    mix_u64(field.size());
    mix_bytes(field);
    mix_u64(value.size());
    mix_bytes(value);
    return *this;
}

CacheKeyBuilder& CacheKeyBuilder::add(std::string_view field, uint64_t value)
{
    mix_u64(field.size());
    mix_bytes(field);
    mix_u64(8);
    mix_u64(value);
    return *this;
}

std::pair<uint64_t, uint64_t> CacheKeyBuilder::value() const
{
    return {avalanche(a_ ^ (b_ >> 7)), avalanche(b_ ^ (a_ << 9))};
}

std::string CacheKeyBuilder::hex() const
{
    const auto [hi, lo] = value();
    char buf[33];
    std::snprintf(buf, sizeof(buf), "%016llx%016llx", static_cast<unsigned long long>(hi),
                  static_cast<unsigned long long>(lo));
    return std::string(buf, 32);
}

std::string compute_cache_key(const CompileRequest& r)
{
    CacheKeyBuilder k;
    k.add("source", r.source);
    k.add("include.count", r.include_contents.size());
    for (const std::string& inc : r.include_contents) {
        k.add("include", inc);
    }
    k.add("entry", r.entry_point);
    k.add("stage", stage_name(r.stage));
    k.add("define.count", r.defines.size());
    for (const auto& [name, value] : r.defines) {
        k.add("define.name", name);
        k.add("define.value", value);
    }
    k.add("target", r.target);
    k.add("compiler", r.compiler_version);
    k.add("tool", r.tool_version);
    k.add("args.count", r.extra_args.size());
    for (const std::string& a : r.extra_args) {
        k.add("arg", a);
    }
    return k.hex();
}

} // namespace saige::shaderc
