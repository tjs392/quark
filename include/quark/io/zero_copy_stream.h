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
    const uint8_t* data_;   // Pointer to the start of the buffer
    int size_;              // Total size of the buffer
    int pos_;               // Current read position in the buffer
    int last_returned_;     // Size of the last block returned by Next()
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
        const uint8_t* data;    // pointer to the chunk memory
        int size;               // size of the chunk in bytes
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
    std::vector<Chunk> chunks_; // underlying memory chunks
    int idx_;                   // index of next chunk to serve
    int backed_up_ = 0;         // number of bytes backed up from last chunk
    int last_size_ = 0;         // size of last chunk returned
    int64_t total_;             // total bytes returned so far
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
    uint8_t* data_;         // Pointer to the start of the buffer.
    int size_;              // Total size of the buffer in bytes.
    int pos_;               // Current write position in the buffer.
    int last_provided_;     // Size of the last block returned by Next().
    int64_t total_;         // Total bytes written so far.
};

/**
 * @class VectorOutputStream
 * @brief A ZeroCopyOutputStream implementation backed by std::vector<uint8_t>.
 *
 * Provides zero-copy semantics: callers can write directly into the vector's
 * underlying memory without intermediate buffers or extra copies. This makes it
 * useful for serialization frameworks that expect ZeroCopyOutputStream.
 */
class VectorOutputStream : ZeroCopyOutputStream {
public:
    /**
     * @brief Construct a new VectorOutputStream.
     * 
     * @param block_size Size of each memory block handed out by Next().
     *                   Default is 8192 bytes, minimum enforced is 64.
     */
    explicit VectorOutputStream(size_t block_size = 8192)
        :   block_size_(static_cast<int>(std::max<size_t>(64, block_size))), 
            size_(0), 
            last_provided_(0) {
        buf_.reserve(block_size_);
    }

    /**
     * @brief Provide the caller with a fresh writable block of memory.
     * 
     * @param block Pointer to the start of the writable memory (output).
     * @param size  Size of the writable block in bytes (output).
     * @return true if a block was successfully allocated.
     */
    bool Next(uint8_t** block, int* size) override {
        if (static_cast<int>(buf_.capacity() - buf_.size()) < block_size_) {
            buf_.reserve(buf_.capacity() + static_cast<size_t>(block_size_));
        }

        size_t grow = static_cast<size_t>(block_size_);
        buf_.resize(buf_.size() + grow);
        *block = buf_.data() + size_;
        *size = block_size_;
        last_provided_ = block_size_;
        total_ += block_size_;
        return true;
    }

    /**
     * @brief Return unused bytes from the most recent Next() call.
     *
     * @param count Number of bytes to back up (must be between 0 and last_provided_).
     * @throws std::runtime_error if count is out of range.
     */
    void BackUp(int count) override {
        if (count < 0 || count > last_provided_) throw std::runtime_error("BackUp out of range");
        size_ -= count;
        total_ -= count;
        last_provided_ -= count;
        buf_.resize(size_);
    }

    /**
     * @brief Flush the stream.
     * 
     * For VectorOutputStream, this is a no-op since data is already in memory.
     * @return Always returns true.
     */
    bool Flush() override { 
        return true; 
    }

    /**
     * @brief Get the total number of bytes written so far.
     * 
     * @return int64_t Total number of bytes.
     */
    int64_t ByteCount() const override {
        return size_;
    }

    /**
     * @brief Access the underlying buffer (read-only).
     * 
     * @return const std::vector<uint8_t>& Reference to the buffer.
     */
    const std::vector<uint8_t>& buffer() const { return buf_; }

    /**
     * @brief Access the underlying buffer (mutable).
     * 
     * @return std::vector<uint8_t>& Reference to the buffer.
     */
    std::vector<uint8_t>& buffer() { return buf_; }

private:
    std::vector<uint8_t> buf_;  // Underlying storage buffer
    int block_size_;            // Preferred size of each allocated block
    int size_;                  // Logical size of data written
    int last_provided_;         // Bytes provided in last Next() call
    int64_t total_ = 0;         // Total bytes ever provided
};

// ================================
// Read and Write APIs for Zero Copy Streams
// =====================================

static constexpr int kMaxVarint32Bytes = 5;
static constexpr int kMaxVarint64Bytes = 10;

/**
 * @brief Writes a 32-bit unsigned integer to a ZeroCopyOutputStream using varint encoding.
 *
 * Varint encoding is a variable-length encoding where:
 * - Each byte uses 7 bits to store data.
 * - The most significant bit (MSB) of each byte is a continuation flag:
 *   - 1 → more bytes follow
 *   - 0 → last byte of the varint
 *
 * This encoding is space-efficient for small numbers: values less than 128
 * fit in a single byte.
 *
 * @param out Pointer to the ZeroCopyOutputStream where the varint will be written.
 * @param varint The 32-bit unsigned integer to encode and write.
 * @return true if the write to the output stream succeeded, false otherwise.
 *
 * @note The maximum number of bytes used for a 32-bit varint is kMaxVarint32Bytes (5 bytes).
 */
