#include <Arduino.h>

#include "animation_store.hpp"
#include "buttons.hpp"
#include "edl_protocol.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "driver/spi_master.h"
#include "esp32c5/rom/tjpgd.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"

namespace {

extern const uint8_t idleVideo[] asm("_binary_media_colors_idle_bin_start");
extern const uint8_t idleVideoEnd[] asm("_binary_media_colors_idle_bin_end");
uint64_t idleStartedUs = 0;
uint64_t idleFrameNumber = UINT64_MAX;

constexpr int PIN_BUTTON_1 = 23;
constexpr int PIN_BUTTON_2 = 24;
edl::Buttons buttons;
uint32_t buttonSequence = 0;

constexpr int PIN_LCD_MOSI = 7;
constexpr int PIN_LCD_SCLK = 6;
constexpr int PIN_LCD_CS = 10;
constexpr int PIN_LCD_DC = 8;
constexpr int PIN_LCD_RST = 9;
constexpr int PIN_LCD_BL = 1;
constexpr int LCD_WIDTH = 160;
constexpr int LCD_HEIGHT = 160;
constexpr uint32_t LCD_SPI_HZ = 27'000'000;
constexpr size_t FRAME_BYTES = LCD_WIDTH * LCD_HEIGHT * 2;
// C5 SPI DMA transactions are limited to 262143 bits (less than 32 KiB).
constexpr int STRIPE_ROWS = 80;
constexpr size_t STRIPE_BYTES = LCD_WIDTH * STRIPE_ROWS * 2;
static_assert(LCD_HEIGHT % STRIPE_ROWS == 0);
static_assert(STRIPE_BYTES * 8 <= 262143);
constexpr uint64_t LIVE_TIMEOUT_US = 2'000'000;

spi_device_handle_t display = nullptr;
spi_transaction_t frameTransaction{};
bool frameQueued = false;
uint8_t *compressedBuffer = nullptr;
uint8_t *frameBuffers[2]{};
uint8_t decodeBufferIndex = 0;
std::array<uint8_t, 4096> jpegWorkPool{};

edl::AnimationStore animationStore;
edl::PlayerMode mode = edl::PlayerMode::BLACK;

struct Statistics {
	uint32_t presented = 0;
	uint32_t dropped = 0;
	uint32_t crcFailures = 0;
	uint32_t decodeFailures = 0;
	uint32_t decodeLastUs = 0;
	uint32_t decodeMaximumUs = 0;
	uint64_t decodeTotalUs = 0;
	uint32_t decoded = 0;
} statistics;

struct Receiver {
	std::array<uint8_t, edl::PACKET_HEADER_SIZE> headerBytes{};
	size_t headerUsed = 0;
	edl::PacketHeader header{};
	uint32_t payloadUsed = 0;
	bool pendingLiveFrame = false;
} receiver;

uint32_t expectedUploadSequence = 0;
uint32_t expectedStreamSequence = 0;
uint64_t lastUploadPacketUs = 0;
bool streamArmed = false;
uint64_t lastLiveFrameUs = 0;
uint64_t lastUsbByteUs = 0;
uint64_t liveLocalOriginUs = 0;
uint64_t liveRemoteOriginUs = 0;
uint64_t pendingPresentationUs = 0;
uint32_t pendingDurationUs = 0;
uint32_t pendingJpegLength = 0;
bool pendingRawFrame = false;
bool relayActive = false;
size_t relayBytes = 0;
uint64_t relayLastByteUs = 0;

void waitForFrame() {
	if (!frameQueued) return;
	spi_transaction_t *completed = nullptr;
	ESP_ERROR_CHECK(spi_device_get_trans_result(display, &completed, portMAX_DELAY));
	frameQueued = false;
}

bool transmit(bool dataMode, const uint8_t *bytes, size_t length, bool queued = false) {
	if (!queued) waitForFrame();
	digitalWrite(PIN_LCD_DC, dataMode ? HIGH : LOW);
	spi_transaction_t transaction{};
	transaction.length = length * 8;
	transaction.tx_buffer = bytes;
	if (!queued) {
		ESP_ERROR_CHECK(spi_device_polling_transmit(display, &transaction));
		return true;
	}
	frameTransaction = transaction;
	ESP_ERROR_CHECK(spi_device_queue_trans(display, &frameTransaction, portMAX_DELAY));
	frameQueued = true;
	return true;
}

// Require a length for pointers so array calls select the sized overload below.
void writeCommand(uint8_t command, const uint8_t *data, size_t length) {
	transmit(false, &command, 1);
	if (length > 0) transmit(true, data, length);
}

void writeCommand(uint8_t command) {
	writeCommand(command, nullptr, 0);
}

template <size_t N>
void writeCommand(uint8_t command, const uint8_t (&data)[N]) {
	writeCommand(command, data, N);
}

void initDisplay() {
	digitalWrite(PIN_LCD_RST, HIGH);
	delay(10);
	digitalWrite(PIN_LCD_RST, LOW);
	delay(100);
	digitalWrite(PIN_LCD_RST, HIGH);
	delay(120);

	writeCommand(0xFE);
	writeCommand(0xEF);
	for (uint8_t command = 0x80; command <= 0x8F; ++command) writeCommand(command, (const uint8_t[]){0xFF}, 1);
	writeCommand(0x3A, (const uint8_t[]){0x05}, 1);
	writeCommand(0xEC, (const uint8_t[]){0x01}, 1);
	writeCommand(0x74, (const uint8_t[]){0x02, 0x0E, 0x00, 0x00, 0x00, 0x00, 0x00}, 7);
	writeCommand(0x98, (const uint8_t[]){0x3E}, 1);
	writeCommand(0x99, (const uint8_t[]){0x3E}, 1);
	writeCommand(0xB5, (const uint8_t[]){0x0D, 0x0D}, 2);
	writeCommand(0x60, (const uint8_t[]){0x38, 0x0F, 0x79, 0x67}, 4);
	writeCommand(0x61, (const uint8_t[]){0x38, 0x11, 0x79, 0x67}, 4);
	writeCommand(0x64, (const uint8_t[]){0x38, 0x17, 0x71, 0x5F, 0x79, 0x67}, 6);
	writeCommand(0x65, (const uint8_t[]){0x38, 0x13, 0x71, 0x5B, 0x79, 0x67}, 6);
	writeCommand(0x6A, (const uint8_t[]){0x00, 0x00}, 2);
	writeCommand(0x6C, (const uint8_t[]){0x22, 0x02, 0x22, 0x02, 0x22, 0x22, 0x50}, 7);
	writeCommand(0x6E, (const uint8_t[]){
		0x03, 0x03, 0x01, 0x01, 0x00, 0x00, 0x0F, 0x0F, 0x0D, 0x0D, 0x0B, 0x0B, 0x09, 0x09, 0x00, 0x00,
		0x00, 0x00, 0x0A, 0x0A, 0x0C, 0x0C, 0x0E, 0x0E, 0x10, 0x10, 0x00, 0x00, 0x02, 0x02, 0x04, 0x04
	}, 32);
	writeCommand(0xBF, (const uint8_t[]){0x01}, 1);
	writeCommand(0xF9, (const uint8_t[]){0x40}, 1);
	writeCommand(0x9B, (const uint8_t[]){0x3B}, 1);
	writeCommand(0x93, (const uint8_t[]){0x33, 0x7F, 0x00}, 3);
	writeCommand(0x7E, (const uint8_t[]){0x30}, 1);
	writeCommand(0x70, (const uint8_t[]){0x0D, 0x02, 0x08, 0x0D, 0x02, 0x08}, 6);
	writeCommand(0x71, (const uint8_t[]){0x0D, 0x02, 0x08}, 3);
	writeCommand(0x91, (const uint8_t[]){0x0E, 0x09}, 2);
	writeCommand(0xC3, (const uint8_t[]){0x19}, 1);
	writeCommand(0xC4, (const uint8_t[]){0x19}, 1);
	writeCommand(0xC9, (const uint8_t[]){0x3C}, 1);
	writeCommand(0xF0, (const uint8_t[]){0x53, 0x15, 0x0A, 0x04, 0x00, 0x3E}, 6);
	writeCommand(0xF2, (const uint8_t[]){0x53, 0x15, 0x0A, 0x04, 0x00, 0x3A}, 6);
	writeCommand(0xF1, (const uint8_t[]){0x56, 0xA8, 0x7F, 0x33, 0x34, 0x5F}, 6);
	writeCommand(0xF3, (const uint8_t[]){0x52, 0xA4, 0x7F, 0x33, 0x34, 0xDF}, 6);
	writeCommand(0x36, (const uint8_t[]){0x00}, 1);
	writeCommand(0x11);
	delay(200);
	writeCommand(0x29);
}

void presentFrame(uint8_t *frame) {
	const uint8_t columns[] = {0, 0, 0, LCD_WIDTH - 1};
	for (int y = 0; y < LCD_HEIGHT; y += STRIPE_ROWS) {
		// Each stripe has its own address window and RAM-write command.
		// Wait before changing DC or reusing the transaction descriptor.
		waitForFrame();
		const uint8_t rows[] = {0, static_cast<uint8_t>(y),
			0, static_cast<uint8_t>(y + STRIPE_ROWS - 1)};
		writeCommand(0x2A, columns);
		writeCommand(0x2B, rows);
		writeCommand(0x2C);
		transmit(true, frame + y * LCD_WIDTH * 2, STRIPE_BYTES, true);
	}
	statistics.presented++;
}

bool validateJpeg(const uint8_t *jpeg, size_t length) {
	if (length < 4 || length > edl::MAX_JPEG_SIZE || jpeg[0] != 0xFF || jpeg[1] != 0xD8 ||
		jpeg[length - 2] != 0xFF || jpeg[length - 1] != 0xD9) return false;
	bool baseline = false;
	for (size_t offset = 2; offset + 3 < length;) {
		if (jpeg[offset++] != 0xFF) continue;
		while (offset < length && jpeg[offset] == 0xFF) offset++;
		if (offset >= length) return false;
		const uint8_t marker = jpeg[offset++];
		if (marker == 0xD9 || marker == 0xDA) break;
		if ((marker >= 0xD0 && marker <= 0xD7) || marker == 0x01) continue;
		if (offset + 2 > length) return false;
		const uint16_t segmentLength = static_cast<uint16_t>(jpeg[offset] << 8 | jpeg[offset + 1]);
		if (segmentLength < 2 || offset + segmentLength > length) return false;
		if (marker == 0xC0) {
			if (segmentLength < 8 || jpeg[offset + 2] != 8) return false;
			const uint16_t height = static_cast<uint16_t>(jpeg[offset + 3] << 8 | jpeg[offset + 4]);
			const uint16_t width = static_cast<uint16_t>(jpeg[offset + 5] << 8 | jpeg[offset + 6]);
			baseline = width == LCD_WIDTH && height == LCD_HEIGHT;
		} else if (marker >= 0xC1 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC) {
			return false;
		}
		offset += segmentLength;
	}
	return baseline;
}

struct JpegDecodeContext {
	const uint8_t *input;
	size_t inputLength;
	size_t inputOffset;
	uint8_t *output;
};

UINT jpegInput(JDEC *decoderObject, BYTE *destination, UINT amount) {
	auto *context = static_cast<JpegDecodeContext *>(decoderObject->device);
	const size_t available = context->inputLength - context->inputOffset;
	const UINT actual = std::min<size_t>(amount, available);
	if (destination != nullptr) memcpy(destination, context->input + context->inputOffset, actual);
	context->inputOffset += actual;
	return actual;
}

UINT jpegOutput(JDEC *decoderObject, void *bitmap, JRECT *rectangle) {
	auto *context = static_cast<JpegDecodeContext *>(decoderObject->device);
	const auto *rgb = static_cast<const uint8_t *>(bitmap);
	for (uint16_t y = rectangle->top; y <= rectangle->bottom; ++y) {
		for (uint16_t x = rectangle->left; x <= rectangle->right; ++x) {
			const uint16_t color = static_cast<uint16_t>(((rgb[0] & 0xF8) << 8) |
				((rgb[1] & 0xFC) << 3) | (rgb[2] >> 3));
			const size_t outputOffset = (static_cast<size_t>(y) * LCD_WIDTH + x) * 2;
			context->output[outputOffset] = static_cast<uint8_t>(color >> 8);
			context->output[outputOffset + 1] = static_cast<uint8_t>(color);
			rgb += 3;
		}
	}
	return 1;
}

bool decodeJpeg(const uint8_t *jpeg, size_t length) {
	if (!validateJpeg(jpeg, length)) {
		statistics.decodeFailures++;
		return false;
	}
	const int64_t started = esp_timer_get_time();
	JDEC decoderObject{};
	JpegDecodeContext context{jpeg, length, 0, frameBuffers[decodeBufferIndex]};
	JRESULT result = jd_prepare(&decoderObject, jpegInput, jpegWorkPool.data(), jpegWorkPool.size(), &context);
	if (result == JDR_OK && (decoderObject.width != LCD_WIDTH || decoderObject.height != LCD_HEIGHT)) result = JDR_FMT1;
	if (result == JDR_OK) result = jd_decomp(&decoderObject, jpegOutput, 0);
	const uint32_t elapsed = static_cast<uint32_t>(esp_timer_get_time() - started);
	statistics.decodeLastUs = elapsed;
	statistics.decodeMaximumUs = std::max(statistics.decodeMaximumUs, elapsed);
	statistics.decodeTotalUs += elapsed;
	statistics.decoded++;
	if (result != JDR_OK) {
		statistics.decodeFailures++;
		return false;
	}
	presentFrame(frameBuffers[decodeBufferIndex]);
	decodeBufferIndex ^= 1;
	return true;
}

void resetFallback() {
	mode = edl::PlayerMode::IDLE;
	idleStartedUs = esp_timer_get_time();
	idleFrameNumber = UINT64_MAX;
	streamArmed = false;
	liveLocalOriginUs = 0;
	liveRemoteOriginUs = 0;
}

void sendPacket(edl::MessageType type, uint32_t sequence, const void *payload, uint32_t length) {
	edl::PacketHeader header{};
	header.magic = edl::PACKET_MAGIC;
	header.version = edl::PROTOCOL_VERSION;
	header.type = static_cast<uint8_t>(type);
	header.sequence = sequence;
	header.payloadLength = length;
	header.crc32 = edl::crc32(reinterpret_cast<const uint8_t *>(&header), offsetof(edl::PacketHeader, crc32));
	if (length > 0) header.crc32 = edl::crc32(static_cast<const uint8_t *>(payload), length, header.crc32);
	Serial.write(reinterpret_cast<const uint8_t *>(&header), sizeof(header));
	if (length > 0) Serial.write(static_cast<const uint8_t *>(payload), length);
}

void pollButtons() {
	if (!relayActive || !Serial.isPlugged()) {
		buttons = edl::Buttons{};
		return;
	}
	const uint8_t mask = (digitalRead(PIN_BUTTON_1) == LOW ? 1 : 0) |
		(digitalRead(PIN_BUTTON_2) == LOW ? 2 : 0);
	const uint8_t event = buttons.update(mask, millis());
	// Never stall video streaming if the host stops reading USB.
	if (event && Serial.availableForWrite() >= static_cast<int>(edl::PACKET_HEADER_SIZE + 1)) {
		sendPacket(edl::MessageType::BUTTON_EVENT, ++buttonSequence, &event, 1);
	}
}

void sendReply(const edl::PacketHeader &request, edl::Error error, uint32_t detail = 0, uint8_t credits = 1) {
	edl::ReplyPayload reply{request.type, credits, static_cast<uint16_t>(error), detail};
	sendPacket(error == edl::Error::NONE ? edl::MessageType::ACK : edl::MessageType::NACK,
		request.sequence, &reply, sizeof(reply));
}

void sendInfo(uint32_t sequence) {
	const auto &saved = animationStore.activeHeader();
	edl::InfoPayload info{};
	info.firmwareMajor = 1;
	info.firmwareMinor = 8;
	info.protocolVersion = edl::PROTOCOL_VERSION;
	info.mode = static_cast<uint8_t>(mode);
	info.hasSavedAnimation = animationStore.hasAnimation();
	info.credits = receiver.pendingLiveFrame ? 0 : 1;
	info.slotCapacity = edl::SLOT_PAYLOAD_CAPACITY;
	info.savedFrameCount = animationStore.hasAnimation() ? saved.frameCount : 0;
	info.savedJpegBytes = animationStore.hasAnimation() ? saved.jpegBytes : 0;
	info.savedDurationUs = animationStore.hasAnimation() ? saved.durationUs : 0;
	info.framesPresented = statistics.presented;
	info.framesDropped = statistics.dropped;
	info.crcFailures = statistics.crcFailures;
	info.decodeFailures = statistics.decodeFailures;
	info.decodeLastUs = statistics.decodeLastUs;
	info.decodeAverageUs = statistics.decoded == 0 ? 0 : statistics.decodeTotalUs / statistics.decoded;
	info.decodeMaximumUs = statistics.decodeMaximumUs;
	sendPacket(edl::MessageType::INFO, sequence, &info, sizeof(info));
}

void finishPacket() {
	receiver.headerUsed = 0;
	receiver.payloadUsed = 0;
}

void handlePacket() {
	using edl::Error;
	using edl::MessageType;
	const auto type = static_cast<MessageType>(receiver.header.type);
	const uint8_t *payload = compressedBuffer;
	const uint32_t length = receiver.header.payloadLength;
	switch (type) {
		case MessageType::HELLO:
			if (length != 0) sendReply(receiver.header, Error::BAD_LENGTH);
			else sendInfo(receiver.header.sequence);
			break;
		case MessageType::INFO:
			if (length != 0) sendReply(receiver.header, Error::BAD_LENGTH);
			else sendInfo(receiver.header.sequence);
			break;
		case MessageType::UPLOAD_BEGIN: {
			if ((receiver.header.flags & edl::FLAG_CLEAR) != 0) {
				if (length != 0) sendReply(receiver.header, Error::BAD_LENGTH);
				else {
					const bool ok = animationStore.clear();
					resetFallback();
					sendReply(receiver.header, ok ? Error::NONE : Error::FLASH_ERROR);
				}
				break;
			}
			if (length != sizeof(edl::UploadBegin)) {
				sendReply(receiver.header, Error::BAD_LENGTH);
				break;
			}
			edl::UploadBegin beginData{};
			memcpy(&beginData, payload, sizeof(beginData));
			if (!animationStore.beginUpload(beginData)) sendReply(receiver.header, Error::OUT_OF_RANGE);
			else {
				mode = edl::PlayerMode::UPLOAD;
				streamArmed = false;
				liveLocalOriginUs = 0;
				expectedUploadSequence = receiver.header.sequence + 1;
				lastUploadPacketUs = esp_timer_get_time();
				sendReply(receiver.header, Error::NONE);
			}
			break;
		}
		case MessageType::UPLOAD_CHUNK: {
			if (length < 5 || receiver.header.sequence != expectedUploadSequence) {
				sendReply(receiver.header, length < 5 ? Error::BAD_LENGTH : Error::BAD_SEQUENCE, expectedUploadSequence);
				break;
			}
			if (!animationStore.uploadActive()) {
				sendReply(receiver.header, Error::NO_UPLOAD);
				break;
			}
			const uint32_t offset = edl::readU32(payload);
			if (!animationStore.writeUpload(offset, payload + 4, length - 4)) {
				sendReply(receiver.header, Error::FLASH_ERROR, animationStore.expectedUploadOffset());
				break;
			}
			expectedUploadSequence++;
			lastUploadPacketUs = esp_timer_get_time();
			sendReply(receiver.header, Error::NONE, animationStore.expectedUploadOffset());
			break;
		}
		case MessageType::UPLOAD_COMMIT: {
			if (length != 0) {
				sendReply(receiver.header, Error::BAD_LENGTH);
				break;
			}
			if (!animationStore.uploadActive()) {
				sendReply(receiver.header, Error::NO_UPLOAD);
				break;
			}
			if (receiver.header.sequence != expectedUploadSequence) {
				sendReply(receiver.header, Error::BAD_SEQUENCE, expectedUploadSequence);
				break;
			}
			if (!animationStore.verifyUpload()) {
				sendReply(receiver.header, Error::BAD_ANIMATION);
				break;
			}
			bool valid = true;
			for (uint32_t index = 0; index < animationStore.uploadFrameCount() && valid; ++index) {
				edl::FrameIndex entry{};
				valid = animationStore.readUploadFrame(index, entry, compressedBuffer, edl::MAX_JPEG_SIZE) &&
					validateJpeg(compressedBuffer, entry.jpegLength);
			}
			if (!valid || !animationStore.commitUpload()) sendReply(receiver.header, Error::BAD_ANIMATION);
			else {
				resetFallback();
				sendReply(receiver.header, Error::NONE);
			}
			break;
		}
		case MessageType::UPLOAD_ABORT:
			if (length != 0) sendReply(receiver.header, Error::BAD_LENGTH);
			else {
				animationStore.abortUpload();
				resetFallback();
				sendReply(receiver.header, Error::NONE);
			}
			break;
		case MessageType::STREAM_BEGIN:
			if (animationStore.uploadActive()) sendReply(receiver.header, Error::BUSY);
			else if (length != 0) sendReply(receiver.header, Error::BAD_LENGTH);
			else {
				expectedStreamSequence = receiver.header.sequence + 1;
				streamArmed = true;
				liveLocalOriginUs = 0;
				lastLiveFrameUs = esp_timer_get_time();
				sendReply(receiver.header, Error::NONE);
			}
			break;
		case MessageType::RELAY_BEGIN:
			if (length != 0) sendReply(receiver.header, Error::BAD_LENGTH);
			else if (animationStore.uploadActive()) sendReply(receiver.header, Error::BUSY);
			else {
				resetFallback();
				relayActive = true;
				relayBytes = 0;
				relayLastByteUs = esp_timer_get_time();
				sendReply(receiver.header, Error::NONE);
			}
			break;
		case MessageType::STREAM_FRAME:
		case MessageType::STREAM_RAW_FRAME: {
			const bool raw = type == MessageType::STREAM_RAW_FRAME;
			if (!streamArmed) {
				sendReply(receiver.header, Error::BAD_SEQUENCE, expectedStreamSequence);
				break;
			}
			if (length <= sizeof(edl::StreamFramePrefix) ||
				length - sizeof(edl::StreamFramePrefix) > edl::MAX_JPEG_SIZE ||
				(raw && length != sizeof(edl::StreamFramePrefix) + FRAME_BYTES)) {
				sendReply(receiver.header, Error::BAD_LENGTH);
				break;
			}
			if (receiver.header.sequence != expectedStreamSequence) {
				sendReply(receiver.header, Error::BAD_SEQUENCE, expectedStreamSequence);
				break;
			}
			edl::StreamFramePrefix prefix{};
			memcpy(&prefix, payload, sizeof(prefix));
			if (prefix.durationUs == 0 || (liveLocalOriginUs != 0 && prefix.presentationUs < liveRemoteOriginUs)) {
				sendReply(receiver.header, Error::BAD_LENGTH);
				break;
			}
			pendingJpegLength = length - sizeof(prefix);
			memmove(compressedBuffer, payload + sizeof(prefix), pendingJpegLength);
			if (!raw && !validateJpeg(compressedBuffer, pendingJpegLength)) {
				sendReply(receiver.header, Error::BAD_JPEG);
				break;
			}
			const uint64_t now = esp_timer_get_time();
			if (liveLocalOriginUs == 0) {
				liveLocalOriginUs = now;
				liveRemoteOriginUs = prefix.presentationUs;
			}
			pendingPresentationUs = liveLocalOriginUs + (prefix.presentationUs - liveRemoteOriginUs);
			pendingDurationUs = prefix.durationUs;
			lastLiveFrameUs = now;
			expectedStreamSequence++;
			pendingRawFrame = raw;
			receiver.pendingLiveFrame = true;
			return;
		}
		case MessageType::STREAM_END:
			if (length != 0) sendReply(receiver.header, Error::BAD_LENGTH);
			else {
				resetFallback();
				sendReply(receiver.header, Error::NONE);
			}
			break;
		default:
			sendReply(receiver.header, Error::BAD_TYPE);
			break;
	}
	finishPacket();
}

void receiveUsb() {
	if (receiver.pendingLiveFrame) return;
	while (true) {
		if (receiver.headerUsed < edl::PACKET_HEADER_SIZE) {
			const size_t available = static_cast<size_t>(Serial.available());
			if (available == 0) return;
			const size_t received = Serial.read(receiver.headerBytes.data() + receiver.headerUsed,
				std::min(available, edl::PACKET_HEADER_SIZE - receiver.headerUsed));
			receiver.headerUsed += received;
			if (received > 0) lastUsbByteUs = esp_timer_get_time();
			if (receiver.headerUsed < edl::PACKET_HEADER_SIZE) continue;
			memcpy(&receiver.header, receiver.headerBytes.data(), sizeof(receiver.header));
			if (receiver.header.magic != edl::PACKET_MAGIC) {
				memmove(receiver.headerBytes.data(), receiver.headerBytes.data() + 1, edl::PACKET_HEADER_SIZE - 1);
				receiver.headerUsed--;
				continue;
			}
			if (receiver.header.version != edl::PROTOCOL_VERSION || receiver.header.payloadLength > edl::MAX_PACKET_PAYLOAD) {
				sendReply(receiver.header, receiver.header.version != edl::PROTOCOL_VERSION ? edl::Error::BAD_VERSION : edl::Error::BAD_LENGTH);
				finishPacket();
				continue;
			}
		}
		if (receiver.payloadUsed < receiver.header.payloadLength) {
			const size_t available = static_cast<size_t>(Serial.available());
			if (available == 0) return;
			const size_t received = Serial.read(compressedBuffer + receiver.payloadUsed,
				std::min<size_t>(available, receiver.header.payloadLength - receiver.payloadUsed));
			receiver.payloadUsed += received;
			if (received > 0) lastUsbByteUs = esp_timer_get_time();
			if (receiver.payloadUsed < receiver.header.payloadLength) continue;
		}
		uint32_t packetCrc = edl::crc32(receiver.headerBytes.data(), offsetof(edl::PacketHeader, crc32));
		packetCrc = edl::crc32(compressedBuffer, receiver.payloadUsed, packetCrc);
		if (packetCrc != receiver.header.crc32) {
			statistics.crcFailures++;
			sendReply(receiver.header, edl::Error::BAD_CRC);
			finishPacket();
			continue;
		}
		handlePacket();
		if (receiver.pendingLiveFrame || relayActive) return;
	}
}

void receiveRelay() {
	// Read directly into the idle DMA buffer, with no per-frame parsing or copy.
	const size_t available = static_cast<size_t>(Serial.available());
	if (available > 0) {
		const size_t received = Serial.read(frameBuffers[decodeBufferIndex] + relayBytes,
			std::min(available, FRAME_BYTES - relayBytes));
		relayBytes += received;
		if (received > 0) relayLastByteUs = esp_timer_get_time();
		if (relayBytes == FRAME_BYTES) {
			presentFrame(frameBuffers[decodeBufferIndex]);
			decodeBufferIndex ^= 1;
			relayBytes = 0;
			mode = edl::PlayerMode::LIVE;
		}
	} else if (esp_timer_get_time() - relayLastByteUs >= LIVE_TIMEOUT_US) {
		// An idle gap ends relay mode and discards any interrupted partial frame.
		relayActive = false;
		relayBytes = 0;
		finishPacket();
		resetFallback();
	}
}

void playIdle() {
	if (mode != edl::PlayerMode::IDLE || receiver.pendingLiveFrame) return;
	const uint32_t count = edl::readU32(idleVideo);
	const uint32_t duration = edl::readU32(idleVideo + 4);
	if (count == 0 || duration == 0) return;
	const uint64_t frame = (esp_timer_get_time() - idleStartedUs) / duration;
	if (frame == idleFrameNumber) return;
	idleFrameNumber = frame;
	const uint8_t *entry = idleVideo + 8 + (frame % count) * 8;
	const uint32_t offset = edl::readU32(entry);
	const uint32_t length = edl::readU32(entry + 4);
	if (offset <= static_cast<size_t>(idleVideoEnd - idleVideo) &&
		length <= static_cast<size_t>(idleVideoEnd - idleVideo) - offset) {
		decodeJpeg(idleVideo + offset, length);
	}
}

void playPendingLive() {
	if (!receiver.pendingLiveFrame) return;
	const uint64_t now = esp_timer_get_time();
	if (now < pendingPresentationUs) return;
	if (now > pendingPresentationUs + std::max<uint32_t>(pendingDurationUs, 1)) {
		statistics.dropped++;
	} else if (pendingRawFrame) {
		memcpy(frameBuffers[decodeBufferIndex], compressedBuffer, FRAME_BYTES);
		presentFrame(frameBuffers[decodeBufferIndex]);
		decodeBufferIndex ^= 1;
		mode = edl::PlayerMode::LIVE;
	} else if (decodeJpeg(compressedBuffer, pendingJpegLength)) {
		mode = edl::PlayerMode::LIVE;
	}
	sendReply(receiver.header, edl::Error::NONE);
	receiver.pendingLiveFrame = false;
	finishPacket();
}

} // namespace

