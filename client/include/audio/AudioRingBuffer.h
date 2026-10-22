#pragma once

// Single-producer, single-consumer frame queue between the network thread and
// the audio thread.
//
// One producer (the server connection) and one consumer (the render callback),
// and nothing else may touch either side. That is what makes the whole thing
// work without a lock: each index is written by exactly one thread and only
// read by the other.
//
// Everything is allocated once, in configure(); push() and pop() never
// allocate, never lock and never block. A full buffer makes push() short, an
// empty one makes pop() short -- the caller decides what that means, because
// the audio thread cannot wait for anything.

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace multiroom {

class AudioRingBuffer
{
public:
    AudioRingBuffer();

    AudioRingBuffer(const AudioRingBuffer&) = delete;
    AudioRingBuffer& operator=(const AudioRingBuffer&) = delete;

    // Control path only: this is the one call that allocates, and it must not
    // run while either side is active. The capacity is rounded up to a power
    // of two, so the index arithmetic is a mask rather than a modulo.
    bool configure(uint32_t bytesPerFrame, uint32_t capacityFrames, std::string& error);

    // What configure() would allocate for the same arguments, rounding
    // included, so a caller can check a size against a memory budget before
    // asking for it. Zero where configure() would refuse.
    static uint64_t storageBytes(uint32_t bytesPerFrame, uint32_t capacityFrames);

    bool isConfigured() const { return capacityFrames_ != 0; }
    uint32_t capacityFrames() const { return capacityFrames_; }
    uint32_t bytesPerFrame() const { return bytesPerFrame_; }

    // Producer side. Returns how many frames were taken, which is short when
    // the buffer is full and zero when it is unconfigured.
    uint32_t push(const void* frames, uint32_t frameCount);
    uint32_t writableFrames() const;

    // Consumer side. Returns how many frames were delivered; a short result is
    // an underrun and the caller's problem, never a wait.
    uint32_t pop(void* frames, uint32_t frameCount);
    uint32_t readableFrames() const;

    // Consumer side, and the reason it is not a pop() into a bin: frames
    // whose moment on the media timeline has passed are dropped rather than
    // played late, and dropping them has to cost nothing in the render
    // callback. Returns how many were dropped, which is short when the buffer
    // holds fewer.
    uint32_t discard(uint32_t frameCount);

private:
    // Free-running counters, masked only when indexing. The difference between
    // them is the fill level and stays correct across the 32-bit wrap, which is
    // why they are not wrapped indices -- and why they are 32 bits and not 64:
    // std::atomic<uint64_t> is not lock-free on ARMv6.
    std::atomic<uint32_t> writeIndex_;
    std::atomic<uint32_t> readIndex_;

    std::vector<unsigned char> storage_;
    uint32_t capacityFrames_ = 0; // power of two, 0 until configured
    uint32_t mask_ = 0;
    uint32_t bytesPerFrame_ = 0;
};

} // namespace multiroom
