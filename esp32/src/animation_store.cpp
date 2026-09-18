#include "animation_store.hpp"

#include <algorithm>
#include <cstring>

namespace edl {

namespace {

uint32_t headerCrc(const SlotHeader &header) {
	SlotHeader copy = header;
	copy.headerCrc32 = 0;
	return crc32(reinterpret_cast<const uint8_t *>(&copy), sizeof(copy));
}

} // namespace

bool AnimationStore::begin() {
	partitions_[0] = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
		static_cast<esp_partition_subtype_t>(0x40), "animation0");
	partitions_[1] = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
		static_cast<esp_partition_subtype_t>(0x41), "animation1");
	if (partitions_[0] == nullptr || partitions_[1] == nullptr ||
		partitions_[0]->size != SLOT_SIZE || partitions_[1]->size != SLOT_SIZE) {
		return false;
	}

	SlotHeader headers[2]{};
	const bool valid0 = loadSlot(0, headers[0]);
	const bool valid1 = loadSlot(1, headers[1]);
	if (valid0 && (!valid1 || headers[0].generation >= headers[1].generation)) {
		activeSlot_ = 0;
		activeHeader_ = headers[0];
	} else if (valid1) {
		activeSlot_ = 1;
		activeHeader_ = headers[1];
	}
	return true;
}

bool AnimationStore::loadSlot(int slot, SlotHeader &header) const {
	if (esp_partition_read(partitions_[slot], 0, &header, sizeof(header)) != ESP_OK ||
		header.magic != SLOT_MAGIC || header.formatVersion != 1 ||
		header.headerSize != SLOT_HEADER_SIZE || header.headerCrc32 != headerCrc(header) ||
		header.frameCount == 0 || header.jpegBytes > SLOT_PAYLOAD_CAPACITY ||
		header.frameCount > (SLOT_METADATA_SIZE - SLOT_HEADER_SIZE) / sizeof(FrameIndex)) {
		return false;
	}

	uint8_t buffer[1024];
	uint32_t crc = 0;
	uint32_t remaining = header.frameCount * sizeof(FrameIndex);
	uint32_t offset = SLOT_HEADER_SIZE;
	while (remaining > 0) {
		const uint32_t amount = std::min<uint32_t>(remaining, sizeof(buffer));
		if (esp_partition_read(partitions_[slot], offset, buffer, amount) != ESP_OK) {
			return false;
		}
		crc = crc32(buffer, amount, crc);
		offset += amount;
		remaining -= amount;
	}
	remaining = header.jpegBytes;
	offset = SLOT_METADATA_SIZE;
	while (remaining > 0) {
		const uint32_t amount = std::min<uint32_t>(remaining, sizeof(buffer));
		if (esp_partition_read(partitions_[slot], offset, buffer, amount) != ESP_OK) {
			return false;
		}
		crc = crc32(buffer, amount, crc);
		offset += amount;
		remaining -= amount;
	}
	if (crc != header.bodyCrc32) return false;
	uint32_t expectedOffset = 0;
	uint64_t duration = 0;
	for (uint32_t index = 0; index < header.frameCount; ++index) {
		FrameIndex entry{};
		if (esp_partition_read(partitions_[slot], SLOT_HEADER_SIZE + index * sizeof(entry), &entry, sizeof(entry)) != ESP_OK ||
			entry.offset != expectedOffset || entry.jpegLength == 0 || entry.jpegLength > MAX_JPEG_SIZE || entry.durationUs == 0 ||
			expectedOffset > header.jpegBytes || entry.jpegLength > header.jpegBytes - expectedOffset) return false;
		expectedOffset += entry.jpegLength;
		duration += entry.durationUs;
	}
	return expectedOffset == header.jpegBytes && duration == header.durationUs;
}

bool AnimationStore::hasAnimation() const {
	return activeSlot_ >= 0;
}

const SlotHeader &AnimationStore::activeHeader() const {
	return activeHeader_;
}

