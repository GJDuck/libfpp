CXX=g++
CXXFLAGS=-std=c++17 -fPIC -O2 -g -I . -fno-stack-protector -Wall

LDFLAGS=-L . -Wl,-rpath,'$$ORIGIN'
SOFLAGS=-shared
LDLIBS=-lf++

libf++.so: libf++.cpp libf++.h
	$(CXX) $(CXXFLAGS) $(SOFLAGS) $(LDFLAGS) -o $@ $<

test: test_1 test_2

test_1: test/test_1.cpp libf++.so
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

test_2: test/test_2.cpp libf++.so
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

fuzz: test/fuzz.cpp libf++.so
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

fuzz2: test/fuzz2.cpp libf++.so
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

fuzz.O0: test/fuzz.cpp libf++.so
	$(CXX) -std=c++17 -fPIC -O0 -g -I . $(LDFLAGS) -o $@ $< $(LDLIBS)

clean:
	rm -f test_1 test_2 fuzz fuzz2 fuzz.O0 libf++.so

