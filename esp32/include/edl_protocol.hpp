#pragma once

#include <cstddef>
#include <cstdint>

namespace edl {

constexpr uint32_t PACKET_MAGIC = 0x314C4445; // "EDL1" on little-endian targets.
constexpr uint8_t PROTOCOL_VERSION = 1;
constexpr size_t PACKET_HEADER_SIZE = 20;
constexpr uint32_t MAX_JPEG_SIZE = 64 * 1024;
constexpr uint32_t MAX_PACKET_PAYLOAD = MAX_JPEG_SIZE + 16;
constexpr uint16_t FLAG_CLEAR = 0x0001;

enum class MessageType : uint8_t {
	HELLO = 1,
	INFO = 2,
	UPLOAD_BEGIN = 3,
	UPLOAD_CHUNK = 4,
	UPLOAD_COMMIT = 5,
	UPLOAD_ABORT = 6,
	STREAM_BEGIN = 7,
	STREAM_FRAME = 8,
	STREAM_END = 9,
	ACK = 10,
	NACK = 11,
	STREAM_RAW_FRAME = 12, // StreamFramePrefix + 160x160 RGB565, high byte first.
	BUTTON_EVENT = 14, // Unsolicited device-to-host event; one byte, values 1..8.
	RELAY_BEGIN = 13, // Enter unframed RGB565 mode until 2 seconds of USB inactivity.
};

enum class Error : uint16_t {
	NONE = 0,
	BAD_VERSION = 1,
	BAD_TYPE = 2,
	BAD_LENGTH = 3,
	BAD_CRC = 4,
	BAD_SEQUENCE = 5,
	BUSY = 6,
	NO_UPLOAD = 7,
	OUT_OF_RANGE = 8,
	BAD_JPEG = 9,
	BAD_ANIMATION = 10,
	FLASH_ERROR = 11,
	DECODE_ERROR = 12,
};

struct PacketHeader {
	uint32_t magic;
	uint8_t version;
	uint8_t type;
	uint16_t flags;
	uint32_t sequence;
	uint32_t payloadLength;
	uint32_t crc32;
} __attribute__((packed));

static_assert(sizeof(PacketHeader) == PACKET_HEADER_SIZE);

struct UploadBegin {
	uint32_t frameCount;
	uint32_t jpegBytes;
	uint64_t durationUs;
	uint32_t bodyCrc32;
} __attribute__((packed));

struct FrameIndex {
	uint32_t offset;
	uint32_t jpegLength;
	uint32_t durationUs;
} __attribute__((packed));

struct StreamFramePrefix {
	uint64_t presentationUs;
	uint32_t durationUs;
} __attribute__((packed));

struct ReplyPayload {
	uint8_t requestType;
	uint8_t credits;
	uint16_t error;
	uint32_t detail;
} __attribute__((packed));

enum class PlayerMode : uint8_t {
	BLACK = 0,
	SAVED = 1,
	LIVE = 2,
	UPLOAD = 3,
	IDLE = 4, // Built-in colors animation; no USB connection required.
};

struct InfoPayload {
	uint16_t firmwareMajor;
	uint16_t firmwareMinor;
	uint8_t protocolVersion;
	uint8_t mode;
	uint8_t hasSavedAnimation;
	uint8_t credits;
	uint32_t slotCapacity;
	uint32_t savedFrameCount;
	uint32_t savedJpegBytes;
	uint64_t savedDurationUs;
	uint32_t framesPresented;
	uint32_t framesDropped;
	uint32_t crcFailures;
	uint32_t decodeFailures;
	uint32_t decodeLastUs;
	uint32_t decodeAverageUs;
	uint32_t decodeMaximumUs;
} __attribute__((packed));

static_assert(sizeof(UploadBegin) == 20);
static_assert(sizeof(FrameIndex) == 12);
static_assert(sizeof(StreamFramePrefix) == 12);
static_assert(sizeof(ReplyPayload) == 8);
static_assert(sizeof(InfoPayload) == 56);

uint32_t crc32(const uint8_t *data, size_t length, uint32_t seed = 0);

inline uint32_t readU32(const uint8_t *bytes) {
	return static_cast<uint32_t>(bytes[0]) |
		(static_cast<uint32_t>(bytes[1]) << 8) |
		(static_cast<uint32_t>(bytes[2]) << 16) |
		(static_cast<uint32_t>(bytes[3]) << 24);
}

} // namespace edl
