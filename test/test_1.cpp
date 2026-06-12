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
 * Some basic libf++ tests.
 */

#include <vector>
#include <map>

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <cctype>
#include <cwctype>

#include <sys/time.h>

#include <libf++>

static uint64_t gettime(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    uint64_t ms = tv.tv_sec * 1000 + tv.tv_usec / 1000;
    return ms;
}

static F::string<> decode(F::string<> s)
{
    auto i = s.begin();
    const auto end = s.end();

    while (i != end) {
        // Look for a backslash
        if (*i == '\\') {
            auto j = i;
            ++j;
            if (j != end && *j == 'u') {
                // Look ahead for 4 hex digits
                uint32_t x = 0;
                for (int n = 0; n < 4; ++n) {
                    x = (x << 4);
                    ++j;
                    if (j == end)
                        goto no_match;
                    if (*j >= '0' && *j <= '9')
                        x += static_cast<uint32_t>(*j - '0');
                    else if (*j >= 'A' && *j <= 'F')
                        x += 10 + static_cast<uint32_t>(*j - 'A');
                    else if (*j >= 'a' && *j <= 'f')
                        x += 10 + static_cast<uint32_t>(*j - 'a');
                    else
                        goto no_match;
                }

                // Convert hex to code point
                char32_t c = static_cast<char32_t>(x);


                // Erase the 6-character sequence: \ u X X X X
                i.erase(6);

                static int ccc = 0;
                if (ccc++ == 13 || ccc == 17)
                {
                    i.insert(F::RED);
                    i.insert("CAT");
                    i.insert(F::OFF);
                }

                // Insert the codepoint at the current iterator position
                i.insert(c);
                continue;
            }
        }
    no_match:
        ++i;
    }

    return i.value();
}

static F::string<> encode(F::string<> s)
{
    auto i = s.begin();
    const auto end = s.end();

    while (i != end)
    {
        char32_t c = *i;
        if (c >= 0x7F)
        {
            i.erase();
            i.insert('\\');
            i.insert('u');
            uint32_t x = static_cast<uint32_t>(c);
            for (int n = 0; n < 4; n++)
            {
                uint32_t d = (x >> ((3 - n) * 4)) & 0xF;
                if (d >= 0 && d <= 9)
                    i.insert('0' + d);
                else
                    i.insert('a' + (d - 0xa));
            }
        }
        ++i;
    }

    return i.value();
}

int main(int argc, char **argv)
{
    if (argc > 1)
        (void)atoi(argv[1]);
    srand(time(nullptr));
    F::init();

    {
        F::string<> s2;

        s2.push_back("\\u3042\\u3044\\u3046\\u3048\\u304A"  // あいうえお
                     "\\u304B\\u304D\\u304F\\u3051\\u3053"  // かきくけこ
                     "\\u3055\\u3057\\u3059\\u305B\\u305D"  // さしすせそ
                     "\\u305F\\u3061\\u3064\\u3066\\u3068"  // たちつてと
                     "\\u306A\\u306B\\u306C\\u306D\\u306E"  // なにぬねの
                     "\\u306F\\u3072\\u3075\\u3078\\u307B"  // はひふへほ
                     "\\u307E\\u307F\\u3080\\u3081\\u3082"  // まみむめも
                     "\\u3084\\u3086\\u3088"                // やゆよ
                     "\\u3089\\u308A\\u308B\\u308C\\u308D"  // らりるれろ
                     "\\u308F\\u3092\\u3093");
        s2 = decode(s2);

        print(s2);
        putchar('\n');

        {
            auto i = s2.begin(), iend = s2.end();
            for (i.find("CAT"); i != iend; i.find("CAT"))
            {
                auto j = i + 3;
                i.replace(j, "DOG");
            }
            print(i.value());
            putchar('\n');
        }

        {
            s2.read("libf++.cpp");
            uint64_t t0 = gettime();
            s2 = encode(s2);
            uint64_t t1 = gettime();
            putchar('\n');
            printf("time = %zums\n", t1 - t0);
        }

        {
            putchar('\n');
            puts("---libf++---");
            int N = 1000000;
            {
                srand(123456);
                F::map<int, int> m;
                uint64_t t0 = gettime();
                for (int i = 0; i < N; i++)
                    m.insert({rand(), i});
                uint64_t t1 = gettime();
                printf("time [build] = %zums\n", t1 - t0);
                size_t sum = 0;
                t0 = gettime();
                for (const auto x: m)
                    sum += x.second;
                t1 = gettime();
                printf("time [scan ] = %zums\n", t1 - t0);
                printf("size = %zu\n", m.size());
                printf("sum = %zu\n", sum);
                sum = 0;
            }
            puts("---stl------");
            {
                srand(123456);
                std::map<int, int> m;
                uint64_t t0 = gettime();
                for (int i = 0; i < N; i++)
                    m.insert({rand(), i});
                uint64_t t1 = gettime();
                printf("time [build] = %zums\n", t1 - t0);
                size_t sum = 0;
                t0 = gettime();
                for (const auto &x: m)
                    sum += x.second;
                t1 = gettime();
                printf("time [scan ] = %zums\n", t1 - t0);
                printf("size = %zu\n", m.size());
                printf("sum = %zu\n", sum);
            }
        }
    }
}
