/*
 * Copyright 2026 The National University of Singapore
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a copy of
 * this software and associated documentation files (the "Software"), to deal in
 * the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
 * of the Software, and to permit persons to whom the Software is furnished to do
 * so, subject to the following conditions:
 * 
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

/*
 * Simple libf++ fuzzer.
 */

#include <iostream>
#include <string>
#include <cassert>
#include <cstdlib>
#include <ctime>
#include <cassert>

#include <libf++>

#include <map>

// Simple UTF-8 encoding of a single code point
std::string encode_utf8(char32_t cp)
{
    std::string out;

    if (cp <= 0x7F)
        out += static_cast<char>(cp);
    else if (cp <= 0x7FF)
    {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
    else if (cp <= 0xFFFF)
    {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
    else if (cp <= 0x10FFFF)
    {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
    return out;
}

// Generate random code points from BMP and supplementary planes (excluding surrogates)
char32_t random_unicode_codepoint()
{
    while (true)
    {
        char32_t cp = static_cast<char32_t>(0x20 + (std::rand() % (0x10FFFF - 0x20)));
        // Skip surrogate range
        if (cp >= 0xD800 && cp <= 0xDFFF) continue;
        return cp;
    }
}

// Generates both UTF-32 and UTF-8 versions
void generate_random_unicode_string(std::u32string& out_u32, std::string& out_utf8, size_t length)
{
    out_u32.clear();
    out_utf8.clear();

    for (size_t i = 0; i < length; ++i)
    {
        char32_t cp = random_unicode_codepoint();
        out_u32 += cp;
        out_utf8 += encode_utf8(cp);
    }
}

// Equivalence check
void check_same(const std::u32string& ref, const F::string<>& test) 
{
    const char *err = test.verify();
    if (err != nullptr)
    {
        fprintf(stderr, "test.verify() = %s\"%s\"%s\n", F::RED, err, F::OFF);
        test.dump();
        abort();
    }
    if (ref.size() != test.size())
    {
        test.dump();
        fprintf(stderr, "ref.size() = %zu\n", ref.size());
        fprintf(stderr, "test.size() = %zu\n", test.size());
        abort();
    }
    auto i = ref.begin(), iend = ref.end();
    auto j = test.begin(), jend = test.end();
    for (size_t k = 0; i != iend && j != jend; ++i, ++j, ++k)
    {
        char32_t c = *i, d = *j;
        if (c == d)
            continue;
        test.dump();
        fprintf(stderr, "ref[%zu]  = 0x%x\n", k, c);
        fprintf(stderr, "test[%zu] = 0x%x\n", k, d);
        abort();
    }
}
void check_same(const std::map<int,int> &ref, const F::map<int,int> test)
{
    const char *err = test.verify();
    if (err != nullptr)
    {
        fprintf(stderr, "test.verify() = %s\"%s\"%s\n", F::RED, err, F::OFF);
        test.dump();
        abort();
    }
    if (ref.size() != test.size())
    {
        test.dump();
        fprintf(stderr, "ref.size() = %zu\n", ref.size());
        fprintf(stderr, "test.size() = %zu\n", test.size());
        abort();
    }
    auto i = ref.begin(), iend = ref.end();
    auto j = test.begin(), jend = test.end();
    for (size_t k = 0; i != iend && j != jend; ++i, ++j, ++k)
    {
        if (i->first == j->first && i->second == j->second)
            continue;
        test.dump();
        fprintf(stderr, "ref[%d]  = %d\n", i->first, i->second);
        fprintf(stderr, "test[%d] = %d\n", j->first, j->second);
        abort();
    }
}

// Generate a random Unicode code point
char32_t random_codepoint()
{
    if (std::rand() % 3 == 0)
        return static_cast<char32_t>(0x20 + (std::rand() % (0x7FF0))); // Skips control chars
    else
        return static_cast<char32_t>(0x20 + (std::rand() % (0x70)));
}

/*
 * Main.
 */
int main(int argc, char **argv) {
    if (argv[1] != nullptr)
        std::srand(atoi(argv[1]));
    else
        std::srand(static_cast<unsigned int>(std::time(nullptr)));

    std::u32string ref, ref2;
    F::string<> test, old, test2;

    std::map<int, int> mref;
    F::map<int, int> mtest;

    const int NUM_ITERATIONS = 10000000;
    const size_t MAX = 8192;

    for (int i = 0; i < NUM_ITERATIONS; ++i) {

        if (ref.size() > MAX)
        {
            size_t nlen = rand() % MAX;
            ref.erase(ref.begin() + nlen, ref.end());
            test.left(nlen);
        }
        if (mref.size() > MAX / 16)
        {
            size_t nlen = rand() % (MAX / 16);
            std::map<int, int> tmp;
            auto i = mref.begin();
            for (size_t j = 0; j < nlen; j++, ++i)
                tmp.insert({i->first, i->second});
            mref.swap(tmp);
            mtest.left(nlen);
            assert(mref.size() == mtest.size());
        }

        size_t len = ref.size();
        size_t pos = len ? std::rand() % len : 0;
        char32_t ch = random_codepoint();

        if (std::rand() % 100 == 0)
            old = test;

        switch (std::rand() % 33) {
            case 0: case 1: case 2: case 3: case 4: case 5: // Insert
                fprintf(stderr, "%d str.%sinsert%s(%zu,0x%x)\n", i, F::GREEN, F::OFF, pos, ch);
                ref.insert(ref.begin() + pos, ch);
                test.insert(pos, ch);
                break;

            case 6: case 7: // Delete
                fprintf(stderr, "%d str.%serase%s(%zu)\n", i, F::RED, F::OFF, pos);
                if (len > 0) {
                    ref.erase(ref.begin() + pos);
                    test.erase(pos);
                }
                break;

            case 8: case 9: // Assign
                fprintf(stderr, "%d str.%sassign%s(%zu,0x%x)\n", i, F::YELLOW, F::OFF, pos, ch);
                if (len > 0) {
                    ref[pos] = ch;
                    test.assign(pos, ch);
                }
                break;
            
            case 10:    // push_back
                fprintf(stderr, "%d str.%spush_back%s(0x%x)\n", i, F::GREEN, F::OFF, ch);
                ref += ch;
                test.push_back(ch);
                break;
            case 11:    // push_front
                fprintf(stderr, "%d str.%spush_front%s(0x%x)\n", i, F::GREEN, F::OFF, ch);
                ref.insert(ref.begin(), ch);
                test.push_front(ch);
                break;
            case 12:
            {
                size_t max = len - pos;
                max = (max > 256? 256: max);
                max = (max == 0? 1: max);
                size_t n = rand() % max + 1;
                fprintf(stderr, "%d str.%serase%s(%zu,%zu)\n", i, F::RED, F::OFF, pos, pos + n);
                if (len == 0)
                    break;
                ref2 = ref.substr(pos, n);
                ref.erase(pos, n);
                test2 = test;
                test2.left(pos + n);
                test2.right(pos);
                test.erase(pos, pos + n);
                break;
            }
            case 13:
                fprintf(stderr, "%d str.%sinsert%s(%zu,\"...\")\n", i, F::GREEN, F::OFF, pos);
                ref.insert(pos, ref2);
                test.insert(pos, test2);
                break;
            case 14:    // push_back
                fprintf(stderr, "%d str.%spop_back%s()\n", i, F::RED, F::OFF);
                if (len > 0) {
                    ref.pop_back();
                    test.pop_back();
                }
                break;
            case 15:    // push_front
                fprintf(stderr, "%d str.%spop_front%s()\n", i, F::RED, F::OFF);
                if (len > 0) {
                    ref.erase(0, 1);
                    test.pop_front();
                }
                break;
            case 16:
            {
                fprintf(stderr, "%d str.%soperator+=%s(\"...\")\n", i, F::GREEN, F::OFF);
                std::u32string ref_str;
                std::string test_utf8;
                generate_random_unicode_string(ref_str, test_utf8, rand() % 32);
                ref += ref_str;
                test += test_utf8.c_str();
                break;
            }
            case 17:
            {
                fprintf(stderr, "%d str.%sinsert%s(%zu,c\"...\")\n", i, F::GREEN, F::OFF, pos);
                std::u32string ref_str;
                std::string test_utf8;
                generate_random_unicode_string(ref_str, test_utf8, rand() % 32);
                ref.insert(pos, ref_str);
                test.insert(pos, test_utf8.c_str());
                break;
            }
            case 18:
            {
                fprintf(stderr, "%d i<%zu>.%sinsert%s(0x%x)\n", i, pos, F::GREEN, F::OFF, ch);
                if (len > 0)
                {
                    ref.insert(ref.begin() + pos, ch);
                    auto i = test.begin() + pos;
                    i.insert(ch);
                    test = i.value();
                }
                break;
            }
            case 19:
            {
                fprintf(stderr, "%d i<%zu>.%sassign%s(0x%x)\n", i, pos, F::YELLOW, F::OFF, ch);
                if (len > 0)
                {
                    *(ref.begin() + pos) = ch;
                    auto i = test.begin() + pos;
                    i.assign(ch);
                    test = i.value();
                }
                break;
            }
            case 20:
            {
                fprintf(stderr, "%d i<%zu>.%serase%s()\n", i, pos, F::RED, F::OFF);
                if (len > 0) {
                    ref.erase(ref.begin() + pos);
                    auto i = test.begin() + pos;
                    i.erase();
                    test = i.value();
                }
                break;
            }
            case 21:
            {
                fprintf(stderr, "%d i<%zu>.%sinsert%s(0x%x)\n", i, pos, F::GREEN, F::OFF, ch);
                if (len > 0)
                {
                    ref.insert(pos, ref2);
                    auto i = test.begin() + pos;
                    i.insert(test2);
                    test = i.value();
                }
                break;
            }
            case 22: case 23: case 24: case 25: case 26: case 27: case 28:
            {
                int key = std::rand() % 8192, val = std::rand() % 8192;
                fprintf(stderr, "%d map.%sinsert%s({%d,%d})\n", i, F::GREEN, F::OFF, key, val);
                mref.insert({key, val});
                mtest.insert({key, val});
                break;
            }
            case 29:
            {
                fprintf(stderr, "%d map.%serase%s(%zu)\n", i, F::RED, F::OFF, pos);
                len = mtest.size();
                size_t pos = len ? std::rand() % len : 0;
                if (len > 0) {
                    auto i = mref.begin(); 
                    for (size_t j = 0; j < pos; j++)
                        ++i;
                    mref.erase(i);
                    mtest.erase(F::index(pos));
                }
                break;
            }
            case 30:
            {
                fprintf(stderr, "%d map.%spop_back%s()\n", i, F::RED, F::OFF);
                if (mtest.size() > 0) {
                    mref.erase(std::prev(mref.end()));
                    mtest.pop_back();
                }
                break;
            }
            case 31:
            {
                fprintf(stderr, "%d map.%spop_front%s()\n", i, F::RED, F::OFF);
                if (mtest.size() > 0) {
                    mref.erase(mref.begin());
                    mtest.pop_front();
                }
                break;
            }
            case 32:
            {
                int key = std::rand() % 8192, val = std::rand() % 8192;
                fprintf(stderr, "%d map.%sassign%s({%d,%d})\n", i, F::YELLOW, F::OFF, key, val);
                mref[key] = val;
                mtest.assign({key, val});
                break;
            }
        }

        check_same(ref, test);
        check_same(mref, mtest);
    }

    std::cout << "Fuzzing completed.\n";
    return 0;
}

