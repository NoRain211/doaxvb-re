#include "sha.h"

#include <cstdio>
#include <string>
#include <vector>

// Known answers from Python hashlib; lengths cross the 55/56/64-byte padding edges.
struct Case { std::size_t length; const char *sha1, *sha256; };

static const Case kCases[] = {
    {0, "da39a3ee5e6b4b0d3255bfef95601890afd80709",
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
    {1, "86f7e437faa5a7fce15d1ddcb9eaeaea377667b8",
        "ca978112ca1bbdcafac231b39a23dc4da786eff8147c4e72b9807785afee48bb"},
    {55, "c1c8bbdc22796e28c0e15163d20899b65621d65a",
        "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318"},
    {56, "c2db330f6083854c99d4b5bfb6e8f29f201be699",
        "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a"},
    {63, "03f09f5b158a7a8cdad920bddc29b81c18a551f5",
        "7d3e74a05d7db15bce4ad9ec0658ea98e3f06eeecf16b4c6fff2da457ddc2f34"},
    {64, "0098ba824b5c16427bd7a1122a5a442a25ec644d",
        "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb"},
    {65, "11655326c708d70319be2610e8a57d9a5b959d3b",
        "635361c48bb9eab14198e76ea8ab7f1a41685d6ad62aa9146d301d4f17eb0ae0"},
    {1000000, "34aa973cd4c4daa4f61eeb2bdbad27316534016f",
        "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"},
};

int main()
{
    int failures = 0;
    for (const Case &c : kCases) {
        const std::vector<std::uint8_t> bytes(c.length, 'a');
        const std::string sha1 = doaxbv::sha1Hex(bytes), sha256 = doaxbv::sha256Hex(bytes);
        if (sha1 != c.sha1 || sha256 != c.sha256 ||
            doaxbv::sha1Hex(bytes.data(), bytes.size()) != c.sha1) {
            std::fprintf(stderr, "sha mismatch for %zu bytes: %s %s\n",
                c.length, sha1.c_str(), sha256.c_str());
            ++failures;
        }
    }
    return failures == 0 ? 0 : 1;
}
