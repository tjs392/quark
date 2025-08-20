#pragma once
// quark_zero_copy_stream.h
// Minimal zero-copy-style stream interfaces + practical backends.
// Inspired by protobuf's ZeroCopy{Input,Output}Stream
//
// License: MIT (do as you wish).

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <vector>
#include <string>
#include <stdexcept>
#include <algorithm>
#include <memory>
#include <limits>

#if defined(__unix__) || defined(__APPLE__)
    #define QUARK_POSIX 1
    #include <unistd.h>
    #include <sys/mman.h>
    #include <sys/stat.h>
    #include <fcntl.h>
#else
    #define QUARK_POSIX 0
#endif

namespace quark {
namespace io{

// ============================
// Interfaces (Zero Copy)
// =============================

/// Zero-copy input stream interface
/// Provides methods to read contiguous blocks of memory without copying
class ZeroCopyInputStream {
public:
    /// Destructor
    virtual ~ZeroCopyInputStream() = default;

    /**
     * Provides a pointer to the next contiguous block of data and its size.
     * @param block Pointer that will point to the start of the block
     * @param size Pointer that will hold the size of the block
     * @return false if end-of-stream or failure
     */
    virtual bool Next(const uint8_t** block, int* size) = 0;

    /**
     * Pushes back 'count' bytes from the last block returned by Next().
     * @param count Number of bytes to back up
     */
    virtual void BackUp(int count) = 0;

    /**
     * Skips forward 'count' bytes by consuming blocks.
     * @param count Number of bytes to skip
     * @return false if not enough bytes remain
     */
    virtual bool Skip(int64_t count) {
        const uint8_t* ptr;
        int n;
        while (count > 0) {
            if (!Next(&ptr, &n)) return false;
            if (n > count) { 
                BackUp(n - static_cast<int>(count));
                count = 0;
                break;
            }
            count -= n;
        }
        return true;
    }

    /**
     * Returns the total number of bytes returned to the caller so far.
     * Excludes bytes that were backed up.
     * @return total byte count
     */
    virtual int64_t ByteCount() const = 0;
};

/// Zero-copy output stream interface.
/// Provides methods to write contiguous blocks of memory without copying.
class ZeroCopyOutputStream {
public:
    /// Destructor
    virtual ~ZeroCopyOutputStream() = default;

    /**
     * Provides a pointer to a writable block of at least 'size' bytes.
     * @param block Pointer that will point to the start of the block
     * @param size Pointer to the requested block size (may be modified by implementation)
     * @return false if no more space can be provided
     */
    virtual bool Next(uint8_t** block, int* size) = 0;

    /**
     * Backs up 'count' bytes that were returned by the last Next() but not used.
     * @param count Number of bytes to back up
     */
    virtual void BackUp(int count) = 0;

    /**
     * Flushes buffered data to the sink if applicable.
     * @return false on error
     */
    virtual bool Flush() { return true; }

    /**
     * Writes raw bytes using Next() and BackUp().
     * @param src Pointer to the bytes to write
     * @param size Number of bytes to write
     * @return false if writing failed
     */
    bool WriteRaw(const void* src, int size) {
        const uint8_t* ptr = static_cast<const uint8_t*>(src);
        while (size > 0) {
            uint8_t* block;
            int n;
            if (!Next(&block, &n)) return false;
            if (n > size) {
                std::memcpy(block, ptr, size);
                BackUp(n - size);
                return true;
            }
            std::memcpy(block, ptr, n);
            ptr += n;
            size -= n;
        }
    }

    /**
     * Returns the total number of bytes made visible to the caller.
     * Excludes bytes that were backed up.
     * @return total byte count
     */
    virtual int64_t ByteCount() const = 0;
};

// ========================
// Input APIS
// ========================

/**
 * @class BufferInputStream
 * @brief Provides zero-copy read access to a single contiguous memory buffer.
 *
 * Useful for deserializing in-memory block without copying.
 */
class BufferInputStream : public ZeroCopyInputStream {
public:
    /**
     * @brief Construct a new BufferInputStream
     * @param data Pointer to the start of the buffer to read
     * @param size Total size of the buffer in bytes
     */
    BufferInputStream(const uint8_t* data, int size)
        : data_(data), size_(size), pos_(0), last_returned_(0) {}

