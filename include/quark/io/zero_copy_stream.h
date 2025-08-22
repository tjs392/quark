#pragma once
// quark_zero_copy_stream.h
// Minimal zero-copy-style stream interfaces + practical backends.
// Inspired by protobuf's ZeroCopy{Input,Output}Stream
//
// License: MIT (do as you wish, but please dont use for evil).

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <vector>
#include <string>
#include <stdexcept>
#include <algorithm>
#include <memory>
#include <limits>
#include <span>

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
    virtual bool Next(const uint8_t** block, size_t* size) = 0;

    /**
     * Pushes back 'count' bytes from the last block returned by Next().
     * @param count Number of bytes to back up
     */
    virtual void BackUp(size_t count) = 0;

    /**
     * Skips forward 'count' bytes by consuming blocks.
     * @param count Number of bytes to skip
     * @return false if not enough bytes remain
     */
    virtual bool Skip(size_t count) {
        const uint8_t* ptr;
        size_t n;
        while (count > 0) {
            if (!Next(&ptr, &n)) return false;
            if (n > count) { 
                BackUp(n - count);
                count = 0;
                break;
            }
            count -= n;
        }
        return true;
    }

    bool ReadRaw(void* buffer, size_t size) {
        uint8_t* out = static_cast<uint8_t*>(buffer);
        size_t remaining = size;

        while (remaining > 0) {
            const uint8_t* ptr;
            size_t chunk_size;

            if (!Next(&ptr, &chunk_size)) {
                return false;
            }

            if (chunk_size > remaining) {
                std::memcpy(out, ptr, remaining);
                BackUp(chunk_size - remaining);
                return true;
            } else {
                std::memcpy(out, ptr, chunk_size);
                out += chunk_size;
                remaining -= chunk_size;
            }
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
    virtual bool Next(uint8_t** block, size_t* size) = 0;

    /**
     * Backs up 'count' bytes that were returned by the last Next() but not used.
     * @param count Number of bytes to back up
     */
    virtual void BackUp(size_t count) = 0;

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
    bool WriteRaw(const void* src, size_t size) {
        const uint8_t* ptr = static_cast<const uint8_t*>(src);
        while (size > 0) {
            uint8_t* block;
            size_t  n;
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
        return true;
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
    BufferInputStream(const uint8_t* data, size_t size)
        : data_(data), size_(size), pos_(0), last_returned_(0) {}

    /**
     * @brief Provides a pointer to the next block of contiguous data
     * @param block Output pointer to the start of the next unread block
     * @param size Output number of bytes in the block
     * @return true if a block is available, false if end of buffer is reached
     */
    bool Next(const uint8_t** block, size_t* size) override {
        if (pos_ >= size_) return false;
        *block = data_ + pos_;
        size_t available = size_ - pos_;
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
    void BackUp(size_t count) override {
        if (count > last_returned_)throw std::runtime_error("BackUp out of range");
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
    size_t size_;              // Total size of the buffer
    size_t pos_;               // Current read position in the buffer
    size_t last_returned_;     // Size of the last block returned by Next()
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
/// size_t size;
/// while (stream.Next(&data, &size)) {
///     // process 'size' bytes at 'data'
/// }
class MultiBufferInputStream : public ZeroCopyInputStream {
public:
    /// Represents a single contiguous chunk of memory
    struct Chunk {
        const uint8_t* data;    // pointer to the chunk memory
        size_t size;               // size of the chunk in bytes
    };

    /// Constructs the stream from a vector of chunks
    /// @param chunks a vector of memory chunks to read from
    explicit MultiBufferInputStream(std::vector<Chunk> chunks)
        : chunks_(std::move(chunks)), idx_(0), backed_up_(0), total_(0) {}


    /// Returns the next contiguous block of data and its size
    /// @param block pointer that will be set to the start of the next block
    /// @param size pointer that will be set to the size of the block
    /// @return false if no more data is available
    bool Next(const uint8_t** block, size_t* size) override {
        if (backed_up_ > 0) {
            const auto& c = chunks_[idx_ - 1];
            *block = c.data + c.size - backed_up_;
            *size = backed_up_;
            total_ += *size;
            last_size_ = *size;
            backed_up_ = 0;
            return true;
        }

        if (idx_ >= chunks_.size()) return false;
        const auto& c = chunks_[idx_++];
        *block = c.data;
        *size = c.size;
        total_ += *size;
        last_size_ = *size;
        return true;
    }

    /// Backs up a number of bytes from the last block returned by Next()
    /// @param count number of bytes to back up (0 <= count <= last block size)
    void BackUp(size_t count) override {
        if (count > last_size_) throw std::runtime_error("BackUp out of range");
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
    size_t idx_;                   // index of next chunk to serve
    size_t backed_up_ = 0;         // number of bytes backed up from last chunk
    size_t last_size_ = 0;         // size of last chunk returned
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
    BufferOutputStream(uint8_t* data, size_t size) 
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
    bool Next(uint8_t** block, size_t* size) override {
        if (pos_ >= size_) return false;
        *block = data_ + pos_;
        size_t available = size_ - pos_;
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
    void BackUp(size_t count) override {
        if (count > last_provided_) throw std::runtime_error("BackUp out of range");
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
    size_t size_;              // Total size of the buffer in bytes.
    size_t pos_;               // Current write position in the buffer.
    size_t last_provided_;     // Size of the last block returned by Next().
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
        :   block_size_(std::max<size_t>(64, block_size)), 
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
    bool Next(uint8_t** block, size_t* size) override {
        if (buf_.size() < size_ + block_size_) {
            size_t new_capacity = std::max(buf_.capacity() * 2, size_ + block_size_);
            buf_.reserve(new_capacity);
        }

        *block = buf_.data() + size_;
        *size = block_size_;

        size_ += block_size_;
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
    void BackUp(size_t count) override {
        if (count > last_provided_) throw std::runtime_error("BackUp out of range");
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
    size_t block_size_;            // Preferred size of each allocated block
    size_t size_;                  // Logical size of data written
    size_t last_provided_;         // Bytes provided in last Next() call
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
 *   - 1 -> more bytes follow
 *   - 0 -> last byte of the varint
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
    uint8_t* ptr = tmp;

    while (varint >= 0x80) {
        *ptr++ = static_cast<uint8_t>(varint | 0x80);
        varint >>= 7;
    }
    *ptr++ = static_cast<uint8_t>(varint);

    return out->WriteRaw(tmp, ptr - tmp);
}

/**
 * @brief Writes a 64-bit unsigned integer to a ZeroCopyOutputStream using varint encoding.
 *
 * Varint encoding is a variable-length encoding where:
 * - Each byte uses 7 bits to store data.
 * - The most significant bit (MSB) of each byte is a continuation flag:
 *   - 1 -> more bytes follow
 *   - 0 -> last byte of the varint
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
    uint8_t* ptr = tmp;

    while (varint >= 0x80) {
        *ptr++ = static_cast<uint8_t>(varint | 0x80);
        varint >>= 7;
    }
    *ptr++ = static_cast<uint8_t>(varint);

    return out->WriteRaw(tmp, ptr - tmp);
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
    size_t shift = 0;

    const uint8_t* data;
    size_t size;
    const uint8_t* ptr = nullptr;

    while (shift < 35) {
        if (ptr == nullptr || ptr >= data + size) {
            if (!in->Next(&data, &size)) return false;
            if (size == 0) continue;
            ptr = data;
        }

        uint8_t byte = *ptr++;
        out_val |= static_cast<uint32_t>(byte & 0x7F) << shift;
        shift += 7;

        if ((byte & 0x80) == 0) {
            in->BackUp(data + size - ptr);
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
    size_t shift = 0;

    const uint8_t* data;
    size_t size;
    const uint8_t* ptr = nullptr;

    while (shift < 70) {
        if (ptr == nullptr || ptr >= data + size) {
            if (!in->Next(&data, &size)) return false;
            if (size == 0) continue;
            ptr = data;
        }

        uint8_t byte = *ptr++;
        out_val |= static_cast<uint64_t>(byte & 0x7F) << shift;
        shift += 7;

        if ((byte & 0x80) == 0) {
            in->BackUp(data + size - ptr);
            return true;
        }
    }

    return false;
}

/**
 * @brief Writes a 32-bit unsigned integer in little-endian order to a ZeroCopyOutputStream.
 *
 * Each byte of the integer is written in order from least-significant to most-significant.
 * This ensures cross-platform consistency regardless of host endianness.
 *
 * @param out Pointer to the ZeroCopyOutputStream where the data will be written.
 * @param v The 32-bit unsigned integer value to write.
 * @return true if the write succeeded, false otherwise.
 */
inline bool WriteFixed32(ZeroCopyOutputStream* out, uint32_t v) {
    uint8_t b[4];
    b[0] = static_cast<uint8_t>(v); 
    b[1] = static_cast<uint8_t>(v >> 8);
    b[2] = static_cast<uint8_t>(v >> 16); 
    b[3] = static_cast<uint8_t>(v >> 24);
    return out->WriteRaw(b, 4);
}

/**
 * @brief Writes a 64-bit unsigned integer in little-endian order to a ZeroCopyOutputStream.
 *
 * Each byte of the integer is written in order from least-significant to most-significant.
 * This ensures cross-platform consistency regardless of host endianness.
 *
 * @param out Pointer to the ZeroCopyOutputStream where the data will be written.
 * @param v The 64-bit unsigned integer value to write.
 * @return true if the write succeeded, false otherwise.
 */
inline bool WriteFixed64(ZeroCopyOutputStream* out, uint64_t v) {
    uint8_t b[8];
    b[0] = static_cast<uint8_t>(v); 
    b[1] = static_cast<uint8_t>(v >> 8);
    b[2] = static_cast<uint8_t>(v >> 16); 
    b[3] = static_cast<uint8_t>(v >> 24);
    b[4] = static_cast<uint8_t>(v >> 32); 
    b[5] = static_cast<uint8_t>(v >> 40);
    b[6] = static_cast<uint8_t>(v >> 48); 
    b[7] = static_cast<uint8_t>(v >> 56);
    return out->WriteRaw(b, 8);
}

/**
 * @brief Reads a 32-bit fixed-size unsigned integer from the input stream.
 * 
 * This function reads exactly 4 bytes from the provided ZeroCopyInputStream
 * and reconstructs a little-endian 32-bit unsigned integer.
 * 
 * @param[in,out] in Pointer to the ZeroCopyInputStream to read from.
 * @param[out] v Reference to a uint32_t where the result will be stored.
 * 
 * @return true if 4 bytes were successfully read and decoded.
 * @return false if there were fewer than 4 bytes available in the stream.
 */
inline bool ReadFixed32(ZeroCopyInputStream* in, uint32_t &v) {
    const uint8_t* ptr;
    size_t n;
    if (!in->Next(&ptr, &n) || n < 4) {
        if (n > 0) {
            in->BackUp(n);
        }
        return false;
    }

    v = static_cast<uint32_t>(ptr[0]);
    v |= (static_cast<uint32_t>(ptr[1]) << 8);
    v |=(static_cast<uint32_t>(ptr[2]) << 16);
    v |= (static_cast<uint32_t>(ptr[3]) << 24);

    if (n > 4) {
        in->BackUp(n - 4);
    }
    return true;
}

/**
 * @brief Reads a 64-bit fixed-size unsigned integer from the input stream.
 * 
 * This function reads exactly 8 bytes from the provided ZeroCopyInputStream
 * and reconstructs a little-endian 64-bit unsigned integer.
 * 
 * @param[in,out] in Pointer to the ZeroCopyInputStream to read from.
 * @param[out] v Reference to a uint64_t where the result will be stored.
 * 
 * @return true if 8 bytes were successfully read and decoded.
 * @return false if there were fewer than 8 bytes available in the stream.
 */
inline bool ReadFixed64(ZeroCopyInputStream* in, uint64_t &v) {
    const uint8_t* ptr;
    size_t n;
    if (!in->Next(&ptr, &n) || n < 8) {
        if (n > 0) {
            in->BackUp(n);
        }
        return false;
    }

    v = static_cast<uint64_t>(ptr[0]);
    v |= (static_cast<uint64_t>(ptr[1]) << 8);
    v |=(static_cast<uint64_t>(ptr[2]) << 16);
    v |= (static_cast<uint64_t>(ptr[3]) << 24);
    v |= (static_cast<uint64_t>(ptr[4]) << 32);
    v |=(static_cast<uint64_t>(ptr[5]) << 40);
    v |= (static_cast<uint64_t>(ptr[6]) << 48);
    v |= (static_cast<uint64_t>(ptr[7]) << 56);

    if (n > 8) {
        in->BackUp(n - 8);
    }
    return true;
}

/**
 * @brief Writes a length-delimited byte array to the output stream.
 * 
 * This function first writes the length of the data as a varint (32-bit), 
 * then writes the raw bytes themselves. This is useful for writing strings 
 * or arbitrary binary blobs in a zero-copy, protobuf-style format.
 * 
 * @param out Pointer to the ZeroCopyOutputStream to write to.
 * @param data Pointer to the byte array to write.
 * @param len Length of the byte array.
 * @return true if the length and data were successfully written; false otherwise.
 */
inline bool WriteLengthDelimitedBytes(ZeroCopyOutputStream* out, const uint8_t* data, size_t len) {
    if (!WriteVarint32(out, len)) return false;
    return out->WriteRaw(data, len);
}

/**
 * @brief Reads a length-delimited byte sequence from the input stream.
 * @param in The input stream to read from.
 * @param out Span that will point to the read bytes.
 * @param persistent_buffer Optional buffer to hold data if not contiguous.
 * @return true on success, false on failure.
 */
inline bool ReadLengthDelimitedBytes(ZeroCopyInputStream* in,  std::span<const uint8_t>& out, std::shared_ptr<std::vector<uint8_t>>& persistent_buffer) {
    uint32_t length;
    if (!ReadVarint32(in, length)) return false;

    const uint8_t* ptr;
    size_t n;
    if (!in->Next(&ptr, &n)) return false;

    if (n >= length) {
        out = std::span<const uint8_t>(ptr, length);
        in->BackUp(n - length);
        persistent_buffer.reset();
        return true;
    }

    persistent_buffer = std::make_shared<std::vector<uint8_t>>(length);
    if (!in->ReadRaw(persistent_buffer->data(), length)) return false;

    out = std::span<const uint8_t>(persistent_buffer->data(), length);
    return true;
}

// ===========================
// Serialization Apis
// ===========================

/**
 * @brief Enumeration of supported serialization types.
 */
enum class Type : uint8_t { 
    INT32 = 1, 
    FLOAT32 = 2, 
    STRING = 3
};

/**
 * @brief Serializes a 32-bit integer to the output stream.
 * @param out The output stream to write to.
 * @param value The integer value to serialize.
 * @return true on success, false on failure.
 */
inline bool SerializeInt32(ZeroCopyOutputStream* out, int32_t value) {
    uint8_t tag = static_cast<uint8_t>(Type::INT32);
    
    if (!out->WriteRaw(&tag, 1)) return false;
    return WriteFixed32(out, static_cast<uint32_t>(value));
}

/**
 * @brief Deserializes a 32-bit integer from the input stream.
 * @param in The input stream to read from.
 * @param value The integer variable to store the deserialized value.
 * @return true on success, false on failure.
 */
inline bool DeserializeInt32(ZeroCopyInputStream* in, int32_t& value) {
    uint8_t tag;
    if (!in->ReadRaw(&tag, 1)) return false;
    if (tag != static_cast<uint8_t>(Type::INT32)) return false;

    uint32_t tmp;
    if (!ReadFixed32(in, tmp)) return false;
    value = static_cast<int32_t>(tmp);
    return true;
}

/**
 * @brief Serializes a 32-bit float to the output stream.
 * @param out The output stream to write to.
 * @param value The float value to serialize.
 * @return true on success, false on failure.
 */
inline bool SerializeFloat32(ZeroCopyOutputStream* out, float value) {
    uint8_t tag = static_cast<uint8_t>(Type::FLOAT32);
    if (!out->WriteRaw(&tag, 1)) return false;

    uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    return WriteFixed32(out, bits);
}

/**
 * @brief Deserializes a 32-bit float from the input stream.
 * @param in The input stream to read from.
 * @param value The float variable to store the deserialized value.
 * @return true on success, false on failure.
 */
inline bool DeserializeFloat32(ZeroCopyInputStream* in, float& value) {
    uint8_t tag;
    if (!in->ReadRaw(&tag, 1)) return false;
    if (tag != static_cast<uint8_t>(Type::FLOAT32)) return false;

    uint32_t bits;
    if (!ReadFixed32(in, bits)) return false;

    std::memcpy(&value, &bits, sizeof(value));
    return true;
}

/**
 * @brief Serializes a string to the output stream.
 * @param out The output stream to write to.
 * @param str The string to serialize.
 * @return true on success, false on failure.
 */
inline bool SerializeString(ZeroCopyOutputStream* out, const std::string& str) {
    uint8_t tag = static_cast<uint8_t>(Type::STRING);
    if (!out->WriteRaw(&tag, 1)) return false;

    if (!WriteVarint32(out, static_cast<uint32_t>(str.size()))) return false;

    return out->WriteRaw(reinterpret_cast<const uint8_t*>(str.data()), str.size());
}

/**
 * @brief Deserializes a string from the input stream.
 * @param in The input stream to read from.
 * @param str The string to store the deserialized data (if copy is needed).
 * @param persistent_buffer Optional buffer to hold data if not contiguous.
 * @param str_view String view pointing to the deserialized data.
 * @return true on success, false on failure.
 */
inline bool DeserializeString(
    ZeroCopyInputStream* in,
    std::string& str,
    std::shared_ptr<std::vector<uint8_t>>& persistent_buffer,
    std::string_view& str_view)
{
    std::span<const uint8_t> bytes;

    if (!ReadLengthDelimitedBytes(in, bytes, persistent_buffer)) {
        return false;
    }

    if (persistent_buffer) {
        str.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        str_view = str;
    } else {
        str_view = std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    }

    return true;
}


}}