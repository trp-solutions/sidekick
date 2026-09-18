#include "edl_protocol.hpp"

namespace edl {

uint32_t crc32(const uint8_t *data, size_t length, uint32_t seed) {
	uint32_t crc = ~seed;
	for (size_t index = 0; index < length; ++index) {
		crc ^= data[index];
		for (uint8_t bit = 0; bit < 8; ++bit) {
			crc = (crc >> 1) ^ (0xEDB88320U & (0U - (crc & 1U)));
		}
	}
	return ~crc;
}

} // namespace edl