    /**
     * @brief Provides a pointer to the next block of contiguous data
     * @param block Output pointer to the start of the next unread block
     * @param size Output number of bytes in the block
     * @return true if a block is available, false if end of buffer is reached
     */
    bool Next(const uint8_t** block, int* size) override {
        if (pos_ >= size_) return false;
        *block = data_ + pos_;
        int available = size_ - pos_;
        *size = available;
        pos_ += available;
        last_returned_ = available;
        return true;
    }

    /**
     * @brief Pushes back 'count' bytes from the last block returned by Next()
     * @param count Number of bytes to back up; must be <= size of last block returned
     * @throw std::runtime_error if count is invalid
     */
    void BackUp(int count) override {
        if (count < 0 || count > last_returned_)throw std::runtime_error("BackUp out of range");
        pos_ -= count;
        last_returned_ -= count;
    }

    /**
     * @brief Returns the total number of bytes returned to the caller so far
     *        (excluding any backed-up bytes)
     * @return int64_t Number of bytes read
     */
    int64_t ByteCount() const override { return pos_; }

private:
    const uint8_t* data_;   ///< Pointer to the start of the buffer
    int size_;              ///< Total size of the buffer
    int pos_;               ///< Current read position in the buffer
    int last_returned_;     ///< Size of the last block returned by Next()
};

/// MultiBufferInputStream provides zero-copy access to multiple contiguous
/// memory regions (chunks), similar to iovec-style buffers. Useful when
/// data is split across several blocks and you want to read them sequentially
/// without copying.
///
/// Example usage:
/// std::vector<MultiBufferInputStream::Chunk> chunks = { ... };
/// MultiBufferInputStream stream(chunks);
/// const uint8_t* data;
/// int size;
/// while (stream.Next(&data, &size)) {
///     // process 'size' bytes at 'data'
/// }
class MultiBufferInputStream : public ZeroCopyInputStream {
public:
    /// Represents a single contiguous chunk of memory
    struct Chunk {
        const uint8_t* data;    ///< pointer to the chunk memory
        int size;               ///< size of the chunk in bytes
    };

    /// Constructs the stream from a vector of chunks
    /// @param chunks a vector of memory chunks to read from
    explicit MultiBufferInputStream(std::vector<Chunk> chunks)
        : chunks_(std::move(chunks)), idx_(0), backed_up_(0), total_(0) {}


    /// Returns the next contiguous block of data and its size
    /// @param block pointer that will be set to the start of the next block
    /// @param size pointer that will be set to the size of the block
    /// @return false if no more data is available
    bool Next(const uint8_t** block, int* size) override {
        if (backed_up_ > 0) {
            const auto& c = chunks_[idx_ - 1];
            *block = c.data + c.size - backed_up_;
            *size = backed_up_;
            total_ += *size;
            last_size_ = *size;
            backed_up_ = 0;
            return true;
        }

        if (idx_ >= static_cast<int>(chunks_.size())) return false;
        const auto& c = chunks_[idx_++];
        *block = c.data;
        *size = c.size;
        total_ += *size;
        last_size_ = *size;
        return true;
    }

    /// Backs up a number of bytes from the last block returned by Next()
    /// @param count number of bytes to back up (0 <= count <= last block size)
    void BackUp(int count) override {
        if (count < 0 || count > last_size_) throw std::runtime_error("BackUp out of range");
        total_ -= count;
        backed_up_ = count;

        if (backed_up_ == last_size_) {
            if (idx_ > 0) idx_--;
        }
        last_size_ -= count;
    }

