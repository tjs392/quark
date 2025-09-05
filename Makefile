CXX = g++
CXXFLAGS = -std=c++20 -Iinclude -Iproto_gen -Wall -Wextra
LDFLAGS = -lprotobuf -lgtest -lgtest_main -pthread

TEST_SRC = $(wildcard tests/*.cpp) proto_gen/message.pb.cc
TEST_BIN = test_all

.PHONY: test clean

test: $(TEST_BIN)
	./$(TEST_BIN)

$(TEST_BIN): $(TEST_SRC)
	$(CXX) $(CXXFLAGS) $(TEST_SRC) -o $(TEST_BIN) $(LDFLAGS)

clean:
	rm -f $(TEST_BIN)