void setup() {
	Serial0.begin(115200);
	Serial.setRxBufferSize(16384);
	Serial.setTxBufferSize(2048);
	Serial.begin();
	pinMode(PIN_BUTTON_1, INPUT_PULLUP);
	pinMode(PIN_BUTTON_2, INPUT_PULLUP);
	pinMode(PIN_LCD_DC, OUTPUT);
	pinMode(PIN_LCD_RST, OUTPUT);
	pinMode(PIN_LCD_BL, OUTPUT);
	digitalWrite(PIN_LCD_BL, LOW);

	spi_bus_config_t busConfig{};
	busConfig.mosi_io_num = PIN_LCD_MOSI;
	busConfig.miso_io_num = -1;
	busConfig.sclk_io_num = PIN_LCD_SCLK;
	busConfig.quadwp_io_num = -1;
	busConfig.quadhd_io_num = -1;
	busConfig.max_transfer_sz = STRIPE_BYTES;
	ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &busConfig, SPI_DMA_CH_AUTO));
	spi_device_interface_config_t deviceConfig{};
	deviceConfig.clock_speed_hz = LCD_SPI_HZ;
	deviceConfig.mode = 0;
	deviceConfig.spics_io_num = PIN_LCD_CS;
	deviceConfig.queue_size = 1;
	ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST, &deviceConfig, &display));

	compressedBuffer = static_cast<uint8_t *>(heap_caps_malloc(edl::MAX_PACKET_PAYLOAD, MALLOC_CAP_8BIT));
	frameBuffers[0] = static_cast<uint8_t *>(heap_caps_malloc(FRAME_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_8BIT));
	frameBuffers[1] = static_cast<uint8_t *>(heap_caps_malloc(FRAME_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_8BIT));
	if (compressedBuffer == nullptr || frameBuffers[0] == nullptr || frameBuffers[1] == nullptr) abort();
	memset(frameBuffers[0], 0, FRAME_BYTES);
	memset(frameBuffers[1], 0, FRAME_BYTES);

	initDisplay();
	digitalWrite(PIN_LCD_BL, HIGH);
	presentFrame(frameBuffers[0]);
	decodeBufferIndex = 1;
	if (!animationStore.begin()) Serial0.println("Animation partitions unavailable");
	resetFallback();
	Serial0.printf("EDL firmware ready; free heap: %u bytes\n", ESP.getFreeHeap());
}

void loop() {
	pollButtons();
	if (relayActive) {
		receiveRelay();
		delay(1);
		return;
	}
	receiveUsb();
	if (relayActive) return;
	playPendingLive();
	const uint64_t now = esp_timer_get_time();
	if (receiver.headerUsed != 0 && !receiver.pendingLiveFrame && now - lastUsbByteUs >= LIVE_TIMEOUT_US) {
		// Discard an interrupted packet so the next USB session can handshake.
		finishPacket();
	}
	if (streamArmed && now - lastLiveFrameUs >= LIVE_TIMEOUT_US) {
		if (receiver.pendingLiveFrame) {
			sendReply(receiver.header, edl::Error::BUSY);
			receiver.pendingLiveFrame = false;
			finishPacket();
		}
		resetFallback();
	}
	if (animationStore.uploadActive() &&
		(!Serial.isPlugged() || now - lastUploadPacketUs >= 5'000'000)) {
		animationStore.abortUpload();
		resetFallback();
	}
	playIdle();
	delay(1);
}