bool AnimationStore::readFrameIndex(uint32_t index, FrameIndex &entry) const {
	return activeSlot_ >= 0 && index < activeHeader_.frameCount &&
		esp_partition_read(partitions_[activeSlot_], SLOT_HEADER_SIZE + index * sizeof(entry),
			&entry, sizeof(entry)) == ESP_OK && entry.jpegLength > 0 && entry.jpegLength <= MAX_JPEG_SIZE &&
		entry.durationUs > 0 && entry.offset <= activeHeader_.jpegBytes &&
		entry.jpegLength <= activeHeader_.jpegBytes - entry.offset;
}

bool AnimationStore::readFrameFrom(const esp_partition_t *partition, const SlotHeader &header,
	uint32_t index, FrameIndex &entry, uint8_t *jpeg, size_t capacity) const {
	if (partition == nullptr || index >= header.frameCount ||
		esp_partition_read(partition, SLOT_HEADER_SIZE + index * sizeof(FrameIndex),
			&entry, sizeof(entry)) != ESP_OK ||
		entry.jpegLength == 0 || entry.jpegLength > MAX_JPEG_SIZE || entry.jpegLength > capacity ||
		entry.offset > header.jpegBytes || entry.jpegLength > header.jpegBytes - entry.offset) {
		return false;
	}
	return esp_partition_read(partition, SLOT_METADATA_SIZE + entry.offset,
		jpeg, entry.jpegLength) == ESP_OK;
}

bool AnimationStore::readFrame(uint32_t index, FrameIndex &entry, uint8_t *jpeg, size_t capacity) const {
	return activeSlot_ >= 0 && readFrameFrom(partitions_[activeSlot_], activeHeader_, index, entry, jpeg, capacity);
}

bool AnimationStore::readUploadFrame(uint32_t index, FrameIndex &entry, uint8_t *jpeg, size_t capacity) const {
	SlotHeader header{};
	header.frameCount = uploadBegin_.frameCount;
	header.jpegBytes = uploadBegin_.jpegBytes;
	return uploadSlot_ >= 0 && readFrameFrom(partitions_[uploadSlot_], header, index, entry, jpeg, capacity);
}

bool AnimationStore::beginUpload(const UploadBegin &beginData) {
	if (partitions_[0] == nullptr || partitions_[1] == nullptr || uploadSlot_ >= 0 ||
		beginData.frameCount == 0 || beginData.jpegBytes == 0 ||
		beginData.jpegBytes > SLOT_PAYLOAD_CAPACITY ||
		beginData.frameCount > (SLOT_METADATA_SIZE - SLOT_HEADER_SIZE) / sizeof(FrameIndex)) {
		return false;
	}
	uploadSlot_ = activeSlot_ == 0 ? 1 : 0;
	if (esp_partition_erase_range(partitions_[uploadSlot_], 0, SLOT_SIZE) != ESP_OK) {
		uploadSlot_ = -1;
		return false;
	}
	uploadBegin_ = beginData;
	uploadOffset_ = 0;
	return true;
}

bool AnimationStore::writeUpload(uint32_t virtualOffset, const uint8_t *data, size_t length) {
	if (uploadSlot_ < 0 || virtualOffset != uploadOffset_) {
		return false;
	}
	const uint32_t indexBytes = uploadBegin_.frameCount * sizeof(FrameIndex);
	const uint32_t totalBytes = indexBytes + uploadBegin_.jpegBytes;
	if (length > totalBytes - uploadOffset_) {
		return false;
	}

	size_t consumed = 0;
	if (uploadOffset_ < indexBytes) {
		const size_t amount = std::min<size_t>(length, indexBytes - uploadOffset_);
		if (esp_partition_write(partitions_[uploadSlot_], SLOT_HEADER_SIZE + uploadOffset_, data, amount) != ESP_OK) {
			return false;
		}
		consumed = amount;
	}
	if (consumed < length) {
		const uint32_t jpegOffset = uploadOffset_ + consumed - indexBytes;
		if (esp_partition_write(partitions_[uploadSlot_], SLOT_METADATA_SIZE + jpegOffset,
			data + consumed, length - consumed) != ESP_OK) {
			return false;
		}
	}
	uploadOffset_ += length;
	return true;
}

