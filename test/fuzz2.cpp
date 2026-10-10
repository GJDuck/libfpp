#include <cstdlib>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "libf++"

static size_t current_round;
static size_t current_op;

static void check(const std::vector<int> &ref, const F::vector<int> &test)
{
    if (test.size() != ref.size() || test.empty() != ref.empty())
        std::abort();
    if (const char *err = test.verify())
    {
        std::fprintf(stderr, "verify failed at round %zu op %zu size %zu: %s\n", current_round, current_op, ref.size(), err);
        std::abort();
    }
    if (ref.empty())
        return;
    if (test.front() != ref.front() || test.back() != ref.back())
        std::abort();
    for (size_t i = 0; i < ref.size(); i++)
        if (test[i] != ref[i] || test.at(i) != ref.at(i))
            std::abort();
    size_t iterated = 0;
    for (auto i = test.begin(), end = test.end(); i != end; ++i, ++iterated)
        if (iterated >= ref.size() || *i != ref[iterated])
            std::abort();
    if (iterated != ref.size())
        std::abort();
    auto data = test.data();
    if (data.size() != ref.size())
        std::abort();
    for (size_t i = 0; i < ref.size(); i++)
        if (data[i] != ref[i])
            std::abort();
}

static void check(const std::u32string &ref, const F::string<> &test)
{
    if (test.size() != ref.size() || test.empty() != ref.empty())
        std::abort();
    if (const char *err = test.verify())
    {
        std::fprintf(stderr, "string verify failed at round %zu op %zu: %s\n", current_round, current_op, err);
        std::abort();
    }
    for (size_t i = 0; i < ref.size(); i++)
        if (test[i] != ref[i] || test.at(i) != ref.at(i))
            std::abort();
    size_t iterated = 0;
    for (auto i = test.begin(), end = test.end(); i != end; ++i, ++iterated)
        if (iterated >= ref.size() || *i != ref[iterated])
            std::abort();
    if (iterated != ref.size())
        std::abort();
}

static void check(const std::map<int, int> &ref, const F::map<int, int> &test)
{
    if (test.size() != ref.size() || test.empty() != ref.empty()) std::abort();
    if (const char *err = test.verify())
    {
        std::fprintf(stderr, "map verify failed at round %zu op %zu: %s\n", current_round, current_op, err);
        std::abort();
    }
    for (const auto &entry: ref)
        if (!test.contains(entry.first) || test.at(entry.first) != entry.second) std::abort();
    auto expected = ref.begin();
    for (auto i = test.begin(), end = test.end(); i != end; ++i, ++expected)
        if (expected == ref.end() || (*i).first != expected->first || (*i).second != expected->second)
            std::abort();
    if (expected != ref.end()) std::abort();
}

