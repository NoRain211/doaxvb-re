#include "sha.h"

#include <cstdio>
#include <cstring>

static int failures = 0;

static void expect(const std::string& actual, const char* expected, const char* label)
{
    if (actual != expected) {
        std::fprintf(stderr, "FAIL %s: %s\n", label, actual.c_str());
        ++failures;
    }
}

static std::vector<std::uint8_t> bytesOf(const char* text)
{
    return std::vector<std::uint8_t>(text, text + std::strlen(text));
}

int main()
{
    // FIPS 180 example vectors: empty, one block, and a two-block padding case.
    const char* two = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    expect(doaxbv::sha1Hex(bytesOf("")), "da39a3ee5e6b4b0d3255bfef95601890afd80709", "sha1 empty");
    expect(doaxbv::sha1Hex(bytesOf("abc")), "a9993e364706816aba3e25717850c26c9cd0d89d", "sha1 abc");
    expect(doaxbv::sha1Hex(bytesOf(two)), "84983e441c3bd26ebaae4aa1f95129e5e54670f1", "sha1 two blocks");
    expect(doaxbv::sha256Hex(bytesOf("")),
           "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "sha256 empty");
    expect(doaxbv::sha256Hex(bytesOf("abc")),
           "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "sha256 abc");
    expect(doaxbv::sha256Hex(bytesOf(two)),
           "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1", "sha256 two blocks");
    return failures == 0 ? 0 : 1;
}
