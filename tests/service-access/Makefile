# Host tests for the service grant verifier of med-service-access (ACE-03).
# Compiles ServiceGrant.cpp as it ships, against the host's libcrypto, and
# checks it against grants gen.sh signs with the openssl command line.
SRC := ../../meta-custom/meta-med-framework/recipes-framework/med-service-access/files/src

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -g -O0
CXXFLAGS += -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Werror -I$(SRC)
LDLIBS   += $(shell pkg-config --libs libcrypto)

build/test_grant: test_grant.cpp $(SRC)/ServiceGrant.cpp $(SRC)/ServiceGrant.h
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -o $@ test_grant.cpp $(SRC)/ServiceGrant.cpp $(LDLIBS)

build/pki/good.p7s: gen.sh
	@sh gen.sh $(CURDIR)/build/pki

check: build/test_grant build/pki/good.p7s
	@./build/test_grant build/pki

clean:
	rm -rf build

.PHONY: check clean
