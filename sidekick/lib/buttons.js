const BUTTON_EVENTS = ['button1Pressed', 'button1Held', 'button2Pressed', 'button2Held', 'bothButtonsPressed', 'bothButtonsHeld', 'button1DoubleClicked', 'button2DoubleClicked'];
const WINDOW_ACTIONS = ['toggleWindow', 'showWindow', 'hideWindow'];

class ButtonActions {
    constructor({ fetch, actions = {}, windowActions, onError = () => {}, onComplete = () => {} }) {
        Object.assign(this, { fetch, actions, windowActions, onError, onComplete });
        this.controller = new AbortController();
        this.queue = Promise.resolve();
        this.pending = 0;
    }
    stop() { this.controller.abort(); }
    trigger(event) {
        const name = BUTTON_EVENTS[event - 1];
        if (!name || this.controller.signal.aborted) return Promise.resolve();
        const action = this.actions[name];
        if (!action) return Promise.resolve();
        if (action.action !== 'request') {
            try { this.windowActions[action.action](); }
            finally { this.onComplete(); }
            return Promise.resolve();
        }
        if (this.pending >= 16) { this.onError('Too many pending button requests'); return Promise.resolve(); }
        this.pending++;
        let started = false;
        this.queue = this.queue.then(async () => {
            if (this.controller.signal.aborted) return;
            started = true;
            const signal = AbortSignal.any([this.controller.signal, AbortSignal.timeout(5000)]);
            const response = await this.fetch(action.url, { method: action.type, credentials: 'include', redirect: 'error', cache: 'no-store', signal });
            await response.body?.cancel();
            if (!response.ok) throw new Error(`Button endpoint returned HTTP ${response.status}`);
        }).catch(error => {
            if (!this.controller.signal.aborted) this.onError(error.message);
        }).finally(() => {
            this.pending--;
            if (started && !this.controller.signal.aborted) this.onComplete();
        });
        return this.queue;
    }
}
module.exports = { BUTTON_EVENTS, WINDOW_ACTIONS, ButtonActions };
