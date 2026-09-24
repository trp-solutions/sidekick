const { test } = require('node:test');
const assert = require('node:assert/strict');
const { EventEmitter } = require('node:events');
const { Protocol, packet, crc32 } = require('../lib/protocol');
const { writeFrame, FRAME_BYTES } = require('../lib/display');

class Port extends EventEmitter {
    writes = [];
    write(bytes, callback) { this.writes.push(bytes); callback(); }
}

test('streamed pixels and timing are CRC-protected and wait for the matching acknowledgement', async () => {
    const port = new Port();
    const protocol = new Protocol(port);
    const pixels = Buffer.alloc(FRAME_BYTES);
    for (let i = 0; i < pixels.length; i++) pixels[i] = i % 251;
    const done = writeFrame(protocol, pixels, 123456);
    const sent = port.writes[0];
    assert.equal(sent[5], 12);
    assert.equal(sent.readUInt32LE(12), FRAME_BYTES + 12);
    assert.equal(sent.readBigUInt64LE(20), 123456n);
    assert.equal(sent.readUInt32LE(28), 100000);
    assert.deepEqual(sent.subarray(32), pixels);
    assert.equal(sent.readUInt32LE(16), crc32(sent.subarray(20), crc32(sent.subarray(0, 16))));
    await assert.rejects(writeFrame(protocol, pixels, 223456), /already pending/);
    assert.equal(port.writes.length, 1);
    const reply = Buffer.alloc(8);
    reply[0] = 12;
    port.emit('data', packet(10, 99, reply));
    assert.ok(protocol.pending);
    const ack = packet(10, sent.readUInt32LE(8), reply);
    port.emit('data', ack.subarray(0, 7));
    port.emit('data', ack.subarray(7));
    await done;
    assert.equal(protocol.pending, null);
});

test('rejected frame and disconnect fail the pending send instead of advancing', async () => {
    const port = new Port();
    const protocol = new Protocol(port);
    const pixels = Buffer.alloc(FRAME_BYTES);
    const done = writeFrame(protocol, pixels, 0);
    const rejected = assert.rejects(done, /ESP rejected command: 4/);
    const reply = Buffer.alloc(8);
    reply[0] = 12;
    reply.writeUInt16LE(4, 2);
    port.emit('data', packet(11, 1, reply));
    await rejected;
    const disconnected = assert.rejects(writeFrame(protocol, pixels, 100000), /USB disconnected/);
    port.emit('close');
    await disconnected;
    await assert.rejects(writeFrame(protocol, Buffer.alloc(1), 0), /Invalid display frame size/);
});
