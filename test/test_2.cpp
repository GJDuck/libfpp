#include <libf++>

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using F::operator""_idx;

namespace
{

struct Point
{
    int x;
    int y;
};

struct PointCmp
{
    F::order operator()(const Point &a, const Point &b) const
    {
        if (a.x < b.x) return F::order(F::order::LT);
        if (a.x > b.x) return F::order(F::order::GT);
        if (a.y < b.y) return F::order(F::order::LT);
        if (a.y > b.y) return F::order(F::order::GT);
        return F::order(F::order::EQ);
    }
};

struct Span
{
    int lo;
    int hi;

    int lb() const { return lo; }
    int ub() const { return hi; }
};

static size_t g_free_calls = 0;
static size_t g_free_bytes = 0;

static void tracked_free(void *ptr, size_t size)
{
    g_free_calls++;
    g_free_bytes += size;
    std::free(ptr);
}

static void test_rc_and_array()
{
    using F::array;
    using F::optional;
    using F::rc;

    rc<Point, PointCmp> p1(Point{1, 2});
    rc<Point, PointCmp> p2 = p1;
    rc<Point, PointCmp> p3(Point{2, 3});
    assert(p1);
    assert(p1.refcount() >= 2);
    assert(p1.shared(p2));
    assert(!p1.shared(p3));
    assert(p1->x == 1);
    assert((*p1).y == 2);
    assert(p1 == p2);
    assert(p1 != p3);
    assert(p1 < p3);
    p3 = nullptr;
    assert(!p3);

    optional<Point, PointCmp> maybe(Point{7, 8});
    assert(maybe);
    assert(maybe->x == 7);

    {
        char *buf = static_cast<char *>(std::malloc(6));
        std::memcpy(buf, "hello", 6);
        array<char, int> a(buf, 6, tracked_free, 17);
        array<char, int> b = a;
        array<char, int> c;
        c = b;
        assert(a.size() == 6);
        assert(a[0_idx] == 'h');
        assert(std::strcmp(a.c_str(), "hello") == 0);
        assert(a.user() == 17);
        assert(a.data()[4] == 'o');
    }

    assert(g_free_calls == 1);
    assert(g_free_bytes == 6);
}

static void test_vector()
{
    F::vector<int> xs;
    assert(xs.empty());
    xs.push_back(2);
    xs.push_front(1);
    xs += 3;
    xs.insert(3_idx, 4);
    xs.assign(1_idx, 10);
    assert(xs.size() == 4);
    assert(xs.at(0_idx) == 1);
    assert(xs[1_idx] == 10);
    assert(xs.front() == 1);
    assert(xs.back() == 4);

    F::vector<int> ys;
    ys.push_back(5);
    ys.push_back(6);
    xs.append(ys);
    F::vector<int> zs = xs + ys;
    xs += ys;
    assert(xs.size() == 8);
    assert(zs.size() == 8);
    assert(xs.compare(zs) == 0);
    assert(xs == zs);
    assert(xs.shared(xs));

    auto sum = xs.foldl<int>(0, [](int acc, int v) { return acc + v; });
    auto rev = xs.foldr<int>([](int v, int acc) { return acc * 10 + v; }, 0);
    assert(sum > 0);
    assert(rev > 0);

    auto raw = xs.data();
    assert(raw.size() == xs.size());
    assert(raw[0_idx] == xs[0_idx]);

    auto left = xs.split(3_idx);
    assert(left.size() == 3);
    assert(xs.size() == 5);
    left.left(2_idx);
    xs.right(1_idx);
    assert(left.size() == 2);
    assert(xs.size() == 4);

    F::vector<int> itv;
    itv.push_back(7);
    itv.push_back(8);
    itv.push_back(9);
    auto i = itv.begin();
    auto e = itv.end();
    assert(i != e);
    assert(*i == 7);
    assert(i.pos() == 0_idx);
    auto j = i + 2;
    assert(*j == 9);
    j -= 1;
    assert(*j == 8);
    j.assign(42);
    assert(j.value()[1_idx] == 42);
    j.insert(99);
    assert(j.value().size() == 4);
    j.erase();
    j.left();
    j.right();
    auto r = itv.rbegin();
    assert(*r == 9);
    auto f = itv.find(1_idx);
    assert(*f == 8);
    auto d = (j - i);
    assert(d >= 0);

    itv.pop_back();
    itv.pop_front();
    itv.erase(0_idx);
    itv.clear();
    assert(itv.empty());

    xs.shrink_to_fit();
    assert(xs.verify() == nullptr);
}

static void test_multiset_and_set()
{
    F::multiset<int> ms;
    assert(ms.insert(3));
    assert(ms.insert(1));
    assert(ms.insert(3));
    ms.assign(3);
    assert(ms.contains(1));
    assert(ms.search(3));
    assert(ms.at(1) == 1);
    assert(ms[3] == 3);
    assert(ms.pos(1) == 0_idx);

    auto mi = ms.find(3);
    assert(*mi == 3);
    mi.left();
    mi.right();
    auto msplit = ms.split(1_idx);
    assert(msplit.size() == 1);
    auto ms2 = ms + msplit;
    ms2 -= msplit;
    assert(ms2.size() >= 1);
    ms.pop_back();
    ms.pop_front();
    ms.erase(0_idx);
    assert(ms.verify() == nullptr);

    F::set<int> s;
    assert(s.insert(4));
    assert(!s.insert(4));
    assert(s.insert(2));
    assert(s.insert(9));
    assert(s.contains(2));
    assert(s.search(9));
    assert(s.pos(4) == 1_idx);
    auto si = s.find(4);
    assert(*si == 4);
    auto s2 = s;
    s2 += s;
    auto s3 = s2 - s;
    s2.intersect(s);
    assert(s2 == s);
    assert(s3.size() == 0);
    s.pop_back();
    s.pop_front();
    s.erase(0_idx);
    assert(s.verify() == nullptr);
}

static void test_maps()
{
    F::multimap<int, int> mm;
    assert(mm.insert({2, 20}));
    assert(mm.insert({1, 10}));
    assert(mm.insert({2, 21}));
    mm.assign({2, 22});
    assert(mm.contains(1));
    assert(mm.search(2));
    assert(mm.at(1) == 10);
    assert(mm[2] == 22);
    assert(mm.pos(2) == 1_idx);
    auto mmi = mm.find(2);
    assert((*mmi).first == 2);
    auto mms = mm.split(1_idx);
    assert(mms.size() == 1);
    mm.erase(2);
    assert(mm.verify() == nullptr);

    F::map<int, int> m;
    assert(m.insert({3, 30}));
    assert(m.insert({1, 10}));
    assert(!m.insert({3, 99}));
    assert(m.contains(3));
    assert(m.search(3));
    assert(m.at(1) == 10);
    assert(m[3] == 30);
    assert(m.pos(3) == 1_idx);
    auto mi = m.find(3);
    assert((*mi).second == 30);
    auto ms = m.split(1_idx);
    assert(ms.size() == 1);
    assert(m.erase(3));
    assert(m.verify() == nullptr);
}

static void test_string()
{
    F::string<> s("abc");
    assert(s.size() == 3);
    assert(s.front() == U'a');
    assert(s.back() == U'c');
    assert(s.at(1_idx) == U'b');
    assert(s[2_idx] == U'c');

    s.push_back(U'd');
    s.push_front(U'z');
    s += U'!';
    s.push_back(" tail");
    s.push_front("head ");
    s.assign(0_idx, U'H');
    s.insert(1_idx, U'*');
    s.insert(2_idx, "++");

    F::string<> t(" middle ");
    s.insert(3_idx, t);
    s.erase(0_idx);
    s.erase(1_idx, 3_idx);
    s.pop_back();
    s.pop_front();
    s += " more";
    s += t.str();
    s.append(t);
    auto joined = s + t;
    s += t;
    assert(joined.size() > 0);

    auto split = s.split(2_idx);
    split.left(1_idx);
    s.right(1_idx);
    assert(split.size() <= 1);

    auto arr = t.str();
    assert(arr.size() > 0);
    assert(arr.c_str() != nullptr);
    assert(t == " middle ");
    assert(t != "zzz");
    assert(t < "zzz");
    assert(t.compare(t) == 0);

    F::vector<unsigned char> bytes = t;
    assert(bytes.size() > 0);

    F::string<> edit("abcdef");
    auto i = edit.begin();
    auto e = edit.end();
    assert(i != e);
    assert(*i == U'a');
    auto j = i + 2;
    assert(*j == U'c');
    j.assign(U'X');
    j.insert(U'Y');
    j.insert("ZZ");
    j.insert(F::string<>("Q"));
    j.erase();
    j.erase(1);
    auto k = j + 1;
    j.replace(k, "R");
    j.slice(1);
    j.left();
    j.right();
    j.find(U'e');
    j.find("ef");
    j.find(F::string<>("ef"));
    auto out = j.value();
//    assert(out.size() > 0);

    edit = F::string<>("hello world");
    auto b = edit.begin();
    auto n = b + 5;
    b.replace(n, "HELLO");
    auto q = b.value();
    assert(q.size() > 0);

    edit.shrink_to_fit();
    assert(edit.verify() == nullptr);

    {
        const char *path = "/tmp/libfpp_smoke_test.txt";
        FILE *fp = std::fopen(path, "wb");
        assert(fp != nullptr);
        std::fputs("line one\nline two\n", fp);
        std::fclose(fp);

        F::string<> file;
        file.read(path);
        assert(file.size() > 0);
        file.print(stdout);
        std::remove(path);
    }
}

} // namespace

int main()
{
    F::init();

    test_rc_and_array();
    test_vector();
    test_multiset_and_set();
    test_maps();
    test_string();

    std::puts("smoke tests passed");
    return 0;
}
