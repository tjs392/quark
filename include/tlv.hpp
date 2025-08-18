#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <stdexcept>

namespace quark {
    enum class Type : uint8_t {
        INT32 = 1,
        FLOAT32 = 2,
        STRING = 3
    };

    // protobuf-like varint encoding
    // varints store integers in a variable number of bytes
    // each byte will encoe 7 bits of the integer
    // the most significant bit of each byte will be the continuation byte
    //      1 -> more bytes to come
    //      2 -> this is the last byte

    // this funciton stores a variable uint of up to 32 bits in
    // a buffer of uint8s
    // more on varint encoding: https://protobuf.dev/programming-guides/encoding/

    inline size_t encode_varint(uint32_t value, uint8_t* buffer) {
        ssize_t i = 0;
        while (value > 127) {
            // 0x7F = 0111 1111, this masks the lowest 7 bytes
            // 0x80 = 1000 0000, this sets the ocntinuation bit
            buffer[i++] = static_cast<uint8_t>((value & 0x7F) | 0x80);
            value >>= 7;
        }
        buffer[i++] = static_cast<uint8_t>(value & 0x7F);
        return i;
    }

    // decodes varint
    // see how encoding works
    inline size_t decode_varint(const uint8_t* buffer, uint32_t& value) {
        value = 0;
        ssize_t shift = 0;
        ssize_t i = 0;
        while (true) {
            uint8_t byte = buffer[i++];
            value |= (byte & 0x7F) << shift;
            if (!(byte & 0x80)) break;
            shift += 7;
            if (shift > 28) throw std::runtime_error("Varint too long");
        }
        return i;
    }

    // type tag is stored at the first byte of the buffer
    // return num bytes written (tracks next spot in buffer)
    // [ INT32 | int32_data ]
    inline size_t serialize_int32(uint8_t* buffer, int32_t value) {
        buffer[0] = static_cast<uint8_t>(Type::INT32);
        std::memcpy(buffer + 1, &value, sizeof(int32_t));
        return 1 + sizeof(int32_t);
    }

    // return value of deserialize int32
    inline size_t deserialize_int32(const uint8_t* buffer) {
        if (buffer[0] != static_cast<uint8_t>(Type::INT32)) {
            throw std::runtime_error("Type mismatch: expected INT32");
        }

        int32_t value;
        std::memcpy(&value, buffer + 1, sizeof(int32_t));
        return value;
    }

    // encode string
    // [ String | Length | string_data ]
    inline size_t serialize_string(uint8_t* buffer, const std::string& str) {
        buffer[0] = static_cast<uint8_t>(Type::STRING);
        size_t len_bytes = encode_varint(str.size(), buffer + 1);
        std::memcpy(buffer + 1 + len_bytes, str.data(), str.size());
        return 1 + len_bytes + str.size();
    }

    // out_size tells the caller how many bytes were consumed from the buffer
    inline std::string deserialize_string(const uint8_t* buffer, size_t& out_size) {
        if (buffer[0] != static_cast<uint8_t>(Type::STRING)) {
            throw std::runtime_error("Type mismatch: expected STRING");
        }

        uint32_t len;
        ssize_t len_bytes = decode_varint(buffer + 1, len);
        out_size = 1 + len_bytes + len;
        return std::string(reinterpret_cast<const char*>(buffer + 1 + len_bytes), len);
    }

    // [ FLOAT32 | float_data ]
    inline size_t serialize_float32(uint8_t* buffer, float value) {
        buffer[0] = static_cast<uint8_t>(Type::FLOAT32);
        std::memcpy(buffer + 1, &value, sizeof(float));
        return 1 + sizeof(float);
    }

    inline float deserialize_float32(const uint8_t* buffer) {
        if (buffer[0] != static_cast<uint8_t>(Type::FLOAT32)) {
            throw std::runtime_error("Type mismatch: expected FLOAT32");
        }

        float value;
        std::memcpy(&value, buffer + 1, sizeof(float));
        return value;
    }
}