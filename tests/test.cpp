#include "tlv.hpp"
#include "message.pb.h"
#include <iostream>
#include <cassert>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <functional>

using namespace quark;

constexpr size_t BUFFER_SIZE = 256 * 1024;
uint8_t buffer[BUFFER_SIZE];

// ---------- Basic Tests ----------

void test_int32() {
    int32_t values[] = {192, INT32_MIN, INT32_MAX};
    for (auto val : values) {
        serialize_int32(buffer, val);
        int32_t out = deserialize_int32(buffer);
        assert(val == out);
    }
    std::cout << "[PASS] INT32 tests\n";
}

void test_float32() {
    struct FloatTest { float val; bool is_special; };
    FloatTest tests[] = {
        {3.141592653f, false},
        {NAN, true},
        {INFINITY, true}
    };

    for (auto t : tests) {
        serialize_float32(buffer, t.val);
        float out = deserialize_float32(buffer);
        if (std::isnan(t.val)) assert(std::isnan(out));
        else if (std::isinf(t.val)) assert(std::isinf(out));
        else assert(std::fabs(t.val - out) < 1e-6f);
    }
    std::cout << "[PASS] FLOAT32 tests\n";
}

void test_string() {
    std::string values[] = {"hello world", "", std::string(10000, 'x')};
    size_t out_size = 0;
    for (auto &str : values) {
        serialize_string(buffer, str);
        std::string out = deserialize_string(buffer, out_size);
        assert(str == out);
    }
    std::cout << "[PASS] STRING tests\n";
}

// ---------- Latency Benchmarks ----------

void benchmark(const std::string& name, size_t ITER,
               const std::function<void()>& serialize_func,
               const std::function<void()>& deserialize_func) {
    auto start = std::chrono::high_resolution_clock::now();
    for (size_t i = 0; i < ITER; ++i) {
        serialize_func();
        deserialize_func();
    }
    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
    std::cout << std::setw(8) << name 
              << " | total: " << std::setw(7) << duration << " us"
              << " | per op: " << std::fixed << std::setprecision(6) 
              << (duration / (double)ITER) << " us" << std::endl;
}

void benchmark_protobuf(size_t ITER) {
    TestData test_data;
    test_data.set_int_val(123);
    test_data.set_float_val(3.141592653f);
    test_data.set_str_val("hello protobuf");

    std::string serialized;
    test_data.SerializeToString(&serialized);
    TestData deserialized;

    auto start = std::chrono::high_resolution_clock::now();
    for (size_t i = 0; i < ITER; ++i) {
        deserialized.ParseFromString(serialized);
        test_data.SerializeToString(&serialized);
    }
    auto end = std::chrono::high_resolution_clock::now();

    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
    std::cout << std::setw(8) << "PROTOBUF"
              << " | total: " << std::setw(7) << duration << " us"
              << " | per op: " << std::fixed << std::setprecision(6) 
              << (duration / (double)ITER) << " us" << std::endl;
}

int main() {
    constexpr size_t ITER = 1'000'000;

    std::cout << "========== TLV Basic Tests ==========\n";
    test_int32();
    test_float32();
    test_string();
    std::cout << "All basic TLV tests passed!\n\n";

    std::cout << "========== Latency Benchmarks ==========\n";
    benchmark("INT32", ITER,
              [](){ serialize_int32(buffer, 123); },
              [](){ deserialize_int32(buffer); });

    benchmark("FLOAT32", ITER,
              [](){ serialize_float32(buffer, 3.14f); },
              [](){ deserialize_float32(buffer); });

    std::string test_str = "hello quark";
    size_t out_size = 0;
    benchmark("STRING", ITER,
              [&](){ serialize_string(buffer, test_str); },
              [&](){ deserialize_string(buffer, out_size); });

    benchmark_protobuf(ITER);

    std::cout << "All latency tests completed!\n";
    return 0;
}
