#include <gtest/gtest.h>
#include <chrono>
#include "quark/io/zero_copy_stream.h"

using namespace quark::io;

template <typename Func>
double measure_microseconds(Func f) {
    auto start = std::chrono::high_resolution_clock::now();
    f();
    auto end = std::chrono::high_resolution_clock::now();
    return std::chrono::duration<double, std::micro>(end - start).count();
}

// ---------------------------
// Basic Functionality Tests
// ---------------------------

// write and read 32 bit varint sequentially
TEST(ZeroCopyStream, Varint32ReadWrite) {
    VectorOutputStream vos;
    for (uint32_t i = 0; i < 100; ++i) {
        EXPECT_TRUE(WriteVarint32(&vos, i));
    }

    BufferInputStream bis(vos.buffer().data(), vos.buffer().size());
    for (uint32_t i = 0; i < 100; ++i) {
        uint32_t val;
        EXPECT_TRUE(ReadVarint32(&bis, val));
        EXPECT_EQ(val, i);
    }
}

// write and read 64 bit varint sequentially
TEST(ZeroCopyStream, Varint64ReadWrite) {
    VectorOutputStream vos;
    for (uint64_t i = 0; i < 100; ++i) {
        EXPECT_TRUE(WriteVarint64(&vos, i));
    }

    BufferInputStream bis(vos.buffer().data(), vos.buffer().size());
    for (uint64_t i = 0; i < 100; ++i) {
        uint64_t val;
        EXPECT_TRUE(ReadVarint64(&bis, val));
        EXPECT_EQ(val, i);
    }
}

// write and read fixed 32 bit int sequentially
TEST(ZeroCopyStream, Fixed32ReadWrite) {
    VectorOutputStream vos;
    for (uint32_t i = 0; i < 100; ++i) {
        WriteFixed32(&vos, i);
    }

    BufferInputStream bis(vos.buffer().data(), vos.buffer().size());
    for (uint32_t i = 0; i < 100; ++i) {
        uint32_t val;
        EXPECT_TRUE(ReadFixed32(&bis, val));
        EXPECT_EQ(val, i);
    }
}

// write and read 64 bit int sequentially
TEST(ZeroCopyStream, Fixed64ReadWrite) {
    VectorOutputStream vos;
    for (uint64_t i = 0; i < 100; ++i) {
        WriteFixed64(&vos, i);
    }

    BufferInputStream bis(vos.buffer().data(), vos.buffer().size());
    for (uint64_t i = 0; i < 100; ++i) {
        uint64_t val;
        EXPECT_TRUE(ReadFixed64(&bis, val));
        EXPECT_EQ(val, i);
    }
}

// ---------------------------
// MultiBufferInputStream Tests
// ---------------------------

// test chunking
TEST(ZeroCopyStream, MultiBufferInputStream) {
    std::vector<MultiBufferInputStream::Chunk> chunks = {
        {reinterpret_cast<const uint8_t*>("abc"), 3},
        {reinterpret_cast<const uint8_t*>("defg"), 4},
        {reinterpret_cast<const uint8_t*>("hij"), 3}
    };
    MultiBufferInputStream mb(chunks);
    const uint8_t* block;
    size_t size;
    std::string result;
    while (mb.Next(&block, &size)) {
        result.append(reinterpret_cast<const char*>(block), size);
    }
    EXPECT_EQ(result, "abcdefghij");
}

// ---------------------------
// BackUp & Skip Tests
// ---------------------------
TEST(ZeroCopyStream, BackUpAndSkip) {
    uint8_t data[10] = {0,1,2,3,4,5,6,7,8,9};
    BufferInputStream bis(data, 10);

    const uint8_t* block;
    size_t size;
    EXPECT_TRUE(bis.Next(&block, &size));
    EXPECT_EQ(size, 10u);

    bis.BackUp(3);
    EXPECT_EQ(bis.ByteCount(), 7);
    EXPECT_TRUE(bis.Skip(2));
    EXPECT_EQ(bis.ByteCount(), 9);

    uint8_t buffer[1];
    EXPECT_TRUE(bis.ReadRaw(buffer, 1));
    EXPECT_EQ(buffer[0], 9);
    EXPECT_FALSE(bis.Skip(1));
}

