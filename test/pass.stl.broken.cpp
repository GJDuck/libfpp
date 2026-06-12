#include <cstdio>
#include <set>
#include <vector>

static __attribute__((__noinline__)) void transform(std::set<int> &s)
{
    for (auto it = s.begin(); it != s.end(); ++it)
        if (*it % 2 == 0)
            s.insert(2 * *it);
}

int main(void)
{
    std::set<int> s;
    s.insert(-10);
    s.insert(-1);
    s.insert(0);
    s.insert(1);
    s.insert(2);
    s.insert(3);
    s.insert(10);
    s.insert(11);
    s.insert(12);
    s.insert(13);
    s.insert(1301);
    s.insert(1302);

    for (auto x: s)
        printf("%d ", x);
    putchar('\n');
	transform(s);
    for (auto x: s)
        printf("%d ", x);
    putchar('\n');
    return 0;
}

