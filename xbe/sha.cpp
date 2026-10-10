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
#endif

} // namespace doaxbv
