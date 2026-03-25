CXX      := g++
CXXFLAGS := -std=c++17 -Wall -Wextra -O2 -I.
PROTO_FLAGS := $(shell pkg-config --cflags protobuf)
LDFLAGS  := $(shell pkg-config --libs protobuf) -lncurses -lpthread

PROTO_DIR  := ../protos
TARGET := cliente

.PHONY: all proto clean

all: proto
	$(CXX) $(CXXFLAGS) $(PROTO_FLAGS) \
	    client.cpp common.pb.cc cliente-side/*.pb.cc server-side/*.pb.cc \
	    -o $(TARGET) $(LDFLAGS)

proto:
	protoc -I $(PROTO_DIR) --cpp_out=. \
	    $(PROTO_DIR)/common.proto \
	    $(PROTO_DIR)/cliente-side/*.proto \
	    $(PROTO_DIR)/server-side/*.proto

clean:
	rm -f $(TARGET) *.o *.pb.cc *.pb.h
	rm -rf cliente-side/ server-side/