    /// Returns the total number of bytes returned to the caller so far
    /// (minus any backed-up bytes)
    int64_t ByteCount() const override { return total_; }

private:
    std::vector<Chunk> chunks_; ///< underlying memory chunks
    int idx_;                   ///< index of next chunk to serve
    int backed_up_ = 0;         ///< number of bytes backed up from last chunk
    int last_size_ = 0;         ///< size of last chunk returned
    int64_t total_;             ///< total bytes returned so far
};

// TODO: Implement MmapInputStream usage and tests
// Description:
// This class provides a true zero-copy file input stream using POSIX mmap.
// It maps an entire file into memory, allowing Next() to return direct pointers
// into the file without any intermediate copying. This can be very useful for
// low-latency or high-throughput applications that need to process large files
// efficiently. 
//
// Next steps:
// - Write unit tests that open a file and read chunks using MmapInputStream.
// - Handle edge cases such as empty files and small files.
// - Benchmark against BufferInputStream to see latency improvements.
// - Ensure compatibility with WSL2 and native Linux.

// ==================
// Output APIs
// ===================

/**
 * @class FixedArrayOutputStream
 * @brief Zero-copy output stream that writes into a fixed, caller-provided buffer.
 *
 * This class allows writing contiguous blocks of data directly into a preallocated
 * memory buffer without allocating additional memory. It is useful for
 * low-latency or high-performance applications where memory copies should be minimized.
 */
class BufferOutputStream : public ZeroCopyOutputStream {
public:

    /**
     * @brief Constructs a BufferOutputStream.
     * @param data Pointer to the start of the buffer to write into.
     * @param size Total size of the buffer in bytes.
     */
    BufferOutputStream(uint8_t* data, int size) 
        : data_(data), size_(size), pos_(0), last_provided_(0) {}

    /**
     * @brief Provides the next writable block in the buffer.
     * 
     * Sets `block` to point to the next available memory region in the buffer
     * and sets `size` to the number of bytes available in that block. Updates
     * internal bookkeeping to track written bytes.
     * 
     * @param block Output pointer to the start of the next writable block.
     * @param size Output pointer to the size of the writable block.
     * @return true if a block is available, false if the end of the buffer is reached.
     */
    bool Next(uint8_t** block, int* size) override {
        if (pos_ >= size_) return false;
        *block = data_ + pos_;
        int available = size_ - pos_;
        *size = available;
        pos_ += available; 
        last_provided_ = available; 
        total_ += available;
        return true;
    }

    /**
     * @brief Backs up a number of bytes from the last block returned by Next().
     * 
     * This allows the caller to "unclaim" bytes that were returned by Next()
     * but not actually used. Updates internal bookkeeping accordingly.
     * 
     * @param count Number of bytes to back up. Must be <= last block size.
     * @throws std::runtime_error if count is invalid.
     */
    void BackUp(int count) override {
        if (count < 0 || count > last_provided_) throw std::runtime_error("BackUp out of range");
        pos_ -= count;
        last_provided_ -= count;
        total_ -= count;
    }

    /**
     * @brief Returns the total number of bytes written to the buffer so far.
     * 
     * Does not include any bytes that were backed up.
     * 
     * @return Total number of bytes written.
     */
    int64_t ByteCount() const override {
        return pos_;
    }

    private:
    uint8_t* data_;         ///< Pointer to the start of the buffer.
    int size_;              ///< Total size of the buffer in bytes.
    int pos_;               ///< Current write position in the buffer.
    int last_provided_;     ///< Size of the last block returned by Next().
    int64_t total_;         ///< Total bytes written so far.
};

class VectorOutputStream : ZeroCopyOutputStream {
public:
    explicit VectorOutputStream(size_t chunk_hint = 8192)
        :   chunk_hint_(static_cast<int>(std::max<size_t>(64, chunk_hint))), 
            size_(0), 
            last_provided_(0) {
        buf_.reserve(chunk_hint_);
    }

    bool Next(uint8_t** block, int* size) override {
        if (static_cast<int>(buf_.capacity() - buf_.size()) < chunk_hint_) {
            buf_.reserve(buf_.capacity() + static_cast<size_t>(chunk_hint_));
        }

        size_t grow = static_cast<size_t>(chunk_hint_)
    }

private:
    std::vector<uint8_t> buf_;
    int chunk_hint_;
    int size_;
    int last_provided_;
    int64_t total_ = 0;
}





}
}