int main(int argc, char **argv)
{
    size_t rounds = (argc > 1? std::strtoull(argv[1], nullptr, 10): 1000);
    unsigned seed = (argc > 2? std::strtoul(argv[2], nullptr, 10): 1);
    std::srand(seed);

    std::vector<int> ref;
    F::vector<int> test;
    F::vector<int> unique;
    check(ref, test);

    for (size_t i = 0; i < 100; i++)
    {
        std::vector<int> old_ref = ref;
        F::vector<int> snapshot = test;
        if (!snapshot.shared(test))
            std::abort();

        int value = std::rand();
        ref.push_back(value);
        test.push_back(value);
        unique.push_back(value);
        check(old_ref, snapshot);
        check(ref, test);
        check(ref, unique);
        if (snapshot.shared(test))
        {
            std::fprintf(stderr, "push_back retained a shared root at warmup round %zu\n", i);
            std::abort();
        }
    }

    for (size_t i = 0; i < rounds; i++)
    {
        current_round = i;
        std::vector<int> old_ref = ref;
        F::vector<int> snapshot = test;
        size_t op = std::rand() % 14;
        current_op = op;
        size_t idx;
        int value = std::rand();
        switch (op)
        {
            case 0:
                ref.push_back(value);
                test.push_back(value);
                break;
            case 1:
                ref.insert(ref.begin(), value);
                test.push_front(value);
                break;
            case 2:
                if (ref.empty()) break;
                idx = std::rand() % ref.size();
                ref[idx] = value;
                test.assign(idx, value);
                break;
            case 3:
                idx = std::rand() % (ref.size() + 1);
                ref.insert(ref.begin() + idx, value);
                test.insert(idx, value);
                break;
            case 4:
                if (ref.empty()) break;
                idx = std::rand() % ref.size();
                ref.erase(ref.begin() + idx);
                if (!test.erase(idx)) std::abort();
                break;
            case 5:
                if (ref.empty()) break;
                ref.pop_back();
                test.pop_back();
                break;
            case 6:
                if (ref.empty()) break;
                ref.erase(ref.begin());
                test.pop_front();
                break;
            case 7:
            {
                std::vector<int> extra_ref;
                F::vector<int> extra;
                size_t n = std::rand() % 20;
                for (size_t j = 0; j < n; j++)
                {
                    int x = std::rand();
                    extra_ref.push_back(x);
                    extra.push_back(x);
                }
                ref.insert(ref.end(), extra_ref.begin(), extra_ref.end());
                test.append(extra);
                break;
            }
            case 8:
            {
                idx = std::rand() % (ref.size() + 1);
                std::vector<int> left_ref(ref.begin(), ref.begin() + idx);
                std::vector<int> right_ref(ref.begin() + idx, ref.end());
                F::vector<int> left = test.split(idx);
                check(left_ref, left);
                check(right_ref, test);
                left.append(test);
                test = left;
                break;
            }
            case 9:
                idx = std::rand() % (ref.size() + 1);
                ref.resize(idx);
                test.left(idx);
                break;
            case 10:
                idx = std::rand() % (ref.size() + 1);
                ref.erase(ref.begin(), ref.begin() + idx);
                test.right(idx);
                break;
            case 11:
            {
                if (ref.empty()) break;
                idx = std::rand() % ref.size();
                auto iterator = test.find(idx);
                if (*iterator != ref[idx]) std::abort();
                iterator.assign(value);
                ref[idx] = value;
                test = iterator.value();
                break;
            }
            case 12:
            {
                if (ref.empty()) break;
                idx = std::rand() % ref.size();
                auto iterator = test.find(idx);
                iterator.insert(value);
                ref.insert(ref.begin() + idx, value);
                test = iterator.value();
                break;
            }
            case 13:
            {
                if (ref.empty()) break;
                idx = std::rand() % ref.size();
                auto iterator = test.find(idx);
                iterator.erase();
                ref.erase(ref.begin() + idx);
                test = iterator.value();
                break;
            }
        }
        check(old_ref, snapshot);
        check(ref, test);
        if (ref.size() > 500)
        {
            ref.clear();
            test.clear();
        }
    }

    std::u32string str_ref;
    F::string<> str_test;
    for (size_t i = 0; i < rounds; i++)
    {
        current_round = i;
        current_op = std::rand() % 9;
        char32_t c = static_cast<char32_t>(0x20 + std::rand() % 0x700);
        size_t idx;
        switch (current_op)
        {
            case 0: str_ref.push_back(c); str_test.push_back(c); break;
            case 1: str_ref.insert(str_ref.begin(), c); str_test.push_front(c); break;
            case 2:
                if (str_ref.empty()) break;
                idx = std::rand() % str_ref.size();
                str_ref[idx] = c; str_test.assign(idx, c);
                break;
            case 3:
                idx = std::rand() % (str_ref.size() + 1);
                str_ref.insert(str_ref.begin() + idx, c); str_test.insert(idx, c);
                break;
            case 4:
                if (str_ref.empty()) break;
                idx = std::rand() % str_ref.size();
                str_ref.erase(str_ref.begin() + idx); str_test.erase(idx);
                break;
            case 5:
                if (str_ref.empty()) break;
                str_ref.pop_back(); str_test.pop_back();
                break;
            case 6:
                if (str_ref.empty()) break;
                str_ref.erase(str_ref.begin()); str_test.pop_front();
                break;
            case 7:
                idx = std::rand() % (str_ref.size() + 1);
                str_ref.resize(idx); str_test.left(idx);
                break;
            case 8:
                idx = std::rand() % (str_ref.size() + 1);
                str_ref.erase(str_ref.begin(), str_ref.begin() + idx); str_test.right(idx);
                break;
        }
        check(str_ref, str_test);
    }

    std::map<int, int> map_ref;
    F::map<int, int> map_test;
    for (size_t i = 0; i < rounds; i++)
    {
        current_round = i;
        current_op = std::rand() % 4;
        int key = std::rand() % 200;
        int value = std::rand();
        if (current_op == 0)
        {
            bool inserted = map_ref.insert({key, value}).second;
            if (map_test.insert({key, value}) != inserted) std::abort();
        }
        else if (current_op == 1)
        {
            map_ref[key] = value;
            map_test.assign({key, value});
        }
        else if (current_op == 2 && map_test.contains(key) != (map_ref.count(key) != 0))
            std::abort();
        else if (current_op == 3)
        {
            bool expected = (map_ref.erase(key) != 0);
            bool actual = map_test.erase(key);
            if (actual != expected)
            {
                std::fprintf(stderr, "map erase mismatch at round %zu key %d: expected %d, got %d\n",
                    current_round, key, expected, actual);
                std::abort();
            }
        }
        check(map_ref, map_test);
    }

    std::puts("fuzz2 completed");
    return 0;
}
