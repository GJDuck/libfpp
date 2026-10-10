#include <cstdlib>
#include <cstdio>
#include <vector>

#include "libf++"

static void check(const std::vector<int> &ref, const F::vector<int> &test)
{
    if (test.size() != ref.size() || test.empty() != ref.empty())
        std::abort();
    if (const char *err = test.verify())
    {
        std::fprintf(stderr, "verify failed: %s\n", err);
        std::abort();
    }
    if (ref.empty())
        return;
    if (test.front() != ref.front() || test.back() != ref.back())
        std::abort();
    for (size_t i = 0; i < ref.size(); i++)
        if (test[i] != ref[i] || test.at(i) != ref.at(i))
            std::abort();
    auto data = test.data();
    if (data.size() != ref.size())
        std::abort();
    for (size_t i = 0; i < ref.size(); i++)
        if (data[i] != ref[i])
            std::abort();
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

    for (size_t i = 0; i < rounds; i++)
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
            std::abort();
    }

    std::puts("fuzz2 completed");
    return 0;
}
