#include "sha.h"

#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#endif
#include <algorithm>
#include <array>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace doaxbv {
namespace {

std::string hexLower(const std::vector<std::uint8_t>& bytes)
{
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (const auto byte : bytes) {
        out << std::setw(2) << static_cast<int>(byte);
    }
    return out.str();
}

#ifdef _WIN32
std::string hashHex(const wchar_t* algorithm, const std::uint8_t* bytes, std::size_t size)
{
    BCRYPT_ALG_HANDLE algorithmHandle = nullptr;
    BCRYPT_HASH_HANDLE hashHandle = nullptr;
    DWORD objectSize = 0;
    DWORD dataSize = 0;
    DWORD hashSize = 0;

    if (BCryptOpenAlgorithmProvider(&algorithmHandle, algorithm, nullptr, 0) != 0) {
        throw std::runtime_error("BCryptOpenAlgorithmProvider failed");
    }

    auto closeAlgorithm = [&]() {
        if (algorithmHandle != nullptr) {
            BCryptCloseAlgorithmProvider(algorithmHandle, 0);
        }
    };

    if (BCryptGetProperty(
            algorithmHandle,
            BCRYPT_OBJECT_LENGTH,
            reinterpret_cast<PUCHAR>(&objectSize),
            sizeof(objectSize),
            &dataSize,
            0) != 0) {
        closeAlgorithm();
        throw std::runtime_error("BCryptGetProperty object length failed");
    }

    if (BCryptGetProperty(
            algorithmHandle,
            BCRYPT_HASH_LENGTH,
            reinterpret_cast<PUCHAR>(&hashSize),
            sizeof(hashSize),
            &dataSize,
            0) != 0) {
        closeAlgorithm();
        throw std::runtime_error("BCryptGetProperty hash length failed");
    }

    std::vector<std::uint8_t> objectBuffer(objectSize);
    std::vector<std::uint8_t> hash(hashSize);

    if (BCryptCreateHash(
            algorithmHandle,
            &hashHandle,
            objectBuffer.data(),
            objectSize,
            nullptr,
            0,
            0) != 0) {
        closeAlgorithm();
        throw std::runtime_error("BCryptCreateHash failed");
    }

    auto closeHash = [&]() {
        if (hashHandle != nullptr) {
            BCryptDestroyHash(hashHandle);
        }
        closeAlgorithm();
    };

    if (size > 0) {
        if (BCryptHashData(
                hashHandle,
                const_cast<PUCHAR>(reinterpret_cast<const UCHAR*>(bytes)),
                static_cast<ULONG>(size),
                0) != 0) {
            closeHash();
            throw std::runtime_error("BCryptHashData failed");
        }
    }

    if (BCryptFinishHash(hashHandle, hash.data(), static_cast<ULONG>(hash.size()), 0) != 0) {
        closeHash();
        throw std::runtime_error("BCryptFinishHash failed");
    }

    closeHash();
    return hexLower(hash);
}

#define RECOMP_SHA1 BCRYPT_SHA1_ALGORITHM
#define RECOMP_SHA256 BCRYPT_SHA256_ALGORITHM
#else
std::uint32_t rotr(std::uint32_t value, unsigned bits)
{
    return (value >> bits) | (value << (32u - bits));
}

// FIPS 180-4 padding shared by SHA-1 and SHA-256: big-endian words and length.
template <typename Compress>
void padAndCompress(const std::uint8_t* bytes, std::size_t size, Compress compress)
{
    std::size_t full = size - size % 64u;
    for (std::size_t offset = 0; offset < full; offset += 64u) {
        compress(bytes + offset);
    }
    std::array<std::uint8_t, 128> tail{};
    const std::size_t rest = size - full;
    std::copy(bytes + full, bytes + size, tail.begin());
    tail[rest] = 0x80u;
    const std::size_t tailSize = rest < 56u ? 64u : 128u;
    const std::uint64_t bits = static_cast<std::uint64_t>(size) * 8u;
    for (unsigned i = 0; i < 8u; ++i) {
        tail[tailSize - 1u - i] = static_cast<std::uint8_t>(bits >> (8u * i));
    }
    for (std::size_t offset = 0; offset < tailSize; offset += 64u) {
        compress(tail.data() + offset);
    }
}

std::uint32_t loadBigEndian(const std::uint8_t* bytes)
{
    return (std::uint32_t{bytes[0]} << 24) | (std::uint32_t{bytes[1]} << 16) |
           (std::uint32_t{bytes[2]} << 8) | std::uint32_t{bytes[3]};
}

template <std::size_t Words>
std::string wordsHex(const std::array<std::uint32_t, Words>& state)
{
    std::vector<std::uint8_t> digest;
    for (const auto word : state) {
        for (int shift = 24; shift >= 0; shift -= 8) {
            digest.push_back(static_cast<std::uint8_t>(word >> shift));
        }
    }
    return hexLower(digest);
}

enum Algorithm { RECOMP_SHA1, RECOMP_SHA256 };

std::string hashHex(Algorithm algorithm, const std::uint8_t* bytes, std::size_t size)
{
    if (algorithm == RECOMP_SHA1) {
        std::array<std::uint32_t, 5> h{
            0x67452301u, 0xefcdab89u, 0x98badcfeu, 0x10325476u, 0xc3d2e1f0u};
        padAndCompress(bytes, size, [&](const std::uint8_t* block) {
            std::array<std::uint32_t, 80> w;
            for (unsigned i = 0; i < 16u; ++i) w[i] = loadBigEndian(block + 4u * i);
            for (unsigned i = 16; i < 80u; ++i) w[i] = rotr(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 31u);
            std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
            for (unsigned i = 0; i < 80u; ++i) {
                const std::uint32_t f = i < 20u ? ((b & c) | (~b & d)) + 0x5a827999u
                                      : i < 40u ? (b ^ c ^ d) + 0x6ed9eba1u
                                      : i < 60u ? ((b & c) | (b & d) | (c & d)) + 0x8f1bbcdcu
                                                : (b ^ c ^ d) + 0xca62c1d6u;
                const std::uint32_t t = rotr(a, 27u) + f + e + w[i];
                e = d; d = c; c = rotr(b, 2u); b = a; a = t;
            }
            h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
        });
        return wordsHex(h);
    }
    static const std::array<std::uint32_t, 64> k{
        0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
        0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
        0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
        0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
        0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
        0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
        0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
        0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};
    std::array<std::uint32_t, 8> h{0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                                   0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
    padAndCompress(bytes, size, [&](const std::uint8_t* block) {
        std::array<std::uint32_t, 64> w;
        for (unsigned i = 0; i < 16u; ++i) w[i] = loadBigEndian(block + 4u * i);
        for (unsigned i = 16; i < 64u; ++i) {
            const std::uint32_t s0 = rotr(w[i - 15], 7u) ^ rotr(w[i - 15], 18u) ^ (w[i - 15] >> 3);
            const std::uint32_t s1 = rotr(w[i - 2], 17u) ^ rotr(w[i - 2], 19u) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        std::array<std::uint32_t, 8> v = h;
        for (unsigned i = 0; i < 64u; ++i) {
            const std::uint32_t s1 = rotr(v[4], 6u) ^ rotr(v[4], 11u) ^ rotr(v[4], 25u);
            const std::uint32_t t1 = v[7] + s1 + ((v[4] & v[5]) ^ (~v[4] & v[6])) + k[i] + w[i];
            const std::uint32_t s0 = rotr(v[0], 2u) ^ rotr(v[0], 13u) ^ rotr(v[0], 22u);
            const std::uint32_t t2 = s0 + ((v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]));
            v = {t1 + t2, v[0], v[1], v[2], v[3] + t1, v[4], v[5], v[6]};
        }
        for (unsigned i = 0; i < 8u; ++i) h[i] += v[i];
    });
    return wordsHex(h);
}
#endif

} // namespace

std::string sha1Hex(const std::vector<std::uint8_t>& bytes)
{
    return sha1Hex(bytes.data(), bytes.size());
}

std::string sha1Hex(const std::uint8_t* bytes, std::size_t size)
{
    return hashHex(RECOMP_SHA1, bytes, size);
}

std::string sha256Hex(const std::vector<std::uint8_t>& bytes)
{
    return hashHex(RECOMP_SHA256, bytes.data(), bytes.size());
}

} // namespace doaxbv
