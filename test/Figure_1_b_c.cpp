#include <iostream>
#include <list>
#include <string>
#include <vector>
#include <iomanip>

// -----------------------------
// Version 1 (list<char32_t>)
// -----------------------------
void filterAscii_list(std::list<char32_t> &s) {
    for (auto i = s.begin(); i != s.end(); )
        if (*i >= 0x7F) i = s.erase(i);
        else            ++i;
}

// -----------------------------
// Version 2 (u32string)
// -----------------------------
void filterAscii_u32string(std::u32string &s) {
    std::u32string t;
    for (auto c : s)
        if (c < 0x7F)
            t += c;
    s.swap(t);
}

// -----------------------------
// Reference implementation
// -----------------------------
std::u32string filterAscii_ref(const std::u32string &s) {
    std::u32string t;
    for (auto c : s)
        if (c < 0x7F)
            t += c;
    return t;
}

// -----------------------------
// Helpers
// -----------------------------
std::u32string listToString(const std::list<char32_t> &lst) {
    std::u32string s;
    for (auto c : lst)
        s += c;
    return s;
}

std::list<char32_t> stringToList(const std::u32string &s) {
    std::list<char32_t> lst;
    for (auto c : s)
        lst.push_back(c);
    return lst;
}

// Print as hex code points (portable, no locale issues)
void printU32(const std::u32string &s) {
    std::cout << "\"";
    for (auto c : s) {
        if (c < 128 && std::isprint(static_cast<unsigned char>(c))) {
            std::cout << static_cast<char>(c);
        } else {
            std::cout << "\\u"
                      << std::hex << std::uppercase << std::setw(4)
                      << std::setfill('0') << static_cast<uint32_t>(c)
                      << std::dec;
        }
    }
    std::cout << "\"";
}

// -----------------------------
// Test runner
// -----------------------------
void runTest(const std::u32string &input) {
    std::cout << "Input:    ";
    printU32(input);
    std::cout << "\n";

    auto expected = filterAscii_ref(input);

    std::cout << "Expected: ";
    printU32(expected);
    std::cout << "\n";

    // Test u32string version
    auto s1 = input;
    filterAscii_u32string(s1);

    std::cout << "String:   ";
    printU32(s1);
    std::cout << "\n";

    // Test list version
    auto lst = stringToList(input);
    filterAscii_list(lst);
    auto s2 = listToString(lst);

    std::cout << "List:     ";
    printU32(s2);
    std::cout << "\n";

    bool ok = (s1 == expected) && (s2 == expected);

    if (ok)
        std::cout << "✅ PASS\n\n";
    else
        std::cout << "❌ FAIL\n\n";
}

// -----------------------------
// Main
// -----------------------------
int main() {
    std::cout << "Running Unicode tests...\n\n";

    runTest(U"");
    runTest(U"hello");
    runTest(U"abc\u007Fdef");     // DEL
    runTest(U"abc\u0080def");     // non-ASCII
    runTest(U"héllo");            // Latin-1
    runTest(U"こんにちは");       // Japanese
    runTest(U"ASCII only 123!");
    runTest(U"a\u007Fb\u0080c\u0900d"); // mixed

    return 0;
}
