#pragma once

#include "edl_protocol.hpp"

#include <cstddef>
#include <cstdint>

#include "esp_partition.h"

namespace edl {

constexpr uint32_t SLOT_SIZE = 0x170000;
constexpr uint32_t SLOT_METADATA_SIZE = 16 * 1024;
constexpr uint32_t SLOT_HEADER_SIZE = 128;
constexpr uint32_t SLOT_PAYLOAD_CAPACITY = SLOT_SIZE - SLOT_METADATA_SIZE;
constexpr uint32_t SLOT_MAGIC = 0x314D4E41; // "ANM1"

struct SlotHeader {
	uint32_t magic;
	uint16_t formatVersion;
	uint16_t headerSize;
	uint32_t generation;
	uint32_t frameCount;
	uint32_t jpegBytes;
	uint64_t durationUs;
	uint32_t bodyCrc32;
	uint32_t headerCrc32;
	uint8_t reserved[SLOT_HEADER_SIZE - 36];
} __attribute__((packed));

static_assert(sizeof(SlotHeader) == SLOT_HEADER_SIZE);

class AnimationStore {
public:
	bool begin();
	bool hasAnimation() const;
	const SlotHeader &activeHeader() const;
	bool readFrameIndex(uint32_t index, FrameIndex &entry) const;
	bool readFrame(uint32_t index, FrameIndex &entry, uint8_t *jpeg, size_t capacity) const;
	bool readUploadFrame(uint32_t index, FrameIndex &entry, uint8_t *jpeg, size_t capacity) const;
	bool beginUpload(const UploadBegin &begin);
	bool writeUpload(uint32_t virtualOffset, const uint8_t *data, size_t length);
	bool verifyUpload();
	bool commitUpload();
	void abortUpload();
	bool clear();
	bool uploadActive() const;
	uint32_t expectedUploadOffset() const;
	uint32_t uploadFrameCount() const;

private:
	bool loadSlot(int slot, SlotHeader &header) const;
	bool readFrameFrom(const esp_partition_t *partition, const SlotHeader &header,
		uint32_t index, FrameIndex &entry, uint8_t *jpeg, size_t capacity) const;
	const esp_partition_t *partitions_[2]{};
	SlotHeader activeHeader_{};
	UploadBegin uploadBegin_{};
	int activeSlot_ = -1;
	int uploadSlot_ = -1;
	uint32_t uploadOffset_ = 0;
};

} // namespace edl
