#include "goldrop/management_crypto.hpp"
#include <iostream>
#include <string>
#include <iterator>
using namespace goldrop;
int main(int argc, char **argv) {
    if (argc != 2)
        return 2;
    std::array<uint8_t, 32> key{};
    for (unsigned i = 0; i < key.size(); ++i)
        key[i] = uint8_t(i);
    DeviceId id{{0x90, 0xda, 0x72, 0x9b, 0xcd, 0x4c}};
    if (std::string(argv[1]) == "fingerprint") {
        char hex[17];
        if (!management_key_id(key, hex))
            return 3;
        std::cout << hex;
        return 0;
    }
    std::string input(std::istreambuf_iterator<char>{std::cin}, {});
    if (input.empty())
        return 2;
    std::array<uint8_t, 1024> output{};
    std::size_t n{};
    const auto *bytes = reinterpret_cast<const uint8_t *>(input.data() + 1);
    const bool ok =
        std::string(argv[1]) == "encrypt"
            ? management_encrypt(key, id, input[0], bytes, input.size() - 1, output.data(), output.size(), n)
            : management_decrypt(key, id, input[0], bytes, input.size() - 1, output.data(), output.size(), n);
    if (!ok)
        return 3;
    std::cout.write(reinterpret_cast<char *>(output.data()), n);
    return 0;
}
