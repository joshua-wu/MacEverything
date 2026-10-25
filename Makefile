CXX = clang++
CXXFLAGS = -std=c++20 -O2 -Wall -Wextra
FRAMEWORKS = -framework CoreServices
CORE_SRCS = $(wildcard MacEverything/Core/*.cpp)
RE2_PREFIX = /opt/homebrew/opt/re2
ABSEIL_PREFIX = /opt/homebrew/opt/abseil
RE2_CFLAGS = -I$(RE2_PREFIX)/include -I$(ABSEIL_PREFIX)/include
RE2_LDFLAGS = -L$(RE2_PREFIX)/lib -lre2

# === Build targets ===
.PHONY: test test-fast test-slow test-all test-asan test-tsan build clean app dmg daemon help

test_all: test_all.cpp $(CORE_SRCS)
	$(CXX) $(CXXFLAGS) $(RE2_CFLAGS) $(FRAMEWORKS) $(RE2_LDFLAGS) -IMacEverything/Core $^ -o $@

benchmark: benchmark.cpp $(CORE_SRCS)
	$(CXX) $(CXXFLAGS) $(RE2_CFLAGS) $(FRAMEWORKS) $(RE2_LDFLAGS) $^ -o $@

maceverything-daemon: MacEverything/CLI/daemon_main.cpp $(CORE_SRCS)
	$(CXX) $(CXXFLAGS) $(RE2_CFLAGS) $(FRAMEWORKS) $(RE2_LDFLAGS) -IMacEverything/Core $^ -o $@

daemon: maceverything-daemon

# === Lint targets ===
lint-bridge:
	$(CXX) $(CXXFLAGS) -fsyntax-only -fobjc-arc -x objective-c++ \
		-IMacEverything/Core -IMacEverything/Bridge \
		MacEverything/Bridge/MacSearchBridge.mm \
		MacEverything/Bridge/MacSearchBridge+Content.mm

# === Sanitizer targets ===
test-asan: test_all.cpp $(CORE_SRCS)
	$(CXX) -std=c++20 -O1 -g -fsanitize=address -fno-omit-frame-pointer $(RE2_CFLAGS) $(FRAMEWORKS) $(RE2_LDFLAGS) -IMacEverything/Core $^ -o test_all_asan
	./test_all_asan --fast

test-tsan: test_all.cpp $(CORE_SRCS)
	$(CXX) -std=c++20 -O1 -g -fsanitize=thread $(RE2_CFLAGS) $(FRAMEWORKS) $(RE2_LDFLAGS) -IMacEverything/Core $^ -o test_all_tsan
	./test_all_tsan --fast

# === Test targets ===
test: test-fast

test-fast: test_all lint-bridge
	./test_all --fast

test-slow: test_all
	./test_all --slow

test-all: test_all
	./test_all

# === Xcode build ===
app:
	DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer xcodebuild \
		-project MacEverything.xcodeproj -scheme MacEverything \
		-configuration Release build SYMROOT=build

# === Package ===
dmg: app
	-hdiutil detach /Volumes/MacEverything 2>/dev/null
	hdiutil create -volname MacEverything \
		-srcfolder build/Release/MacEverything.app \
		-ov -format UDZO MacEverything.dmg

# === Cleanup ===
clean:
	rm -f test_all test_all_asan test_all_tsan benchmark maceverything-daemon
	rm -rf build/

# === Help ===
help:
	@echo "Available targets:"
	@echo "  make test       - Run fast unit tests (alias for test-fast)"
	@echo "  make test-fast  - Run fast unit tests (Part 3, 3b, 3c, 3d, 3e, 5)"
	@echo "  make test-slow  - Run slow integration tests (Part 1, 4, 6)"
	@echo "  make test-all   - Run all tests"
	@echo "  make app        - Build MacEverything.app via Xcode"
	@echo "  make dmg        - Build + package into DMG"
	@echo "  make daemon     - Build CLI daemon (maceverything-daemon)"
	@echo "  make benchmark  - Build benchmark binary"
	@echo "  make clean      - Remove build artifacts"
