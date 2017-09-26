//g++  5.4.0

// Compile with:
// g++ -std=c++11 -O3 && a.out
#include <memory>
#include <cstdint>

using namespace std;

__attribute__ ((noinline))
void SerializeTo(const uint64_t * const & v, size_t len, uint8_t* dest) {
  for (size_t i = 0; i < len; ++i) {
    *reinterpret_cast<uint64_t*>(dest) = v[i];
    dest += sizeof(v[i]);
  }
}

int main() {
 unique_ptr<uint64_t[]> d(new uint64_t[64]);

 unique_ptr<uint8_t[]> tmp(new uint8_t[1024]);

 SerializeTo(d.get(), 64, tmp.get() + 4);

 return 0;
}
