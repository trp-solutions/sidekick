const MAGIC = Buffer.from('EDL1');

function crc32(data, seed = 0) {
    let crc = (seed ^ 0xffffffff) >>> 0;
    for (const byte of data) {
        crc ^= byte;
        for (let bit = 0; bit < 8; bit++) crc = (crc >>> 1) ^ ((crc & 1) ? 0xedb88320 : 0);
    }
    return (crc ^ 0xffffffff) >>> 0;
}

function packet(type, sequence, payload = Buffer.alloc(0)) {
    const header = Buffer.alloc(20);
    MAGIC.copy(header);
    header[4] = 1;
    header[5] = type;
    header.writeUInt32LE(sequence, 8);
    header.writeUInt32LE(payload.length, 12);
    header.writeUInt32LE(crc32(payload, crc32(header.subarray(0, 16))), 16);
    return Buffer.concat([header, payload]);
}

class Protocol {
    constructor(port, onButton = () => {}) {
        this.onButton = onButton;
        this.port = port;
        this.sequence = 0;
        this.buffer = Buffer.alloc(0);
        this.pending = null;
        port.on('data', chunk => this.receive(chunk));
        port.on('error', error => this.fail(error));
        port.on('close', () => this.fail(new Error('USB disconnected')));
    }

    fail(error) {
        if (this.pending) this.pending.reject(error);
    }

    receive(chunk) {
        this.buffer = Buffer.concat([this.buffer, chunk]);
        while (this.buffer.length >= 20) {
            if (!this.buffer.subarray(0, 4).equals(MAGIC) || this.buffer[4] !== 1 || this.buffer.readUInt32LE(12) > 65552) {
                this.buffer = this.buffer.subarray(1);
                continue;
            }
            const length = this.buffer.readUInt32LE(12);
            if (this.buffer.length < 20 + length) return;
            const header = this.buffer.subarray(0, 20);
            const body = this.buffer.subarray(20, 20 + length);
            this.buffer = this.buffer.subarray(20 + length);
            if (crc32(body, crc32(header.subarray(0, 16))) !== header.readUInt32LE(16)) {
                this.fail(new Error('USB response CRC mismatch'));
                continue;
            }
            if (header[5] === 14) {
                const sequence = header.readUInt32LE(8);
                if (body.length === 1 && body[0] >= 1 && body[0] <= 8 && sequence !== this.lastButtonSequence) {
                    this.lastButtonSequence = sequence;
                    this.onButton(body[0]);
                }
                continue;
            }
            const pending = this.pending;
            if (!pending || header.readUInt32LE(8) !== pending.sequence) continue;
            if (header[5] === 2 && pending.type === 2 && body.length === 56) pending.resolve(body);
            else if ([10, 11].includes(header[5]) && body.length === 8 && body[0] === pending.type) {
                if (header[5] === 11 || body.readUInt16LE(2)) pending.reject(new Error(`ESP rejected command: ${body.readUInt16LE(2)}`));
                else pending.resolve(body);
            } else pending.reject(new Error('Unexpected ESP response'));
        }
    }

    request(type, timeout = 4000) {
        if (this.pending) return Promise.reject(new Error('A USB command is already pending'));
        const sequence = this.sequence = (this.sequence + 1) >>> 0;
        return new Promise((resolve, reject) => {
            const finish = callback => value => {
                clearTimeout(timer);
                this.pending = null;
                callback(value);
            };
            const timer = setTimeout(() => this.fail(new Error('ESP response timed out')), timeout);
            this.pending = { type, sequence, resolve: finish(resolve), reject: finish(reject) };
            this.port.write(packet(type, sequence), error => { if (error) this.fail(error); });
        });
    }
}

module.exports = { crc32, packet, Protocol };
