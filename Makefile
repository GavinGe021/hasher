CXX       = g++
CXXFLAGS  = -std=c++17 -O3 -pthread
LDFLAGS   = -lcrypto -lssl
TARGET    = hasher
SRC       = hasher.cpp

$(TARGET): $(SRC)
	$(CXX) $(CXXFLAGS) -o $(TARGET) $(SRC) $(LDFLAGS)

clean:
	rm -f $(TARGET)

.PHONY: clean