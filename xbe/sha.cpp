#include "sha.h"

#ifdef _WIN32
#include <windows.h>
#include <array>
#include <bcrypt.h>
#elif defined(__APPLE__)
#include <CommonCrypto/CommonDigest.h>
#endif

#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace doaxbv {
namespace {

std::string hexLower(const std::uint8_t* bytes, std::size_t size)
{
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (std::size_t i = 0; i < size; ++i) {
        out << std::setw(2) << static_cast<int>(bytes[i]);
    }
    return out.str();
}

std::string hexLower(const std::vector<std::uint8_t>& bytes)
{
    return hexLower(bytes.data(), bytes.size());
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
#endif

} // namespace

#ifdef _WIN32
std::string sha1Hex(const std::vector<std::uint8_t>& bytes)
{
    return sha1Hex(bytes.data(), bytes.size());
}

std::string sha1Hex(const std::uint8_t* bytes, std::size_t size)
{
    return hashHex(BCRYPT_SHA1_ALGORITHM, bytes, size);
}

std::string sha256Hex(const std::vector<std::uint8_t>& bytes)
{
    return hashHex(BCRYPT_SHA256_ALGORITHM, bytes.data(), bytes.size());
}
#elif defined(__APPLE__)
std::string sha1Hex(const std::vector<std::uint8_t>& bytes)
{
    return sha1Hex(bytes.data(), bytes.size());
}

std::string sha1Hex(const std::uint8_t* bytes, std::size_t size)
{
    unsigned char digest[CC_SHA1_DIGEST_LENGTH];
    CC_SHA1(bytes, static_cast<CC_LONG>(size), digest);
    return hexLower(reinterpret_cast<const std::uint8_t*>(digest), sizeof(digest));
}

std::string sha256Hex(const std::vector<std::uint8_t>& bytes)
{
    unsigned char digest[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(bytes.data(), static_cast<CC_LONG>(bytes.size()), digest);
    return hexLower(reinterpret_cast<const std::uint8_t*>(digest), sizeof(digest));
}
#else
namespace {

inline uint32_t ror32(uint32_t x, uint32_t n) { return (x >> n) | (x << (32 - n)); }
inline uint32_t rol32(uint32_t x, uint32_t n) { return (x << n) | (x >> (32 - n)); }

std::vector<uint8_t> computeSha1(const uint8_t* data, size_t len)
{
    uint32_t h[5] = { 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0 };
    uint64_t bit_len = static_cast<uint64_t>(len) * 8;
    std::vector<uint8_t> msg(data, data + len);
    msg.push_back(0x80);
    while ((msg.size() % 64) != 56) {
        msg.push_back(0x00);
    }
    for (int i = 7; i >= 0; --i) {
        msg.push_back(static_cast<uint8_t>((bit_len >> (i * 8)) & 0xff));
    }
    for (size_t chunk = 0; chunk < msg.size(); chunk += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i) {
            w[i] = (static_cast<uint32_t>(msg[chunk + i * 4]) << 24) |
                   (static_cast<uint32_t>(msg[chunk + i * 4 + 1]) << 16) |
                   (static_cast<uint32_t>(msg[chunk + i * 4 + 2]) << 8) |
                   (static_cast<uint32_t>(msg[chunk + i * 4 + 3]));
        }
        for (int i = 16; i < 80; ++i) {
            w[i] = rol32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i) {
            uint32_t f, k;
            if (i < 20) {
                f = (b & c) | ((~b) & d);
                k = 0x5A827999;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDC;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6;
            }
            uint32_t temp = rol32(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = rol32(b, 30);
            b = a;
            a = temp;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }
    std::vector<uint8_t> digest(20);
    for (int i = 0; i < 5; ++i) {
        digest[i * 4] = static_cast<uint8_t>((h[i] >> 24) & 0xff);
        digest[i * 4 + 1] = static_cast<uint8_t>((h[i] >> 16) & 0xff);
        digest[i * 4 + 2] = static_cast<uint8_t>((h[i] >> 8) & 0xff);
        digest[i * 4 + 3] = static_cast<uint8_t>(h[i] & 0xff);
    }
    return digest;
}

std::vector<uint8_t> computeSha256(const uint8_t* data, size_t len)
{
    static const uint32_t K[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
    };
    uint32_t h[8] = {
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
    };
    uint64_t bit_len = static_cast<uint64_t>(len) * 8;
    std::vector<uint8_t> msg(data, data + len);
    msg.push_back(0x80);
    while ((msg.size() % 64) != 56) {
        msg.push_back(0x00);
    }
    for (int i = 7; i >= 0; --i) {
        msg.push_back(static_cast<uint8_t>((bit_len >> (i * 8)) & 0xff));
    }
    for (size_t chunk = 0; chunk < msg.size(); chunk += 64) {
        uint32_t w[64];
        for (int i = 0; i < 16; ++i) {
            w[i] = (static_cast<uint32_t>(msg[chunk + i * 4]) << 24) |
                   (static_cast<uint32_t>(msg[chunk + i * 4 + 1]) << 16) |
                   (static_cast<uint32_t>(msg[chunk + i * 4 + 2]) << 8) |
                   (static_cast<uint32_t>(msg[chunk + i * 4 + 3]));
        }
        for (int i = 16; i < 64; ++i) {
            uint32_t s0 = ror32(w[i - 15], 7) ^ ror32(w[i - 15], 18) ^ (w[i - 15] >> 3);
            uint32_t s1 = ror32(w[i - 2], 17) ^ ror32(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], h_val = h[7];
        for (int i = 0; i < 64; ++i) {
            uint32_t S1 = ror32(e, 6) ^ ror32(e, 11) ^ ror32(e, 25);
            uint32_t ch = (e & f) ^ ((~e) & g);
            uint32_t temp1 = h_val + S1 + ch + K[i] + w[i];
            uint32_t S0 = ror32(a, 2) ^ ror32(a, 13) ^ ror32(a, 22);
            uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            uint32_t temp2 = S0 + maj;

            h_val = g;
            g = f;
            f = e;
            e = d + temp1;
            d = c;
            c = b;
            b = a;
            a = temp1 + temp2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d;
        h[4] += e; h[5] += f; h[6] += g; h[7] += h_val;
    }
    std::vector<uint8_t> digest(32);
    for (int i = 0; i < 8; ++i) {
        digest[i * 4] = static_cast<uint8_t>((h[i] >> 24) & 0xff);
        digest[i * 4 + 1] = static_cast<uint8_t>((h[i] >> 16) & 0xff);
        digest[i * 4 + 2] = static_cast<uint8_t>((h[i] >> 8) & 0xff);
        digest[i * 4 + 3] = static_cast<uint8_t>(h[i] & 0xff);
    }
    return digest;
}

} // namespace

std::string sha1Hex(const std::vector<std::uint8_t>& bytes)
{
    return sha1Hex(bytes.data(), bytes.size());
}

std::string sha1Hex(const std::uint8_t* bytes, std::size_t size)
{
    return hexLower(computeSha1(bytes, size));
}

std::string sha256Hex(const std::vector<std::uint8_t>& bytes)
{
    return hexLower(computeSha256(bytes.data(), bytes.size()));
}
#endif

} // namespace doaxbv
