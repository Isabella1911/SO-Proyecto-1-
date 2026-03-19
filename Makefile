CXX      = g++
CXXFLAGS = -std=c++17 -pthread -Isrc
LIBS     = `pkg-config --cflags --libs protobuf`

.PHONY: all protos server client clean

all: protos server client

protos:
	protoc -I protos/cliente-side -I protos/server-side -I protos \
		--cpp_out=src \
		protos/common.proto
	protoc -I protos/cliente-side -I protos -I protos/server-side \
		--cpp_out=src \
		protos/cliente-side/*.proto
	protoc -I protos/server-side -I protos -I protos/cliente-side \
		--cpp_out=src \
		protos/server-side/*.proto
	@echo "[protos] Generated .pb.h and .pb.cc in src/"
server: protos
	$(CXX) $(CXXFLAGS) -o server src/server/server.cpp src/*.pb.cc $(LIBS)
	@echo "[done] Built ./server"

client: protos
	$(CXX) $(CXXFLAGS) -o client src/client/client.cpp src/*.pb.cc $(LIBS)
	@echo "[done] Built ./client"

clean:
	rm -f server client src/*.pb.cc src/*.pb.h
	@echo "[clean] Done"