bool AnimationStore::verifyUpload() {
	if (uploadSlot_ < 0) {
		return false;
	}
	const uint32_t indexBytes = uploadBegin_.frameCount * sizeof(FrameIndex);
	if (uploadOffset_ != indexBytes + uploadBegin_.jpegBytes) {
		return false;
	}

	uint32_t expectedOffset = 0;
	uint64_t duration = 0;
	for (uint32_t index = 0; index < uploadBegin_.frameCount; ++index) {
		FrameIndex entry{};
		if (esp_partition_read(partitions_[uploadSlot_], SLOT_HEADER_SIZE + index * sizeof(entry),
			&entry, sizeof(entry)) != ESP_OK || entry.offset != expectedOffset ||
			entry.jpegLength == 0 || entry.jpegLength > MAX_JPEG_SIZE || entry.durationUs == 0 ||
			expectedOffset > uploadBegin_.jpegBytes || entry.jpegLength > uploadBegin_.jpegBytes - expectedOffset) {
			return false;
		}
		expectedOffset += entry.jpegLength;
		duration += entry.durationUs;
	}
	if (expectedOffset != uploadBegin_.jpegBytes || duration != uploadBegin_.durationUs) {
		return false;
	}

	uint8_t buffer[1024];
	uint32_t crc = 0;
	uint32_t remaining = indexBytes;
	uint32_t offset = SLOT_HEADER_SIZE;
	while (remaining > 0) {
		const uint32_t amount = std::min<uint32_t>(remaining, sizeof(buffer));
		if (esp_partition_read(partitions_[uploadSlot_], offset, buffer, amount) != ESP_OK) return false;
		crc = crc32(buffer, amount, crc);
		offset += amount;
		remaining -= amount;
	}
	remaining = uploadBegin_.jpegBytes;
	offset = SLOT_METADATA_SIZE;
	while (remaining > 0) {
		const uint32_t amount = std::min<uint32_t>(remaining, sizeof(buffer));
		if (esp_partition_read(partitions_[uploadSlot_], offset, buffer, amount) != ESP_OK) return false;
		crc = crc32(buffer, amount, crc);
		offset += amount;
		remaining -= amount;
	}
	return crc == uploadBegin_.bodyCrc32;
}

bool AnimationStore::commitUpload() {
	if (!verifyUpload()) {
		return false;
	}
	SlotHeader header{};
	header.magic = SLOT_MAGIC;
	header.formatVersion = 1;
	header.headerSize = SLOT_HEADER_SIZE;
	header.generation = activeSlot_ < 0 ? 1 : activeHeader_.generation + 1;
	header.frameCount = uploadBegin_.frameCount;
	header.jpegBytes = uploadBegin_.jpegBytes;
	header.durationUs = uploadBegin_.durationUs;
	header.bodyCrc32 = uploadBegin_.bodyCrc32;
	header.headerCrc32 = headerCrc(header);
	if (esp_partition_write(partitions_[uploadSlot_], 0, &header, sizeof(header)) != ESP_OK) {
		return false;
	}
	activeSlot_ = uploadSlot_;
	activeHeader_ = header;
	uploadSlot_ = -1;
	return true;
}

void AnimationStore::abortUpload() {
	uploadSlot_ = -1;
	uploadOffset_ = 0;
}

bool AnimationStore::clear() {
	abortUpload();
	if (partitions_[0] == nullptr || partitions_[1] == nullptr) return false;
	bool ok = true;
	for (const auto *partition : partitions_) {
		ok = esp_partition_erase_range(partition, 0, 4096) == ESP_OK && ok;
	}
	activeSlot_ = -1;
	activeHeader_ = {};
	return ok;
}

bool AnimationStore::uploadActive() const {
	return uploadSlot_ >= 0;
}

uint32_t AnimationStore::expectedUploadOffset() const {
	return uploadOffset_;
}

uint32_t AnimationStore::uploadFrameCount() const {
	return uploadSlot_ < 0 ? 0 : uploadBegin_.frameCount;
}

} // namespace edl
