#pragma once
#include <cstdint>

namespace edl {
// Events: 1/2 = button 1 press/hold, 3/4 = button 2, 5/6 = both,
// 7/8 = button 1/2 double click. Combined gestures never double click.
class Buttons {
public:
	uint8_t update(uint8_t raw, uint32_t now) {
		for (uint8_t bit = 0; bit < 2; ++bit) {
			const uint8_t mask = 1 << bit;
			if ((raw & mask) != (lastRaw & mask)) changed[bit] = now;
			if (now - changed[bit] >= DEBOUNCE_MS) stable = (stable & ~mask) | (raw & mask);
		}
		lastRaw = raw;
		if (!gesture && stable) {
			gesture = stable;
			started = now;
			if (gesture != 3) {
				const uint8_t index = gesture == 1 ? 0 : 1;
				// Use the physical press onset so debounce cannot turn a
				// timely second click into two single taps.
				secondClick = pending[index] && changed[index] - released[index] < DOUBLE_CLICK_MS;
				if (secondClick) pending[index] = false;
			}
		} else if (gesture && !stable) {
			if (!fired) {
				if (gesture == 3) emit(5);
				else {
					const uint8_t index = gesture == 1 ? 0 : 1;
					if (secondClick) emit(7 + index);
					else { pending[index] = true; released[index] = now; }
				}
			}
			gesture = 0;
			fired = false;
			chordBroken = false;
			secondClick = false;
		} else if (gesture && !fired) {
			if (stable == 3 && gesture != 3) {
				gesture = 3;
				started = now;
			}
			if (gesture == 3 && stable != 3) chordBroken = true;
		}
		if (gesture == 3 && !fired) {
			pending[0] = pending[1] = false;
			secondClick = false;
		}
		// Expired first clicks stay separate from a later press or the other button.
		for (uint8_t index = 0; index < 2; ++index) {
			const bool secondPressDebouncing = (raw & (1 << index)) &&
				changed[index] - released[index] < DOUBLE_CLICK_MS;
			if (pending[index] && now - released[index] >= DOUBLE_CLICK_MS && !secondPressDebouncing) {
				pending[index] = false;
				emit(index == 0 ? 1 : 3);
			}
		}
		if (gesture && !fired && !chordBroken && stable == gesture && raw == gesture && now - started >= 1000) {
			fired = true;
			emit(gesture == 1 ? 2 : gesture == 2 ? 4 : 6);
		}
		if (!eventCount) return 0;
		const uint8_t event = events[0];
		for (uint8_t index = 1; index < eventCount; ++index) events[index - 1] = events[index];
		--eventCount;
		return event;
	}
private:
	static constexpr uint32_t DEBOUNCE_MS = 10;
	static constexpr uint32_t DOUBLE_CLICK_MS = 300;
	void emit(uint8_t event) { if (eventCount < 4) events[eventCount++] = event; }
	uint8_t lastRaw = 0, stable = 0, gesture = 0;
	uint8_t events[4]{}, eventCount = 0;
	uint32_t changed[2]{}, started = 0, released[2]{};
	bool pending[2]{};
	bool fired = false, chordBroken = false, secondClick = false;
};
} // namespace edl
