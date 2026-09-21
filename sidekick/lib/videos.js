const { createHash } = require('node:crypto');
const { mkdir, readFile, writeFile, rename, rm } = require('node:fs/promises');
const path = require('node:path');
const { execFile } = require('node:child_process');
const { promisify } = require('node:util');
const { FRAME_BYTES, FPS } = require('./display');
const run = promisify(execFile);
const MAX_DOWNLOAD = 64 * 1024 * 1024;
const MAX_FRAMES = 128 * 1024 * 1024;
const digest = bytes => createHash('sha256').update(bytes).digest('hex');

function ffmpegPath() {
    if (process.env.SIDEKICK_FFMPEG) return process.env.SIDEKICK_FFMPEG;
    // electron-builder unpacks the executable outside the asar archive.
    return require('ffmpeg-static').replace('app.asar', 'app.asar.unpacked');
}
async function decodeVideo(filename, signal) {
    const { stdout } = await run(ffmpegPath(), [
        '-v', 'error', '-nostdin', '-protocol_whitelist', 'file,pipe', '-i', filename,
        '-an', '-sn', '-vf', `scale=160:160:force_original_aspect_ratio=increase,crop=160:160,fps=${FPS}`,
        '-f', 'rawvideo', '-pix_fmt', 'rgb565be', 'pipe:1',
    ], { encoding: 'buffer', maxBuffer: MAX_FRAMES, timeout: 60000, signal, windowsHide: true });
    if (!stdout.length || stdout.length % FRAME_BYTES) throw new Error('MP4 did not decode to complete display frames');
    return stdout;
}
class VideoCache {
    constructor({ directory, fetch, decode = decodeVideo }) {
        Object.assign(this, { directory, fetch, decode });
    }
    async get(info, signal) {
        signal?.throwIfAborted();
        if (this.ready?.checksum === info.checksum) return this.ready.frames;
        const operation = AbortSignal.timeout(120000);
        const bounded = signal ? AbortSignal.any([signal, operation]) : operation;
        await mkdir(this.directory, { recursive: true });
        const filename = path.join(this.directory, `${info.checksum}.mp4`);
        let bytes;
        try { bytes = await readFile(filename); }
        catch (error) { if (error.code !== 'ENOENT') throw error; }
        if (!bytes || digest(bytes) !== info.checksum) {
            if (bytes) await rm(filename, { force: true });
            if (!info.downloadUrl) throw new Error(`No download URL for uncached video ${info.video}`);
            const response = await this.fetch(info.downloadUrl, { credentials: 'include', redirect: 'follow', cache: 'no-store', signal: bounded });
            if (!response.ok) throw new Error(`Video download returned HTTP ${response.status}`);
            if (Number(response.headers.get('content-length')) > MAX_DOWNLOAD) {
                await response.body?.cancel();
                throw new Error('Video download exceeds 64 MiB');
            }
            const chunks = [];
            let size = 0;
            const reader = response.body.getReader();
            try {
                while (true) {
                    bounded.throwIfAborted();
                    const { value, done } = await reader.read();
                    if (done) break;
                    size += value.length;
                    if (size > MAX_DOWNLOAD) throw new Error('Video download exceeds 64 MiB');
                    chunks.push(Buffer.from(value));
                }
            } finally { await reader.cancel(); }
            bytes = Buffer.concat(chunks);
            if (digest(bytes) !== info.checksum) throw new Error(`SHA-256 checksum mismatch for ${info.video}`);
            bounded.throwIfAborted();
            const temporary = `${filename}.tmp`;
            try {
                await writeFile(temporary, bytes, { mode: 0o600, signal: bounded });
                await rename(temporary, filename);
            } finally { await rm(temporary, { force: true }); }
        }
        bounded.throwIfAborted();
        const frames = await this.decode(filename, bounded);
        bounded.throwIfAborted();
        this.ready = { checksum: info.checksum, frames };
        return frames;
    }
}
module.exports = { VideoCache, decodeVideo, digest };
