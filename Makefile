CXX = g++
CXXFLAGS = -std=c++20 -Iinclude -Iproto_gen -Wall -Wextra
LDFLAGS = -lprotobuf

TEST_SRC = tests/test.cpp proto_gen/message.pb.cc
TEST_BIN = test_tlv

.PHONY: test clean

test: $(TEST_BIN)
	./$(TEST_BIN)

# Compile the test binary
$(TEST_BIN): $(TEST_SRC)
	$(CXX) $(CXXFLAGS) $(TEST_SRC) -o $(TEST_BIN) $(LDFLAGS)

clean:
	rm -f $(TEST_BIN)