inline bool WriteVarint32(ZeroCopyOutputStream* out, uint32_t varint) {
    uint8_t tmp[kMaxVarint32Bytes];
    int n = 0;
    while (varint >= 0x80) {
        tmp[n++] = static_cast<uint8_t>(varint | 0x80);
        varint >>= 7;
    }
    tmp[n++] = static_cast<uint8_t>(varint);
    return out->WriteRaw(tmp, n);
}

/**
 * @brief Writes a 64-bit unsigned integer to a ZeroCopyOutputStream using varint encoding.
 *
 * Varint encoding is a variable-length encoding where:
 * - Each byte uses 7 bits to store data.
 * - The most significant bit (MSB) of each byte is a continuation flag:
 *   - 1 → more bytes follow
 *   - 0 → last byte of the varint
 *
 * This encoding is space-efficient for small numbers: values less than 128
 * fit in a single byte. Larger values may take multiple bytes, up to
 * kMaxVarint64Bytes (10 bytes for a full 64-bit integer).
 *
 * @param out Pointer to the ZeroCopyOutputStream where the varint will be written.
 * @param varint The 64-bit unsigned integer to encode and write.
 * @return true if the write to the output stream succeeded, false otherwise.
 *
 * @note The maximum number of bytes used for a 64-bit varint is kMaxVarint64Bytes (10 bytes).
 */
inline bool WriteVarint64(ZeroCopyOutputStream* out, uint64_t varint) {
    uint8_t tmp[kMaxVarint64Bytes];
    int n = 0;
    while (varint > 0x80) {
        tmp[n++] = static_cast<uint8_t>(varint | 0x80);
        varint >>= 7;
    }
    tmp[n++] = static_cast<uint8_t>(varint);
    return out->WriteRaw(tmp, n);
}

/**
 * @brief Reads a 32-bit unsigned integer from a ZeroCopyInputStream using varint encoding.
 *
 * Varint encoding stores integers in a variable number of bytes. Each byte
 * contributes 7 bits to the result, and the most significant bit is a
 * continuation flag: 1 if more bytes follow, 0 if this is the last byte.
 *
 * This function reads directly from the current chunk provided by the stream,
 * minimizing calls to Next() and BackUp(). It handles small and multi-byte
 * varints efficiently.
 *
 * @param[in] in Pointer to a ZeroCopyInputStream to read from.
 * @param[out] out_val Reference to a uint32_t where the decoded value will be stored.
 *                     Will be set to 0 if reading fails.
 * @return true if a varint was successfully read; false if the stream ended
 *         before a complete varint could be read.
 */
inline bool ReadVarint32(ZeroCopyInputStream* in, uint32_t& out_val) {
    out_val = 0;
    int shift = 0;

    const uint8_t* data;
    int size;
    const uint8_t* ptr = nullptr;

    while (shift < 35) {
        if (ptr == nullptr || ptr >= data + size) {
            if (!in->Next(&data, &size)) return false;
            if (size <= 0) continue;
            ptr = data;
        }

        uint8_t byte = *ptr++;
        out_val |= static_cast<uint32_t>(byte & 0x7F) << shift;
        shift += 7;

        if ((byte & 0x80) == 0) {
            in->BackUp(static_cast<int>(data + size - ptr));
            return true;
        }
    }

    return false;
}

/**
 * @brief Reads a 64-bit unsigned integer from a ZeroCopyInputStream using varint encoding.
 *
 * Varint encoding stores integers in a variable number of bytes. Each byte
 * contributes 7 bits to the result, and the most significant bit is a
 * continuation flag: 1 if more bytes follow, 0 if this is the last byte.
 *
 * This function reads directly from the current chunk provided by the stream,
 * minimizing calls to Next() and BackUp(). It efficiently handles both
 * small and multi-byte varints, supporting full 64-bit integers.
 *
 * @param[in] in Pointer to a ZeroCopyInputStream to read from.
 * @param[out] out_val Reference to a uint64_t where the decoded value will be stored.
 *                     Will be set to 0 if reading fails.
 * @return true if a varint was successfully read; false if the stream ended
 *         before a complete varint could be read.
 */
inline bool ReadVarint64(ZeroCopyInputStream* in, uint64_t& out_val) {
    out_val = 0;
    int shift = 0;

    const uint8_t* data;
    int size;
    const uint8_t* ptr = nullptr;

    while (shift < 63) {
        if (ptr == nullptr || ptr >= data + size) {
            if (!in->Next(&data, &size)) return false;
            if (size <= 0) continue;
            ptr = data;
        }

        uint8_t byte = *ptr++;
        out_val |= static_cast<uint64_t>(byte & 0x7F) << shift;
        shift += 7;

        if ((byte & 0x80) == 0) {
            in->BackUp(static_cast<int>(data + size - ptr));
            return true;
        }
    }

    return false;
}



}} 