// ---------------------------
// VectorOutputStream Growth Tests
// ---------------------------
TEST(ZeroCopyStream, VectorOutputStreamGrowth) {
    VectorOutputStream vos(4);
    const uint8_t src[10] = {0,1,2,3,4,5,6,7,8,9};
    EXPECT_TRUE(vos.WriteRaw(src, 10));
    EXPECT_EQ(vos.ByteCount(), 10);
    const auto& buf = vos.buffer();
    EXPECT_EQ(buf.size(), static_cast<size_t>(10));

}

// ---------------------------
// Performance Tests
// ---------------------------
TEST(ZeroCopyStream, Varint32PerformanceMicro) {
    const int N = 10000;
    VectorOutputStream vos;
    double write_us = measure_microseconds([&]{
        for (uint32_t i = 0; i < N; ++i) WriteVarint32(&vos, i);
    });
    BufferInputStream bis(vos.buffer().data(), vos.buffer().size());
    double read_us = measure_microseconds([&]{
        for (uint32_t i = 0; i < N; ++i) {
            uint32_t val;
            ReadVarint32(&bis, val);
        }
    });
    std::cout << "Varint32 write: " << write_us << " us, read: " << read_us << " us\n";
}

TEST(ZeroCopyStream, Fixed64PerformanceMicro) {
    const int N = 10000;
    VectorOutputStream vos;
    double write_us = measure_microseconds([&]{
        for (uint64_t i = 0; i < N; ++i) WriteFixed64(&vos, i);
    });
    BufferInputStream bis(vos.buffer().data(), vos.buffer().size());
    double read_us = measure_microseconds([&]{
        for (uint64_t i = 0; i < N; ++i) {
            uint64_t val;
            ReadFixed64(&bis, val);
        }
    });
    std::cout << "Fixed64 write: " << write_us << " us, read: " << read_us << " us\n";
}

// ---------------------------
// Edge Case Tests
// ---------------------------
TEST(ZeroCopyStream, ReadRawPartial) {
    uint8_t data[5] = {1,2,3,4,5};
    BufferInputStream bis(data, 5);
    uint8_t buf[3];
    EXPECT_TRUE(bis.ReadRaw(buf, 3));
    EXPECT_EQ(buf[0], 1);
    EXPECT_EQ(buf[2], 3);
    EXPECT_EQ(bis.ByteCount(), 3);
    uint8_t buf2[3];
    EXPECT_FALSE(bis.ReadRaw(buf2, 3));
}

TEST(ZeroCopyStream, WriteAndBackUpMultipleBlocks) {
    VectorOutputStream vos(2);
    const uint8_t data[5] = {1,2,3,4,5};
    EXPECT_TRUE(vos.WriteRaw(data, 5));
    EXPECT_EQ(vos.ByteCount(), 5);
    vos.BackUp(2);
    EXPECT_EQ(vos.ByteCount(), 3);
    const auto& buf = vos.buffer();
    EXPECT_EQ(buf.size(), static_cast<size_t>(3));

}

TEST(ZeroCopyStream, BackUpTooManyBytes) {
    VectorOutputStream vos;
    const uint8_t data[5] = {1,2,3,4,5};
    EXPECT_TRUE(vos.WriteRaw(data, 5));
    EXPECT_THROW(vos.BackUp(10), std::runtime_error);
}

TEST(ZeroCopyStream, SkipTooManyBytes) {
    uint8_t data[5] = {1,2,3,4,5};
    BufferInputStream bis(data, 5);
    uint8_t block;
    EXPECT_TRUE(bis.ReadRaw(&block, 3));
    EXPECT_FALSE(bis.Skip(10));
    EXPECT_EQ(bis.ByteCount(), 5);
}

TEST(ZeroCopyStream, InterleavedNextReadRaw) {
    std::vector<MultiBufferInputStream::Chunk> chunks = {
        {reinterpret_cast<const uint8_t*>("ab"), 2},
        {reinterpret_cast<const uint8_t*>("cd"), 2}
    };
    MultiBufferInputStream mb(chunks);

    const uint8_t* block;
    size_t size;
    mb.Next(&block, &size);
    uint8_t buf[3];
    EXPECT_FALSE(mb.ReadRaw(buf, 3));
    EXPECT_EQ(mb.ByteCount(), 2);
}

TEST(ZeroCopyStream, MultiBlockBackUp) {
    VectorOutputStream vos(2);
    const uint8_t data[5] = {1,2,3,4,5};
    EXPECT_TRUE(vos.WriteRaw(data, 5));
    EXPECT_THROW(vos.BackUp(10), std::runtime_error);
}



