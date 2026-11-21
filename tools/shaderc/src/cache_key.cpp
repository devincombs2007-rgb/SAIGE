#include "saige/shaderc/cache_key.h"

#include <cstdio>

namespace saige::shaderc {

namespace {

constexpr uint64_t kFnvPrime = 1099511628211ull;

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
    : m_a(14695981039346656037ull)
    , m_b(0x6c62272e07bb0142ull)
{
    mixU64(kCacheKeyVersion);
}

void CacheKeyBuilder::mixBytes(std::string_view bytes)
{
    for (const char c : bytes) {
        const auto byte = static_cast<uint64_t>(static_cast<unsigned char>(c));
        m_a = (m_a ^ byte) * kFnvPrime;
        m_b = (m_b ^ (byte + 0x9eu)) * 0x100000001b3ull;
        m_b ^= m_b >> 29;
    }
}

void CacheKeyBuilder::mixU64(uint64_t v)
{
    char bytes[8];
    for (int i = 0; i < 8; ++i) {
        bytes[i] = static_cast<char>((v >> (i * 8)) & 0xFFu);
    }
    mixBytes(std::string_view(bytes, sizeof(bytes)));
}

CacheKeyBuilder& CacheKeyBuilder::add(std::string_view field, std::string_view value)
{
    mixU64(field.size());
    mixBytes(field);
    mixU64(value.size());
    mixBytes(value);
    return *this;
}

CacheKeyBuilder& CacheKeyBuilder::add(std::string_view field, uint64_t value)
{
    mixU64(field.size());
    mixBytes(field);
    mixU64(8);
    mixU64(value);
    return *this;
}

std::pair<uint64_t, uint64_t> CacheKeyBuilder::value() const
{
    return {avalanche(m_a ^ (m_b >> 7)), avalanche(m_b ^ (m_a << 9))};
}

std::string CacheKeyBuilder::hex() const
{
    const auto [hi, lo] = value();
    char buf[33];
    std::snprintf(buf, sizeof(buf), "%016llx%016llx", static_cast<unsigned long long>(hi),
                  static_cast<unsigned long long>(lo));
    return std::string(buf, 32);
}

std::string computeCacheKey(const CompileRequest& r)
{
    CacheKeyBuilder k;
    k.add("source", r.source);
    k.add("include.count", r.includeContents.size());
    for (const std::string& inc : r.includeContents) {
        k.add("include", inc);
    }
    k.add("entry", r.entryPoint);
    k.add("stage", stageName(r.stage));
    k.add("define.count", r.defines.size());
    for (const auto& [name, value] : r.defines) {
        k.add("define.name", name);
        k.add("define.value", value);
    }
    k.add("target", r.target);
    k.add("compiler", r.compilerVersion);
    k.add("tool", r.toolVersion);
    k.add("args.count", r.extraArgs.size());
    for (const std::string& a : r.extraArgs) {
        k.add("arg", a);
    }
    return k.hex();
}

} // namespace saige::shaderc
