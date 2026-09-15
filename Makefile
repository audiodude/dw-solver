CXX ?= c++
CXXFLAGS ?= -O3 -std=c++20 -Wall -Wextra -Wpedantic
EMXX ?= em++
PORT ?= 5555
WEBFLAGS = -std=c++20 -O3 -fexceptions \
	-sMODULARIZE=1 -sEXPORT_ES6=1 -sINVOKE_RUN=0 -sENVIRONMENT=worker \
	-sEXPORTED_FUNCTIONS='["_main"]' -sEXPORTED_RUNTIME_METHODS='["callMain"]' \
	-sALLOW_MEMORY_GROWTH=1 -sSTACK_SIZE=8388608 -sEXIT_RUNTIME=1

.PHONY: all web serve test clean
all: dont-wordle

dont-wordle: solver.cpp
	$(CXX) $(CXXFLAGS) $< -o $@

web: web/solver.mjs web/solver.wasm

web/solver.mjs web/solver.wasm &: solver.cpp data/words.txt Makefile
	mkdir -p web
	$(EMXX) $(WEBFLAGS) solver.cpp --embed-file data/words.txt@/data/words.txt -o web/solver.mjs

serve: web
	python3 -m http.server $(PORT) --bind 127.0.0.1 --directory web

test: dont-wordle
	python3 -m unittest -v test_solver.py

clean:
	rm -f dont-wordle web/solver.mjs web/solver.wasm
