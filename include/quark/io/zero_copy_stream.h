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
     * @param data Pointer that will point to the start of the block
     * @param size Pointer that will hold the size of the block
     * @return false if end-of-stream or failure
     */
    virtual bool Next(const uint8_t** data, int* size) = 0;

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
     * @param data Pointer that will point to the start of the block
     * @param size Pointer to the requested block size (may be modified by implementation)
     * @return false if no more space can be provided
     */
    virtual bool Next(uint8_t** data, int* size) = 0;

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
            uint8_t* out;
            int n;
            if (!Next(&out, &n)) return false;
            if (n > size) {
                std::memcpy(out, ptr, size);
                BackUp(n - size);
                return true;
            }
            std::memcpy(out, ptr, n);
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
 * Useful for deserializing in-memory data without copying.
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
     * @param data Output pointer to the start of the next unread block
     * @param size Output number of bytes in the block
     * @return true if a block is available, false if end of buffer is reached
     */
    bool Next(const uint8_t** data, int* size) override {
        if (pos_ >= size_) return false;
        *data = data_ + pos_;
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
    /// @param data pointer that will be set to the start of the next block
    /// @param size pointer that will be set to the size of the block
    /// @return false if no more data is available
    bool Next(const uint8_t** data, int* size) override {
        if (backed_up_ > 0) {
            const auto& c = chunks_[idx_ - 1];
            *data = c.data + c.size - backed_up_;
            *size = backed_up_;
            total_ += *size;
            last_size_ = *size;
            backed_up_ = 0;
            return true;
        }

        if (idx_ >= static_cast<int>(chunks_.size())) return false;
        const auto& c = chunks_[idx_++];
        *data = c.data;
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



}
}