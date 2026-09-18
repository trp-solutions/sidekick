// A missed native readiness notification must not leave a nonblocking USB read
// or write waiting forever. Retry outstanding waits periodically; the binding
// still performs the actual syscall and handles EAGAIN/backpressure normally.
function retrySerialReadiness(port) {
    const poller = port.port?.poller;
    if (!poller || poller.retryPendingEvents) return;
    const retry = setInterval(() => {
        if (!port.isOpen) return;
        for (const event of ['readable', 'writable']) {
            if (poller.listenerCount(event)) poller.emit(event, null);
        }
    }, 100);
    retry.unref();
    const cleanup = () => clearInterval(retry);
    port.once('close', cleanup);
    for (const name of ['stop', 'destroy']) {
        const original = poller[name];
        if (original) poller[name] = function(...args) { cleanup(); return original.apply(this, args); };
    }
    poller.retryPendingEvents = true;
}

module.exports = { retrySerialReadiness };
