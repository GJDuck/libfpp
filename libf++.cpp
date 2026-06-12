/*  _     _ _     _____ ____  ____  
 * | |   (_) |__ |  ___|  _ \|  _ \
 * | |   | | '_ \| |_  | |_) | |_) |
 * | |___| | |_) |  _| |  __/|  __/ 
 * |_____|_|_.__/|_|   |_|   |_|    
 * 
 * ------------------------------------------------------------------------------
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
 * ------------------------------------------------------------------------------
 */

#include <libf++>

#include <sys/mman.h>

#include <cassert>
#include <cstdlib>
#include <cstring>

using namespace F;

namespace F
{

thread_local SEQ<LOCAL> *LOCAL_NIL;
thread_local OBJECT<LOCAL> *LOCAL_HEAP = nullptr;

SEQ<SHARE> SHARE_NIL;
OBJECT<SHARE> *SHARE_HEAP = nullptr;
std::mutex     SHARE_HEAP_MUTEX;

LIBFPP_NORETURN void panic(const char *msg)
{
    fprintf(stderr, "%spanic%s: %s\n", RED, OFF, msg);
    std::abort();
}

template <typename T, typename... Ts>
static inline void gc(const T *keep, const Ts *... rest)
{
    const void *seen[1 + sizeof...(Ts)] = {reinterpret_cast<const void *>(keep)};
    size_t n = 1;

    auto visit = [&](const auto *p)
    {
        if (p == nullptr)
            return;
        const void *addr = reinterpret_cast<const void *>(p);
        for (size_t i = 0; i < n; i++)
        {
            if (seen[i] == addr)
                return;
        }
        seen[n++] = addr;
        p->gc();
    };

    (visit(rest), ...);
}
#define GC(s, ...)     gc(s, ##__VA_ARGS__)

template <typename S, typename M>
static LIBFPP_PURE size_t len(const TREE<S, M> *t)
{
    return t->len;
}
template <typename S>
static LIBFPP_PURE size_t len(const TREE<S, STR> *t)
{
    return t->monoid.len;
}
template <typename S, typename M>
static LIBFPP_PURE size_t len(const DIG<S, M> *d)
{
    return d->len;
}
template <typename S>
static LIBFPP_PURE size_t len(const DIG<S, STR> *d)
{
    return d->monoid.len;
}
template <typename S, typename M>
static LIBFPP_PURE size_t len(const SEQ<S, M> *s)
{
    return s->len;
}
template <typename S>
static LIBFPP_PURE size_t len(const SEQ<S, STR> *s)
{
    return s->monoid.len;
}

template <typename S, typename M>
static LIBFPP_PURE ssize_t slen(const TREE<S, M> *t)
{
    return ssize_t(len(t));
}
template <typename S>
static LIBFPP_PURE ssize_t slen(const TREE<S, STR> *t)
{
    return ssize_t(len(t));
}
template <typename S, typename M>
static LIBFPP_PURE ssize_t slen(const DIG<S, M> *d)
{
    return ssize_t(len(d));
}
template <typename S>
static LIBFPP_PURE ssize_t slen(const DIG<S, STR> *d)
{
    return ssize_t(len(d));
}
template <typename S, typename M>
static LIBFPP_PURE ssize_t slen(const SEQ<S, M> *s)
{
    return ssize_t(len(s));
}
template <typename S>
static LIBFPP_PURE ssize_t slen(const SEQ<S, STR> *s)
{
    return ssize_t(len(s));
}

static LIBFPP_PURE char32_t utf8_decode(const char *cstr)
{
    char32_t c = 0;
    if ((*cstr & 0x80) == 0)
    {
        c = (char32_t)*cstr;
        return c;
    }
    if ((*cstr & 0xE0) == 0xC0)
    {
        c = ((((char32_t)*cstr) & 0x1F) << 6);
utf8_decode_2_bytes:
        cstr++;
        if ((*cstr & 0xC0) != 0x80)
            goto utf8_decode_bad_seq_error;
        c |= (((char32_t)*cstr) & 0x3F);
        return c;
    }
    if ((*cstr & 0xF0) == 0xE0)
    {
        c = ((((char32_t)*cstr) & 0x0F) << 12);
utf8_decode_3_bytes:
        cstr++;
        if ((*cstr & 0xC0) != 0x80)
            goto utf8_decode_bad_seq_error;
        c |= ((((char32_t)*cstr) & 0x3F) << 6);
        goto utf8_decode_2_bytes;
    }
    if ((*cstr & 0xF8) == 0xF0)
    {
        c = ((((char32_t)*cstr) & 0x07) << 18);
        cstr++;
        if ((*cstr & 0xC0) != 0x80)
            goto utf8_decode_bad_seq_error;
        c |= ((((char32_t)*cstr) & 0x3F) << 12);
        goto utf8_decode_3_bytes;
    }

utf8_decode_bad_seq_error:
    LIBFPP_PANIC("bad utf8 character");
}

static char *utf8_encode(char *cstr, char32_t c)
{
    if (c <= 0x7F)
    {
        cstr[0] = c;
        return cstr + 1;
    }
    if (c <= 0x7FF)
    {
        cstr[1] = (c & 0x3F) | 0x80;
        c >>= 6;
        cstr[0] = c | 0xC0;
        return cstr + 2;
    }
    if (c <= 0xFFFF)
    {
        cstr[2] = (c & 0x3F) | 0x80;
        c >>= 6;
        cstr[1] = (c & 0x3F) | 0x80;
        c >>= 6;
        cstr[0] = c | 0xE0;
        return cstr + 3;
    }
    if (c <= 0x1FFFFF)
    {
        cstr[3] = (c & 0x3F) | 0x80;
        c >>= 6;
        cstr[2] = (c & 0x3F) | 0x80;
        c >>= 6;
        cstr[1] = (c & 0x3F) | 0x80;
        c >>= 6;
        cstr[0] = c | 0xF0;
        return cstr + 4;
    }
    LIBFPP_PANIC("bad utf8 character");
}

static LIBFPP_PURE size_t utf8_decode_len(const char *cstr)
{
    char c = *cstr;
    if ((c & 0x80) == 0)
        return 1;
    if ((c & 0xE0) == 0xC0)
        return 2;
    if ((c & 0xF0) == 0xE0)
        return 3;
    if ((c & 0xF8) == 0xF0)
        return 4;
    return 1;
}

static LIBFPP_PURE size_t utf8_encode_len(char32_t c)
{
    if (c <= 0x7F)
        return 1;
    if (c <= 0x7FF)
        return 2;
    if (c <= 0xFFFF)
        return 3;
    if (c <= 0x1FFFFF)
        return 4;
    LIBFPP_PANIC("bad utf8 character");
}

static LIBFPP_PURE char32_t utf8_decode_len_backward(const char *cstr, const char *cmin)
{
    if (cstr <= cmin)
        LIBFPP_PANIC("bad utf8 character");
    const char *p = cstr;
    size_t clen = 1;
    while (p > cmin)
    {
        const char *q = p - 1;
        if ((*q & 0xC0) != 0x80)
            break;
        p = q;
        clen++;
    }
    const char *lead = cstr - clen;
    if (lead < cmin || utf8_decode_len(lead) != clen)
        LIBFPP_PANIC("bad utf8 character");
    return clen;
}

LIBFPP_PURE size_t utf8_len(const char *cstr, size_t max)
{
    size_t len = 0;
    for (size_t i = 0; i < max; )
    {
        len++;
        i += utf8_decode_len(cstr + i);
    }
    return len;
}

LIBFPP_PURE char32_t utf8_get(const char *cstr, size_t idx)
{
    size_t i = 0;
    while (idx >= sizeof(uint64_t))
    {
        uint64_t c64 = *reinterpret_cast<const uint64_t *>(cstr + i);
        c64 &= 0x8080808080808080ll;
        if (LIBFPP_LIKELY(c64 == 0))
        {
            // FAST PATH:
            idx -= sizeof(uint64_t);
            i   += sizeof(uint64_t);
            continue;
        }
        for (size_t j = 0; j < sizeof(uint64_t); j++)
        {
            i += utf8_decode_len(cstr + i);
            idx--;
        }
    }
    while (idx > 0)
    {
        i += utf8_decode_len(cstr + i);
        idx--;
    }
    return utf8_decode(cstr + i);
}

static void utf8_tree_print(const UTF8_TREE<LOCAL> *t)
{
    switch (t->tag)
    {
        case UTF8_TREE<LOCAL>::ELEMENT:
            for (size_t i = 0; i < t->len; i++)
                fputc(t->str[i], stderr);
            break;
        case UTF8_TREE<LOCAL>::TREE_2:
            utf8_tree_print(t->t[0]);
            utf8_tree_print(t->t[1]);
            break;
        case UTF8_TREE<LOCAL>::TREE_3:
            utf8_tree_print(t->t[0]);
            utf8_tree_print(t->t[1]);
            utf8_tree_print(t->t[2]);
            break;
    }
}
static void utf8_dig_print(const UTF8_DIG<LOCAL> *d)
{
    for (size_t i = 0; i <= d->tag; i++)
        utf8_tree_print(d->t[i]);
}
static void utf8_seq_print(const UTF8_SEQ<LOCAL> *s)
{
    switch (s->tag)
    {
        case UTF8_SEQ<LOCAL>::NIL: return;
        case UTF8_SEQ<LOCAL>::SINGLE:
            utf8_tree_print(s->t);
            return;
        case UTF8_SEQ<LOCAL>::DEEP:
            utf8_dig_print(s->l);
            utf8_seq_print(s->m);
            utf8_dig_print(s->r);
            return;
    }
}

void __attribute__((__constructor__(101))) init()
{
    void *ptr = mmap(NULL, (8ull << 30), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (ptr == MAP_FAILED)
        LIBFPP_PANIC("mmap() failed");
    LOCAL::init(ptr);

    LOCAL_NIL = LOCAL::allocate<SEQ<LOCAL>>();
    LOCAL_NIL->refcount = INT64_MAX;
    LOCAL_NIL->tag = SEQ<LOCAL>::NIL;
    LOCAL_NIL->len = 0;
    LOCAL_NIL->l   = reinterpret_cast<DIG<LOCAL> *>(LOCAL_NIL);
    LOCAL_NIL->r   = reinterpret_cast<DIG<LOCAL> *>(LOCAL_NIL);
    LOCAL_NIL->m   = LOCAL_NIL;
}

static LIBFPP_PURE size_t tree_depth(const TREE<LOCAL> *t)
{
    switch (t->tag)
    {
        case TREE<LOCAL>::ELEMENT: return 1;
        case TREE<LOCAL>::TREE_2: case TREE<LOCAL>::TREE_3:
            return 1 + tree_depth(t->t[0]);
    }
    LIBFPP_UNREACHABLE();
}
static LIBFPP_PURE size_t dig_depth(const DIG<LOCAL> *d)
{
    return 1 + tree_depth(d->t[0]);
}
static LIBFPP_PURE size_t seq_depth(const SEQ<LOCAL> *s)
{
    switch (s->tag)
    {
        case SEQ<LOCAL>::NIL: return 0;
        case SEQ<LOCAL>::SINGLE:
            return 1 + tree_depth(s->t);
        case SEQ<LOCAL>::DEEP:
            return 1 + dig_depth(s->l);
    }
    LIBFPP_UNREACHABLE();
}

template <typename S, typename M = EMPTY>
static LIBFPP_PURE size_t depth(const TREE<S, M> *t)
{
    if (t == nullptr) return 0;
    return F::tree_depth(reinterpret_cast<const TREE<LOCAL> *>(t));
}
template <typename S, typename M = EMPTY>
static LIBFPP_PURE size_t depth(const DIG<S, M> *d)
{
    if (d == nullptr) return 0;
    return F::dig_depth(reinterpret_cast<const DIG<LOCAL> *>(d));
}
template <typename S, typename M = EMPTY>
static LIBFPP_PURE size_t depth(const SEQ<S, M> *s)
{
    if (s == nullptr) return 0;
    return F::seq_depth(reinterpret_cast<const SEQ<LOCAL> *>(s));
}

static LIBFPP_PURE const char *tree_verify(const TREE<LOCAL> *t, size_t size, size_t level)
{
    if (t->refcount == 0)
        return "bad tree refcount";
    size_t len = 0;
    switch (t->tag)
    {
        case TREE<LOCAL>::ELEMENT:
            if (t->size != size)
                return "bad element size";
            if (t->len == 0 || t->len * size > TREE<LOCAL>::DATA_SIZE)
                return "bad element length";
            if (level != 0)
                return "bad balance";
            return nullptr;
        case TREE<LOCAL>::TREE_2: case TREE<LOCAL>::TREE_3:
            if (const char *err = tree_verify(t->t[0], size, level-1))
                return err;
            if (const char *err = tree_verify(t->t[1], size, level-1))
                return err;
            len = t->t[0]->len + t->t[1]->len;
            if (t->tag == TREE<LOCAL>::TREE_3)
            {
                if (const char *err = tree_verify(t->t[2], size, level-1))
                    return err;
                len += t->t[2]->len;
            }
            if (t->len != len)
                return "bad tree length";
            return nullptr;
        default:
            return "bad tree tag";
    }
}
static LIBFPP_PURE const char *dig_verify(const DIG<LOCAL> *d, size_t size, size_t level)
{
    if (d->refcount == 0)
        return "bad dig refcount";
    size_t len = 0;
    for (size_t i = 0; i <= d->tag; i++)
    {
        if (const char *err = tree_verify(d->t[i], size, level))
            return err;
        len += d->t[i]->len;
    }
    if (d->len != len)
        return "bad dig length";
    return nullptr;
}
static LIBFPP_PURE const char *seq_verify(const SEQ<LOCAL> *s, size_t size, size_t level)
{
    if (s->refcount == 0)
        return "bad seq refcount";
    switch (s->tag)
    {
        case SEQ<LOCAL>::NIL:
            if (s->len != 0)
                return "bad nil length";
            return nullptr;
        case SEQ<LOCAL>::SINGLE:
            if (s->len != s->t->len)
                return "bad single length";
            return tree_verify(s->t, size, level);
        case SEQ<LOCAL>::DEEP:
            if (s->len != s->l->len + s->m->len + s->r->len)
                return "bad deep length";
            if (const char *err = dig_verify(s->l, size, level))
                return err;
            if (const char *err = seq_verify(s->m, size, level+1))
                return err;
            if (const char *err = dig_verify(s->r, size, level))
                return err;
            return nullptr;
        default:
            return "bad seq tag";
    }
}

template <typename S>
LIBFPP_PURE const char *INTERNAL<S>::seq_verify(const SEQ<S> *s, size_t size)
{
    return F::seq_verify(reinterpret_cast<const SEQ<LOCAL> *>(s), size, 0);
}
template <typename S>
LIBFPP_PURE const char *INTERNAL<S>::seq_verify(const SEQ<S, MIN> *s, size_t size)
{
    return F::seq_verify(reinterpret_cast<const SEQ<LOCAL> *>(s), size, 0);
}
template <typename S>
LIBFPP_PURE const char *INTERNAL<S>::seq_verify(const UTF8_SEQ<S> *s, size_t size)
{
    return F::seq_verify(reinterpret_cast<const SEQ<LOCAL> *>(s), size, 0);
}

static void *tree_write(const TREE<LOCAL> *t, void *buf, void *(*write)(void *, const TREE<LOCAL> *))
{
    switch (t->tag)
    {
        case TREE<LOCAL>::ELEMENT: return write(buf, t);
        case TREE<LOCAL>::TREE_2: case TREE<LOCAL>::TREE_3:
            buf = tree_write(t->t[0], buf, write);
            buf = tree_write(t->t[1], buf, write);
            if (t->tag == TREE<LOCAL>::TREE_3)
                buf = tree_write(t->t[2], buf, write);
            return buf;
    }
    LIBFPP_UNREACHABLE();
}
static void *dig_write(const DIG<LOCAL> *d, void *buf, void *(*write)(void *, const TREE<LOCAL> *))
{
    for (size_t i = 0; i <= d->tag; i++)
        buf = tree_write(d->t[i], buf, write);
    return buf;
}
static void *seq_write(const SEQ<LOCAL> *s, void *buf, void *(*write)(void *, const TREE<LOCAL> *))
{
    switch (s->tag)
    {
        case SEQ<LOCAL>::NIL:    return buf;
        case SEQ<LOCAL>::SINGLE: return tree_write(s->t, buf, write);
        case SEQ<LOCAL>::DEEP:
            buf = dig_write(s->l, buf, write);
            buf = seq_write(s->m, buf, write);
            buf = dig_write(s->r, buf, write);
            return buf;
    }
    LIBFPP_UNREACHABLE();
}

template <typename S>
void INTERNAL<S>::seq_write(const SEQ<S> *s, void *buf, void *(*write)(void *, const TREE<S> *))
{
    F::seq_write(reinterpret_cast<const SEQ<LOCAL> *>(s), buf, reinterpret_cast<void *(*)(void *, const TREE<LOCAL> *)>(write));
}
template <typename S>
void INTERNAL<S>::seq_write(const SEQ<S, MIN> *s, void *buf, void *(*write)(void *, const TREE<S, MIN> *))
{
    F::seq_write(reinterpret_cast<const SEQ<LOCAL> *>(s), buf, reinterpret_cast<void *(*)(void *, const TREE<LOCAL> *)>(write));
}
template <typename S>
void INTERNAL<S>::str_write(const UTF8_SEQ<S> *s, void *buf, void *(*write)(void *, const UTF8_TREE<S> *))
{
    F::seq_write(reinterpret_cast<const SEQ<LOCAL> *>(s), buf, reinterpret_cast<void *(*)(void *, const TREE<LOCAL> *)>(write));
}

template <typename S, typename M = EMPTY>
static LIBFPP_PURE result<const TREE<S, M> *, size_t> tree_get(const TREE<S, M> *t, size_t idx)
{
    switch (t->tag)
    {
        case TREE<S, M>::ELEMENT:
            return {t, idx};
        case TREE<S, M>::TREE_2: case TREE<S, M>::TREE_3:
            if (idx < t->t[0]->len)
                return tree_get(t->t[0], idx);
            idx -= t->t[0]->len;
            if (idx < t->t[1]->len)
                return tree_get(t->t[1], idx);
            if (t->tag == TREE<S, M>::TREE_3)
            {
                idx -= t->t[1]->len;
                return tree_get(t->t[2], idx);
            }
    }
    LIBFPP_UNREACHABLE();
}
template <typename S, typename M = EMPTY>
static LIBFPP_PURE result<const TREE<S, M> *, size_t> dig_get(const DIG<S, M> *d, size_t idx)
{
    for (size_t i = 0; i <= d->tag; i++)
    {
        if (idx < d->t[i]->len)
            return tree_get(d->t[i], idx);
        idx -= d->t[i]->len;
    }
    LIBFPP_UNREACHABLE();
}
template <typename S, typename M = EMPTY>
static LIBFPP_PURE result<const TREE<S, M> *, size_t> seq_get_2(const SEQ<S, M> *s, size_t idx)
{
    switch (s->tag)
    {
        case SEQ<S, M>::SINGLE:
            return tree_get(s->t, idx);
        case SEQ<S, M>::DEEP:
            if (idx < s->l->len)
                return dig_get(s->l, idx);
            idx -= s->l->len;
            if (idx < s->m->len)
                return seq_get_2(s->m, idx);
            idx -= s->m->len;
            return dig_get(s->r, idx);
        default:
            LIBFPP_UNREACHABLE();
    }
}

template <typename S>
LIBFPP_PURE result<const TREE<S> *, size_t> INTERNAL<S>::seq_get(const SEQ<S> *s, size_t idx)
{
    if (idx >= s->len) LIBFPP_PANIC("index out-of-bounds");
    auto r = seq_get_2<S>(s, idx);
    return {r.a, r.b};
}
template <typename S>
LIBFPP_PURE result<const TREE<S, MIN> *, size_t> INTERNAL<S>::seq_get(const SEQ<S, MIN> *s, size_t idx)
{
    if (idx >= s->len) LIBFPP_PANIC("index out-of-bounds");
    auto r = seq_get_2<S, MIN>(s, idx);
    return {r.a, r.b};
}

template <typename S, typename M = EMPTY> static const SEQ<S, M> *single(const TREE<S, M> *t, const void *arg = nullptr)
{
    return S::template allocate<SEQ<S, M>>(t, arg);
}
template <typename S, typename M = EMPTY> static const SEQ<S, M> *deep(const DIG<S, M> *l, const SEQ<S, M> *m, const DIG<S, M> *r, const void *arg = nullptr)
{
    return S::template allocate<SEQ<S, M>>(l, m, r, arg);
}
template <typename S, typename M = EMPTY> static const DIG<S, M> *dig(const TREE<S, M> *t1, const void *arg = nullptr)
{
    return S::template allocate<DIG<S, M>>(t1, arg);
}
template <typename S, typename M = EMPTY> static const DIG<S, M> *dig(const TREE<S, M> *t1, const TREE<S, M> *t2, const void *arg = nullptr)
{
    return S::template allocate<DIG<S, M>>(t1, t2, arg);
}
template <typename S, typename M = EMPTY> static const DIG<S, M> *dig(const TREE<S, M> *t1, const TREE<S, M> *t2, const TREE<S, M> *t3, const void *arg = nullptr)
{
    return S::template allocate<DIG<S, M>>(t1, t2, t3, arg);
}
template <typename S, typename M = EMPTY> static const DIG<S, M> *dig(const TREE<S, M> *t1, const TREE<S, M> *t2, const TREE<S, M> *t3, const TREE<S, M> *t4, const void *arg = nullptr)
{
    return S::template allocate<DIG<S, M>>(t1, t2, t3, t4, arg);
}
template <typename S, typename M = EMPTY> static const TREE<S, M> *tree(const TREE<S, M> *t1, const TREE<S, M> *t2, const void *arg = nullptr)
{
    return S::template allocate<TREE<S, M>>(t1, t2, arg);
}
template <typename S, typename M = EMPTY> static const TREE<S, M> *tree(const TREE<S, M> *t1, const TREE<S, M> *t2, const TREE<S, M> *t3, const void *arg = nullptr)
{
    return S::template allocate<TREE<S, M>>(t1, t2, t3, arg);
}

template <typename S, typename M = EMPTY>
static const SEQ<S, M> *dig_to_seq(const DIG<S, M> *d, const void *arg = nullptr)
{
    switch (d->tag)
    {
        case DIG<S, M>::DIG_1: return single(d->t[0], arg);
        case DIG<S, M>::DIG_2: return deep(dig(d->t[0], arg), nil<S, M>(), dig(d->t[1], arg), arg);
        case DIG<S, M>::DIG_3: return deep(dig(d->t[0], d->t[1], arg), nil<S, M>(), dig(d->t[2], arg), arg);
        case DIG<S, M>::DIG_4: return deep(dig(d->t[0], d->t[1], arg), nil<S, M>(), dig(d->t[2], d->t[3], arg), arg);
    }
    LIBFPP_UNREACHABLE();
}

template <typename S, typename M = EMPTY>
static const DIG<S, M> *tree_to_dig(const TREE<S, M> *t, const void *arg = nullptr)
{
    switch (t->tag)
    {
        case TREE<S, M>::ELEMENT: abort();      // Should not occur?
        case TREE<S, M>::TREE_2:  return dig(t->t[0], t->t[1], arg);
        case TREE<S, M>::TREE_3:  return dig(t->t[0], t->t[1], t->t[2], arg);
    }
    LIBFPP_UNREACHABLE();
}

template <typename S, typename M = EMPTY>
static const SEQ<S, M> *deep(const DIG<S, M> *l, const SEQ<S, M> *m, const void *arg = nullptr)
{
    if (m->tag == SEQ<S, M>::NIL)
        return dig_to_seq(l, arg);
    auto r = seq_pop_back(m, arg);
    auto *tmp = deep(l, r.a, tree_to_dig(r.b, arg), arg);
    return tmp;
}

template <typename S, typename M = EMPTY>
static const SEQ<S, M> *deep(const SEQ<S, M> *m, const DIG<S, M> *r, const void *arg = nullptr)
{
    if (m->tag == SEQ<S, M>::NIL)
        return dig_to_seq(r, arg);
    auto s = seq_pop_front(m, arg);
    return deep(tree_to_dig(s.b, arg), s.a, r, arg);
}

template <typename S>
struct AssignInfo
{
    size_t idx;
    const void *data;
    const TREE<S> *(*set)(const TREE<S> *, size_t, const void *);
};

template <typename S>
static const TREE<S> *tree_assign(const TREE<S> *t, AssignInfo<S> *info)
{
    switch (t->tag)
    {
        case TREE<S>::ELEMENT:
            return info->set(t, info->idx, info->data);
        case TREE<S>::TREE_2:
            if (info->idx < t->t[0]->len)
                return tree(tree_assign(t->t[0], info), t->t[1]);
            info->idx -= t->t[0]->len;
            return tree(t->t[0], tree_assign(t->t[1], info));
        case TREE<S>::TREE_3:
            if (info->idx < t->t[0]->len)
                return tree(tree_assign(t->t[0], info), t->t[1], t->t[2]);
            info->idx -= t->t[0]->len;
            if (info->idx < t->t[1]->len)
                return tree(t->t[0], tree_assign(t->t[1], info), t->t[2]);
            info->idx -= t->t[1]->len;
            return tree(t->t[0], t->t[1], tree_assign(t->t[2], info));
    }
    LIBFPP_UNREACHABLE();
}
template <typename S>
static const DIG<S> *dig_assign(const DIG<S> *d, AssignInfo<S> *info)
{
    switch (d->tag)
    {
        case DIG<S>::DIG_1:
            return dig(tree_assign(d->t[0], info));
        case DIG<S>::DIG_2:
            if (info->idx < d->t[0]->len)
                return dig(tree_assign(d->t[0], info), d->t[1]);
            info->idx -= d->t[0]->len;
            return dig(d->t[0], tree_assign(d->t[1], info));
        case DIG<S>::DIG_3:
            if (info->idx < d->t[0]->len)
                return dig(tree_assign(d->t[0], info), d->t[1], d->t[2]);
            info->idx -= d->t[0]->len;
            if (info->idx < d->t[1]->len)
                return dig(d->t[0], tree_assign(d->t[1], info), d->t[2]);
            info->idx -= d->t[1]->len;
            return dig(d->t[0], d->t[1], tree_assign(d->t[2], info));
        case DIG<S>::DIG_4:
            if (info->idx < d->t[0]->len)
                return dig(tree_assign(d->t[0], info), d->t[1], d->t[2], d->t[3]);
            info->idx -= d->t[0]->len;
            if (info->idx < d->t[1]->len)
                return dig(d->t[0], tree_assign(d->t[1], info), d->t[2], d->t[3]);
            info->idx -= d->t[1]->len;
            if (info->idx < d->t[2]->len)
                return dig(d->t[0], d->t[1], tree_assign(d->t[2], info), d->t[3]);
            info->idx -= d->t[2]->len;
            return dig(d->t[0], d->t[1], d->t[2], tree_assign(d->t[3], info));
    }
    LIBFPP_UNREACHABLE();
}
template <typename S>
static const SEQ<S> *seq_assign(const SEQ<S> *s, AssignInfo<S> *info)
{
    switch (s->tag)
    {
        case SEQ<LOCAL>::SINGLE:
            return single(tree_assign(s->t, info));
        case SEQ<LOCAL>::DEEP:
            if (info->idx < s->l->len)
                return deep(dig_assign(s->l, info), s->m, s->r);
            info->idx -= s->l->len;
            if (info->idx < s->m->len)
                return deep(s->l, seq_assign(s->m, info), s->r);
            info->idx -= s->m->len;
            return deep(s->l, s->m, dig_assign(s->r, info));
        default:
            LIBFPP_UNREACHABLE();
    }
}

template <typename S>
const SEQ<S> *INTERNAL<S>::seq_assign(const SEQ<S> *s, size_t idx, const TREE<S> *(*set)(const TREE<S> *, size_t, const void *), const void *data)
{
    if (idx >= s->len) LIBFPP_PANIC("index out-of-bounds");
    AssignInfo<S> info;
    info.idx = idx;
    info.set = set;
    info.data = data;
    return F::seq_assign<S>(s, &info);
}

template <typename S, typename M = EMPTY>
static const SEQ<S, M> *seq_push_back(const SEQ<S, M> *s, const TREE<S, M> *t, const void *arg = nullptr)
{
    switch (s->tag)
    {
        case SEQ<S, M>::NIL:
            return single(t, arg);
        case SEQ<S, M>::SINGLE:
            return deep(dig(s->t, arg), nil<S, M>(), dig(t, arg), arg);
        case SEQ<S, M>::DEEP:
        {
            const auto *d = s->r;
            switch (d->tag)
            {
                case DIG<S, M>::DIG_1: return deep(s->l, s->m, dig(d->t[0], t, arg), arg);
                case DIG<S, M>::DIG_2: return deep(s->l, s->m, dig(d->t[0], d->t[1], t, arg), arg);
                case DIG<S, M>::DIG_3: return deep(s->l, s->m, dig(d->t[0], d->t[1], d->t[2], t, arg), arg);
                case DIG<S, M>::DIG_4:
                {
                    if (s->l->tag == DIG<S, M>::DIG_1 && s->m->tag == SEQ<S, M>::NIL)
                        return deep(dig(s->l->t[0], d->t[0], d->t[1], d->t[2], arg), nil<S, M>(), dig(d->t[3], t, arg), arg);
                    const TREE<S, M> *t3 = tree(d->t[0], d->t[1], d->t[2], arg);
                    const SEQ<S, M> *m = seq_push_back(s->m, t3, arg);
                    return deep(s->l, m, dig(d->t[3], t, arg), arg);
                }
            }
        }
    }
    LIBFPP_UNREACHABLE();
}

template <typename S>
const SEQ<S> *INTERNAL<S>::seq_push_back(const SEQ<S> *s, const TREE<S> *t)
{
    return F::seq_push_back<S>(s, t);
}

template <typename S, typename M = EMPTY>
static const SEQ<S, M> *seq_replace_back(const SEQ<S, M> *s, const TREE<S, M> *t, const void *arg = nullptr)
{
    switch (s->tag)
    {
        case SEQ<S, M>::SINGLE:
            return single(t, arg);
        case SEQ<S, M>::DEEP:
        {
            const auto *d = s->r;
            switch (d->tag)
            {
                case DIG<S, M>::DIG_1: d = dig(t, arg); break;
                case DIG<S, M>::DIG_2: d = dig(d->t[0], t, arg); break;
                case DIG<S, M>::DIG_3: d = dig(d->t[0], d->t[1], t, arg); break;
                case DIG<S, M>::DIG_4: d = dig(d->t[0], d->t[1], d->t[2], t, arg); break;
                default:
                    LIBFPP_UNREACHABLE();
            }
            return deep(s->l, s->m, d, arg);
        }
        default:
            LIBFPP_UNREACHABLE();
    }
}

template <typename S>
const SEQ<S> *INTERNAL<S>::seq_replace_back(const SEQ<S> *s, const TREE<S> *t, const void *arg)
{
    return F::seq_replace_back<S>(s, t, arg);
}
template <typename S>
const SEQ<S, MIN> *INTERNAL<S>::seq_replace_back(const SEQ<S, MIN> *s, const TREE<S, MIN> *t, const void *arg)
{
    return F::seq_replace_back<S, MIN>(s, t, arg);
}

template <typename S, typename M = EMPTY>
static const SEQ<S, M> *seq_push_front(const SEQ<S, M> *s, const TREE<S, M> *t, const void *arg = nullptr)
{
    switch (s->tag)
    {
        case SEQ<S, M>::NIL:
            return single(t, arg);
        case SEQ<S, M>::SINGLE:
            return deep(dig(t, arg), nil<S, M>(), dig(s->t, arg), arg);
        case SEQ<S, M>::DEEP:
        {
            const DIG<S, M> *d = s->l;
            switch (d->tag)
            {
                case DIG<S, M>::DIG_1: return deep(dig(t, d->t[0], arg), s->m, s->r, arg);
                case DIG<S, M>::DIG_2: return deep(dig(t, d->t[0], d->t[1], arg), s->m, s->r, arg);
                case DIG<S, M>::DIG_3: return deep(dig(t, d->t[0], d->t[1], d->t[2], arg), s->m, s->r, arg);
                case DIG<S, M>::DIG_4:
                {
                    if (s->r->tag == DIG<S, M>::DIG_1 && s->m->tag == SEQ<S, M>::NIL)
                        return deep(dig(t, d->t[0], arg), nil<S, M>(), dig(d->t[1], d->t[2], d->t[3], s->r->t[0], arg), arg);
                    const TREE<S, M> *t3 = tree(d->t[1], d->t[2], d->t[3], arg);
                    const SEQ<S, M> *m = seq_push_front(s->m, t3, arg);
                    return deep(dig(t, d->t[0], arg), m, s->r, arg);
                }
                default:
                    LIBFPP_UNREACHABLE();
            }
        }
    }
    LIBFPP_UNREACHABLE();
}

template <typename S>
const SEQ<S> *INTERNAL<S>::seq_push_front(const SEQ<S> *s, const TREE<S> *t)
{
    return F::seq_push_front<S>(s, t);
}

template <typename S, typename M = EMPTY>
static const SEQ<S, M> *seq_replace_front(const SEQ<S, M> *s, const TREE<S, M> *t, const void *arg = nullptr)
{
    switch (s->tag)
    {
        case SEQ<S, M>::SINGLE:
            return single(t, arg);
        case SEQ<S, M>::DEEP:
        {
            const DIG<S, M> *d = s->l;
            switch (d->tag)
            {
                case DIG<S, M>::DIG_1: d = dig(t, arg); break;
                case DIG<S, M>::DIG_2: d = dig(t, d->t[1], arg); break;
                case DIG<S, M>::DIG_3: d = dig(t, d->t[1], d->t[2], arg); break;
                case DIG<S, M>::DIG_4: d = dig(t, d->t[1], d->t[2], d->t[3], arg); break;
                default:
                     LIBFPP_UNREACHABLE();
            }
            return deep(d, s->m, s->r, arg);
        }
        default:
            LIBFPP_UNREACHABLE();
    }
}

template <typename S>
const SEQ<S> *INTERNAL<S>::seq_replace_front(const SEQ<S> *s, const TREE<S> *t, const void *arg)
{
    return F::seq_replace_front<S>(s, t, arg);
}
template <typename S>
const SEQ<S, MIN> *INTERNAL<S>::seq_replace_front(const SEQ<S, MIN> *s, const TREE<S, MIN> *t, const void *arg)
{
    return F::seq_replace_front<S, MIN>(s, t, arg);
}

template <typename S, typename M = EMPTY>
static result<const SEQ<S, M> *, const TREE<S, M> *> seq_pop_back(const SEQ<S, M> *s, const void *arg = nullptr)
{
    switch (s->tag)
    {
        case SEQ<S, M>::SINGLE:
            return {nil<S, M>(), s->t};
        case SEQ<S, M>::DEEP:
        {
            const DIG<S, M> *d = s->r;
            const TREE<S, M> *t = d->t[d->tag];
            switch (d->tag)
            {
                case DIG<S, M>::DIG_1: return {deep(s->l, s->m, arg), t};
                case DIG<S, M>::DIG_2: return {deep(s->l, s->m, dig(d->t[0], arg), arg), t};
                case DIG<S, M>::DIG_3: return {deep(s->l, s->m, dig(d->t[0], d->t[1], arg), arg), t};
                case DIG<S, M>::DIG_4: return {deep(s->l, s->m, dig(d->t[0], d->t[1], d->t[2], arg), arg), t};
                default:
                    LIBFPP_UNREACHABLE();
            }
        }
        default:
            LIBFPP_UNREACHABLE();
    }
}

template <typename S>
result<const SEQ<S> *, const TREE<S> *> INTERNAL<S>::seq_pop_back(const SEQ<S> *s, const void *arg)
{
    return F::seq_pop_back<S>(s, arg);
}
template <typename S>
result<const SEQ<S, MIN> *, const TREE<S, MIN> *> INTERNAL<S>::seq_pop_back(const SEQ<S, MIN> *s, const void *arg)
{
    return F::seq_pop_back<S, MIN>(s, arg);
}

template <typename S, typename M = EMPTY>
static result<const SEQ<S, M> *, const TREE<S, M> *> seq_pop_front(const SEQ<S, M> *s, const void *arg = nullptr)
{
    switch (s->tag)
    {
        case SEQ<S, M>::SINGLE:
            return {nil<S, M>(), s->t};
        case SEQ<S, M>::DEEP:
        {
            const DIG<S, M> *d = s->l;
            const TREE<S, M> *t = d->t[0];
            switch (d->tag)
            {
                case DIG<S, M>::DIG_1: return {deep(s->m, s->r, arg), t};
                case DIG<S, M>::DIG_2: return {deep(dig(d->t[1], arg), s->m, s->r, arg), t};
                case DIG<S, M>::DIG_3: return {deep(dig(d->t[1], d->t[2], arg), s->m, s->r, arg), t};
                case DIG<S, M>::DIG_4: return {deep(dig(d->t[1], d->t[2], d->t[3], arg), s->m, s->r, arg), t};
                default:
                    LIBFPP_UNREACHABLE();
            }
        }
        default:
            LIBFPP_UNREACHABLE();
    }
}

template <typename S>
result<const SEQ<S> *, const TREE<S> *> INTERNAL<S>::seq_pop_front(const SEQ<S> *s, const void *arg)
{
    return F::seq_pop_front<S>(s, arg);
}
template <typename S>
result<const SEQ<S, MIN> *, const TREE<S, MIN> *> INTERNAL<S>::seq_pop_front(const SEQ<S, MIN> *s, const void *arg)
{
    return F::seq_pop_front<S, MIN>(s, arg);
}

template <typename S, typename M = EMPTY>
struct InsertInfo
{
    size_t refcount;
    size_t idx;
    const void *data;
    const TREE<S, M> *next;
    result <const TREE<S, M> *, const TREE<S, M> *, const TREE<S, M> *> (*insert)(const TREE<S, M> *, const TREE<S, M> *, size_t, const void *, size_t);
};

template <typename S, typename M>
static const TREE<S, M> *tree_insert(const TREE<S, M> *t, InsertInfo<S, M> *info)
{
    const TREE<S, M> *u;
    auto *v = const_cast<TREE<S, M> *>(t);
    info->refcount |= t->refcount;
    switch (t->tag)
    {
        case TREE<S, M>::ELEMENT:
        {
            auto r = info->insert(t, nullptr, info->idx, info->data, info->refcount);
            info->next = r.b;
            return r.a;
        }
        case TREE<S, M>::TREE_2:
            if (t->t[0]->tag == TREE<S, M>::ELEMENT)
            {
                auto r = info->insert(t->t[0], t->t[1], info->idx, info->data, info->refcount);
                if (r.a == t->t[0] && r.b == t->t[1]) goto fast_2;
                u = (r.c == nullptr? tree(r.a, r.b): tree(r.a, r.b, r.c));
                return u;
            }
            if (info->idx < len(t->t[0]))
            {
                u = tree_insert(t->t[0], info);
                if (u == t->t[0]) goto fast_2;
                u = (info->next != nullptr? tree(u, info->next, t->t[1]): tree(u, t->t[1]));
                info->next = nullptr;
                return u;
            }
            info->idx -= len(t->t[0]);
            u = tree_insert(t->t[1], info);
            if (u == t->t[1]) goto fast_2;
            u = (info->next != nullptr? tree(t->t[0], u, info->next): tree(t->t[0], u));
            info->next = nullptr;
            return u;
        fast_2:
            v->len = v->t[0]->len + v->t[1]->len;
            v->monoid = M(v);
            return v;
        case TREE<S, M>::TREE_3:
            if (t->t[0]->tag == TREE<S, M>::ELEMENT)
            {
                if (info->idx < len(t->t[0]))
                {
                    auto r = info->insert(t->t[0], t->t[1], info->idx, info->data, info->refcount);
                    if (r.a == t->t[0] && r.b == t->t[1]) goto fast_3;
                    info->next = (r.c == nullptr? nullptr: tree(r.c, t->t[2]));
                    u = (r.c == nullptr? tree(r.a, r.b, t->t[2]): tree(r.a, r.b));
                    return u;
                }
                info->idx -= len(t->t[0]);
                auto r = info->insert(t->t[1], t->t[2], info->idx, info->data, info->refcount);
                if (r.a == t->t[1] && r.b == t->t[2]) goto fast_3;
                info->next = (r.c == nullptr? nullptr: tree(r.b, r.c));
                u = (r.c == nullptr? tree(t->t[0], r.a, r.b): tree(t->t[0], r.a));
                return u;
            }
            if (info->idx < len(t->t[0]))
            {
                u = tree_insert(t->t[0], info);
                if (u == t->t[0]) goto fast_3;
                if (info->next == nullptr)
                    return tree(u, t->t[1], t->t[2]);
                u = tree(u, info->next);
                info->next = tree(t->t[1], t->t[2]);
                return u;
            }
            info->idx -= len(t->t[0]);
            if (info->idx < len(t->t[1]))
            {
                u = tree_insert(t->t[1], info);
                if (u == t->t[1]) goto fast_3;
                if (info->next == nullptr)
                    return tree(t->t[0], u, t->t[2]);
                info->next = tree(info->next, t->t[2]);
                u = tree(t->t[0], u);
                return u;
            }
            info->idx -= len(t->t[1]);
            u = tree_insert(t->t[2], info);
            if (u == t->t[2]) goto fast_3;
            if (info->next == nullptr)
                return tree(t->t[0], t->t[1], u);
            info->next = tree(u, info->next);
            u = tree(t->t[0], t->t[1]);
            return u;
        fast_3:
            v->len = v->t[0]->len + v->t[1]->len + v->t[2]->len;
            v->monoid = M(v);
            return v;
    }
    LIBFPP_UNREACHABLE();
}

template <typename S, typename M>
static const DIG<S, M> *dig_insert_left(const DIG<S, M> *d, InsertInfo<S, M> *info)
{
    const TREE<S, M> *t;
    const DIG<S, M> *e;
    auto *f = const_cast<DIG<S, M> *>(d);
    info->refcount |= d->refcount;
    switch (d->tag)
    {
        case DIG<S, M>::DIG_1:
            t = tree_insert(d->t[0], info);
            if (t == d->t[0])
            {
                f->len = d->t[0]->len;
                f->monoid = M(f);
                return f;
            }
            e = (info->next != nullptr? dig(t, info->next): dig(t));
            info->next = nullptr;
            return e;
        case DIG<S, M>::DIG_2:
            if (d->t[0]->tag == TREE<S, M>::ELEMENT)
            {
                auto r = info->insert(d->t[0], d->t[1], info->idx, info->data, info->refcount);
                if (r.a == d->t[0] && r.b == d->t[1]) goto fast_2;
                e = (r.c == nullptr? dig(r.a, r.b): dig(r.a, r.b, r.c));
                return e;
            }
            if (info->idx < len(d->t[0]))
            {
                t = tree_insert(d->t[0], info);
                if (t == d->t[0]) goto fast_2;
                e = (info->next != nullptr? dig(t, info->next, d->t[1]): dig(t, d->t[1]));
                info->next = nullptr;
                return e;
            }
            info->idx -= len(d->t[0]);
            t = tree_insert(d->t[1], info);
            if (t == d->t[1]) goto fast_2;
            e = (info->next != nullptr? dig(d->t[0], t, info->next): dig(d->t[0], t));
            info->next = nullptr;
            return e;
        fast_2:
            f->len = d->t[0]->len + d->t[1]->len;
            f->monoid = M(f);
            return f;
        case DIG<S, M>::DIG_3:
            if (d->t[0]->tag == TREE<S, M>::ELEMENT)
            {
                if (info->idx < len(d->t[0]))
                {
                    auto r = info->insert(d->t[0], d->t[1], info->idx, info->data, info->refcount);
                    if (r.a == d->t[0] && r.b == d->t[1]) goto fast_3;
                    e = (r.c == nullptr? dig(r.a, r.b, d->t[2]): dig(r.a, r.b, r.c, d->t[2]));
                    return e;
                }
                info->idx -= len(d->t[0]);
                auto r = info->insert(d->t[1], d->t[2], info->idx, info->data, info->refcount);
                if (r.a == d->t[1] && r.b == d->t[2]) goto fast_3;
                e = (r.c == nullptr? dig(d->t[0], r.a, r.b): dig(d->t[0], r.a, r.b, r.c));
                return e;
            }
            if (info->idx < len(d->t[0]))
            {
                t = tree_insert(d->t[0], info);
                if (t == d->t[0]) goto fast_3;
                e = (info->next != nullptr? dig(t, info->next, d->t[1], d->t[2]): dig(t, d->t[1], d->t[2]));
                info->next = nullptr;
                return e;
            }
            info->idx -= len(d->t[0]);
            if (info->idx < len(d->t[1]))
            {
                t = tree_insert(d->t[1], info);
                if (t == d->t[1]) goto fast_3;
                e = (info->next != nullptr? dig(d->t[0], t, info->next, d->t[2]): dig(d->t[0], t, d->t[2]));
                info->next = nullptr;
                return e;
            }
            info->idx -= len(d->t[1]);
            t = tree_insert(d->t[2], info);
            if (t == d->t[2]) goto fast_3;
            e = (info->next != nullptr? dig(d->t[0], d->t[1], t, info->next): dig(d->t[0], d->t[1], t));
            info->next = nullptr;
            return e;
        fast_3:
            f->len = d->t[0]->len + d->t[1]->len + d->t[2]->len;
            f->monoid = M(f);
            return f;
        case DIG<S, M>::DIG_4:
            if (d->t[0]->tag == TREE<S, M>::ELEMENT)
            {
                if (info->idx < len(d->t[0]) + len(d->t[1]))
                {
                    auto r = info->insert(d->t[0], d->t[1], info->idx, info->data, info->refcount);
                    if (r.a == d->t[0] && r.b == d->t[1]) goto fast_4;
                    info->next = (r.c == nullptr? nullptr: tree(d->t[2], d->t[3]));
                    e = (r.c == nullptr? dig(r.a, r.b, d->t[2], d->t[3]): dig(r.a, r.b, r.c));
                    return e;
                }
                info->idx -= len(d->t[0]) + len(d->t[1]);
                auto r = info->insert(d->t[2], d->t[3], info->idx, info->data, info->refcount);
                if (r.a == d->t[2] && r.b == d->t[3]) goto fast_4;
                info->next = (r.c == nullptr? nullptr: tree(r.b, r.c));
                e = (r.c == nullptr? dig(d->t[0], d->t[1], r.a, r.b): dig(d->t[0], d->t[1], r.a));
                return e;
            }
            if (info->idx < len(d->t[0]))
            {
                t = tree_insert(d->t[0], info);
                if (t == d->t[0]) goto fast_4;
                if (info->next != nullptr)
                {
                    e = dig(t, info->next, d->t[1]);
                    info->next = tree(d->t[2], d->t[3]);
                    return e;
                }
                return dig(t, d->t[1], d->t[2], d->t[3]);
            }
            info->idx -= len(d->t[0]);
            if (info->idx < len(d->t[1]))
            {
                t = tree_insert(d->t[1], info);
                if (t == d->t[1]) goto fast_4;
                if (info->next != nullptr)
                {
                    e = dig(d->t[0], t, info->next);
                    info->next = tree(d->t[2], d->t[3]);
                    return e;
                }
                return dig(d->t[0], t, d->t[2], d->t[3]);
            }
            info->idx -= len(d->t[1]);
            if (info->idx < len(d->t[2]))
            {
                t = tree_insert(d->t[2], info);
                if (t == d->t[2]) goto fast_4;
                if (info->next != nullptr)
                {
                    e = dig(d->t[0], d->t[1], t);
                    info->next = tree(info->next, d->t[3]);
                    return e;
                }
                return dig(d->t[0], d->t[1], t, d->t[3]);
            }
            info->idx -= len(d->t[2]);
            t = tree_insert(d->t[3], info);
            if (t == d->t[3]) goto fast_4;
            if (info->next != nullptr)
            {
                e = dig(d->t[0], d->t[1], d->t[2]);
                info->next = tree(t, info->next);
                return e;
            }
            return dig(d->t[0], d->t[1], d->t[2], t);
        fast_4:
            f->len = d->t[0]->len + d->t[1]->len + d->t[2]->len + d->t[3]->len;
            f->monoid = M(f);
            return f;
    }
    LIBFPP_UNREACHABLE();
}

template <typename S, typename M>
static const DIG<S, M> *dig_insert_right(const DIG<S, M> *d, InsertInfo<S, M> *info)
{
    const TREE<S, M> *t;
    const DIG<S, M> *e;
    auto *f = const_cast<DIG<S, M> *>(d);
    info->refcount |= d->refcount;
    switch (d->tag)
    {
        case DIG<S, M>::DIG_1: case DIG<S, M>::DIG_2: case DIG<S, M>::DIG_3:
            return dig_insert_left(d, info);
        case DIG<S, M>::DIG_4:
            if (d->t[0]->tag == TREE<S, M>::ELEMENT)
            {
                if (info->idx < len(d->t[0]) + len(d->t[1]))
                {
                    auto r = info->insert(d->t[0], d->t[1], info->idx, info->data, info->refcount);
                    if (r.a == d->t[0] && r.b == d->t[1]) goto fast_4;
                    info->next = (r.c == nullptr? nullptr: tree(r.a, r.b));
                    e = (r.c == nullptr? dig(r.a, r.b, d->t[2], d->t[3]): dig(r.c, d->t[2], d->t[3]));
                    return e;
                }
                info->idx -= len(d->t[0]) + len(d->t[1]);
                auto r = info->insert(d->t[2], d->t[3], info->idx, info->data, info->refcount);
                if (r.a == d->t[2] && r.b == d->t[3]) goto fast_4;
                info->next = (r.c == nullptr? nullptr: tree(d->t[0], d->t[1]));
                e = (r.c == nullptr? dig(d->t[0], d->t[1], r.a, r.b): dig(r.a, r.b, r.c));
                return e;
            }
            if (info->idx < len(d->t[0]))
            {
                t = tree_insert(d->t[0], info);
                if (t == d->t[0]) goto fast_4;
                if (info->next != nullptr)
                {
                    e = dig(d->t[2], d->t[3]);
                    info->next = tree(t, info->next, d->t[1]);
                    return e;
                }
                return dig(t, d->t[1], d->t[2], d->t[3]);
            }
            info->idx -= len(d->t[0]);
            if (info->idx < len(d->t[1]))
            {
                t = tree_insert(d->t[1], info);
                if (t == d->t[1]) goto fast_4;
                if (info->next != nullptr)
                {
                    e = dig(d->t[2], d->t[3]);
                    info->next = tree(d->t[0], t, info->next);
                    return e;
                }
                return dig(d->t[0], t, d->t[2], d->t[3]);
            }
            info->idx -= len(d->t[1]);
            if (info->idx < len(d->t[2]))
            {
                t = tree_insert(d->t[2], info);
                if (t == d->t[2]) goto fast_4;
                if (info->next != nullptr)
                {
                    e = dig(info->next, d->t[3]);
                    info->next = tree(d->t[0], d->t[1], t);
                    return e;
                }
                return dig(d->t[0], d->t[1], t, d->t[3]);
            }
            info->idx -= len(d->t[2]);
            t = tree_insert(d->t[3], info);
            if (t == d->t[3]) goto fast_4;
            if (info->next != nullptr)
            {
                e = dig(t, info->next);
                info->next = tree(d->t[0], d->t[1], d->t[2]);
                return e;
            }
            return dig(d->t[0], d->t[1], d->t[2], t);
        fast_4:
            f->len = d->t[0]->len + d->t[1]->len + d->t[2]->len + d->t[3]->len;
            f->monoid = M(f);
            return f;
    }
    LIBFPP_UNREACHABLE();
}

template <typename S, typename M = EMPTY>
static const SEQ<S, M> *seq_insert(const SEQ<S, M> *s, InsertInfo<S, M> *info)
{
    const TREE<S, M> *t;
    const DIG<S, M> *d;
    auto *r = const_cast<SEQ<S, M> *>(s);
    info->refcount |= s->refcount;
    switch (s->tag)
    {
        case SEQ<S, M>::NIL:
        {
            auto r = info->insert(nullptr, nullptr, info->idx, info->data, 0);
            return single(r.a);
        }
        case SEQ<S, M>::SINGLE:
            t = tree_insert(s->t, info);
            if (t == s->t)
            {
                r->len = t->len;
                r->monoid = M(r);
                return r;
            }
            return (info->next != nullptr? deep(dig(t), nil<S, M>(), dig(info->next)): single(t));
        case SEQ<S, M>::DEEP:
            if (info->idx < len(s->l))
            {
                d = dig_insert_left(s->l, info);
                if (d == s->l) goto fast;
                if (info->next != nullptr)
                {
                    const auto *m = seq_push_front(s->m, info->next);
                    return deep(d, m, s->r);
                }
                return deep(d, s->m, s->r);
            }
            info->idx -= len(s->l);
            if (info->idx < len(s->m))
            {
                const auto *m = seq_insert(s->m, info);
                if (m == s->m) goto fast;
                return deep(s->l, m, s->r);
            }
            info->idx -= len(s->m);
            d = dig_insert_right(s->r, info);
            if (d == s->r) goto fast;
            if (info->next != nullptr)
            {
                const auto *m = seq_push_back(s->m, info->next);
                return deep(s->l, m, d);
            }
            return deep(s->l, s->m, d);
        fast:
            r->len = r->l->len + r->m->len + r->r->len;
            r->monoid = M(r);
            return r;
    }
    LIBFPP_UNREACHABLE();
}

template <typename S>
static result<const UTF8_TREE<S> *, const UTF8_TREE<S> *, const UTF8_TREE<S> *> utf8_tree_insert(const UTF8_TREE<S> *t, const UTF8_TREE<S> *u, size_t idx, const void *ptr, size_t refcount)
{
    char32_t c = *reinterpret_cast<const char32_t *>(ptr);
    size_t clen = utf8_encode_len(c);
    if (LIBFPP_UNLIKELY(t == nullptr))
    {
        UTF8_TREE<S> *v = S::template allocate<UTF8_TREE<S>>(sizeof(unsigned char));
        utf8_encode(v->str, c);
        v->len = clen;
        v->monoid.len = 1;
        return {v, nullptr, nullptr};
    }
    bool first = u == nullptr || idx < t->monoid.len;
    const UTF8_TREE<S> *v = (first? t: u);
    refcount |= v->refcount;
    bool full = (v->len + clen > UTF8_TREE<S>::STR_SIZE);
    if (LIBFPP_LIKELY(refcount == 1 && !full))
    {   // FAST PATH:
        auto *w = const_cast<UTF8_TREE<S> *>(v);
        idx -= (first? 0: t->monoid.len);
        char *dptr = w->str;
        for (size_t i = 0; i < idx; i++)
            dptr += utf8_decode_len(dptr);
        memmove(dptr + clen, dptr, v->len - (dptr - w->str));
        utf8_encode(dptr, c);
        w->len += clen;
        w->monoid.len++;
        return {t, u, nullptr};
    }
    if (LIBFPP_UNLIKELY(full))
    {   // SLOW PATH (REBALANCE):
        UTF8_TREE<S> *a = S::template allocate<UTF8_TREE<S>>(sizeof(unsigned char));
        UTF8_TREE<S> *b = S::template allocate<UTF8_TREE<S>>(sizeof(unsigned char));
        UTF8_TREE<S> *d = nullptr;
        size_t mid, len;
        if (u != nullptr && t->len + u->len + /*slack=*/12 >= 2 * UTF8_TREE<S>::STR_SIZE)
        {
            len = t->len + u->len;
            mid = len / 3;
            d = S::template allocate<UTF8_TREE<S>>(sizeof(unsigned char));
        }
        else
        {
            len = t->len + (u == nullptr? 0: u->len);
            mid = len / 2;
        }
        size_t i = 0, j, k = 0;
        char *cptr = a->str;
        const char *dptr = t->str, *dend = t->str + t->len;
        for (j = 0; size_t(cptr - a->str) < mid; i++, j++)
        {
            if (i == idx)     cptr = utf8_encode(cptr, c), j++;
            if (dptr >= dend) dptr = u->str, dend = u->str + u->len;
            cptr = utf8_encode(cptr, utf8_decode(dptr));
            size_t dlen = utf8_decode_len(dptr);
            dptr += dlen, k += dlen;
        }
        a->len = cptr - a->str;
        a->monoid.len = j;
        cptr = b->str;
        for (j = 0; (d == nullptr? k < len: size_t(cptr - b->str) < mid); i++, j++)
        {
            if (i == idx)     cptr = utf8_encode(cptr, c), j++;
            if (dptr >= dend) dptr = u->str, dend = u->str + u->len;
            cptr = utf8_encode(cptr, utf8_decode(dptr));
            size_t dlen = utf8_decode_len(dptr);
            dptr += dlen, k += dlen;
        }
        if (d == nullptr && i == idx)
            cptr = utf8_encode(cptr, c), j++;
        b->len = cptr - b->str;
        b->monoid.len = j;
        if (d != nullptr)
        {
            cptr = d->str;
            for (j = 0; k < len; i++, j++)
            {
                if (i == idx)     cptr = utf8_encode(cptr, c), j++;
                if (dptr >= dend) dptr = u->str, dend = u->str + u->len;
                cptr = utf8_encode(cptr, utf8_decode(dptr));
                size_t dlen = utf8_decode_len(dptr);
                dptr += dlen, k += dlen;
            }
            if (i == idx)
                cptr = utf8_encode(cptr, c), j++;
            d->len = cptr - d->str;
            d->monoid.len = j;
        }
        return {a, b, d};
    }
    UTF8_TREE<S> *w = S::template allocate<UTF8_TREE<S>>(sizeof(unsigned char));
    size_t i;
    char *cptr = w->str;
    const char *dptr = v->str;
    idx -= (first? 0: t->monoid.len);
    for (i = 0; i < idx; i++)
    {
        cptr = utf8_encode(cptr, utf8_decode(dptr));
        dptr += utf8_decode_len(dptr);
    }
    cptr = utf8_encode(cptr, c);
    for (; i < v->monoid.len; i++)
    {
        cptr = utf8_encode(cptr, utf8_decode(dptr));
        dptr += utf8_decode_len(dptr);
    }
    w->len = cptr - w->str;
    w->monoid.len = v->monoid.len + 1;
    if (first)
        return {w, u, nullptr};
    else
        return {t, w, nullptr};
}

template <typename S>
static result<const UTF8_TREE<S> *, const UTF8_TREE<S> *, const UTF8_TREE<S> *> utf8_tree_assign(const UTF8_TREE<S> *t, const UTF8_TREE<S> *u, size_t idx, const void *ptr, size_t refcount)
{
    char32_t c = *reinterpret_cast<const char32_t *>(ptr);
    size_t clen = utf8_encode_len(c);
    bool first = (u == nullptr || idx < t->monoid.len);
    const UTF8_TREE<S> *v = (u == nullptr || idx < t->monoid.len? t: u);
    refcount |= v->refcount;
    bool full = (v->len - 1 + clen > UTF8_TREE<S>::STR_SIZE);
    if (LIBFPP_LIKELY(refcount == 1 && !full))
    {   // FAST PATH:
        auto *w = const_cast<UTF8_TREE<S> *>(v);
        idx -= (first? 0: t->monoid.len);
        char *dptr = w->str;
        for (size_t i = 0; i < idx; i++)
            dptr += utf8_decode_len(dptr);
        char *dend = dptr + utf8_decode_len(dptr);
        memmove(dptr + clen, dend, v->len - (dend - w->str));
        utf8_encode(dptr, c);
        w->len += clen;
        w->len -= (dend - dptr);
        return {t, u, nullptr};
    }
    if (LIBFPP_UNLIKELY(full))
    {   // SLOW PATH (REBALANCE):
        UTF8_TREE<S> *a = S::template allocate<UTF8_TREE<S>>(sizeof(unsigned char));
        UTF8_TREE<S> *b = S::template allocate<UTF8_TREE<S>>(sizeof(unsigned char));
        UTF8_TREE<S> *d = nullptr;
        size_t mid, len;
        if (u != nullptr && t->len + u->len + /*slack=*/8 >= 2 * UTF8_TREE<S>::STR_SIZE)
        {
            len = t->len + u->len;
            mid = len / 3;
            d = S::template allocate<UTF8_TREE<S>>(sizeof(unsigned char));
        }
        else
        {
            len = t->len + (u == nullptr? 0: u->len);
            mid = len / 2;
        }
        size_t i = 0, j, k = 0;
        char *cptr = a->str;
        const char *dptr = t->str, *dend = t->str + t->len;
        for (j = 0; size_t(cptr - a->str) < mid; i++, j++)
        {
            if (dptr >= dend) dptr = u->str, dend = u->str + u->len;
            if (i == idx)     cptr = utf8_encode(cptr, c);
            else              cptr = utf8_encode(cptr, utf8_decode(dptr));
            size_t dlen = utf8_decode_len(dptr);
            dptr += dlen, k += dlen;
        }
        a->len = cptr - a->str;
        a->monoid.len = j;
        cptr = b->str;
        for (j = 0; (d == nullptr? k < len: size_t(cptr - b->str) < mid); i++, j++)
        {
            if (dptr >= dend) dptr = u->str, dend = u->str + u->len;
            if (i == idx)     cptr = utf8_encode(cptr, c);
            else              cptr = utf8_encode(cptr, utf8_decode(dptr));
            size_t dlen = utf8_decode_len(dptr);
            dptr += dlen, k += dlen;
        }
        b->len = cptr - b->str;
        b->monoid.len = j;
        if (d != nullptr)
        {
            cptr = d->str;
            for (j = 0; k < len; i++, j++)
            {
                if (dptr >= dend) dptr = u->str, dend = u->str + u->len;
                if (i == idx)     cptr = utf8_encode(cptr, c);
                else              cptr = utf8_encode(cptr, utf8_decode(dptr));
                size_t dlen = utf8_decode_len(dptr);
                dptr += dlen, k += dlen;
            }
            d->len = cptr - d->str;
            d->monoid.len = j;
        }
        return {a, b, d};
    }

    UTF8_TREE<S> *w = S::template allocate<UTF8_TREE<S>>(sizeof(unsigned char));
    size_t i;
    char *cptr = w->str;
    const char *dptr = v->str;
    idx -= (first? 0: t->monoid.len);
    for (i = 0; i < idx; i++)
    {
        cptr = utf8_encode(cptr, utf8_decode(dptr));
        dptr += utf8_decode_len(dptr);
    }
    cptr = utf8_encode(cptr, c);
    dptr += utf8_decode_len(dptr);
    for (i++; i < v->monoid.len; i++)
    {
        cptr = utf8_encode(cptr, utf8_decode(dptr));
        dptr += utf8_decode_len(dptr);
    }
    w->len = cptr - w->str;
    w->monoid.len = v->monoid.len;
    if (first)
        return {w, u, nullptr};
    else
        return {t, w, nullptr};
}

template <typename S>
const SEQ<S> *INTERNAL<S>::seq_insert(const SEQ<S> *s, size_t idx, const void *data, result<const TREE<S> *, const TREE<S> *, const TREE<S> *> (*insert)(const TREE<S> *, const TREE<S> *, size_t, const void *, size_t))
{
    if (idx > s->len) LIBFPP_PANIC("index out-of-bounds");
    InsertInfo<S> info;
    info.refcount = 0;
    info.idx      = idx;
    info.data     = data;
    info.next     = nullptr;
    info.insert   = insert;
    return F::seq_insert(s, &info);
}
template <typename S>
const UTF8_SEQ<S> *INTERNAL<S>::str_insert(const UTF8_SEQ<S> *s, size_t idx, char32_t c)
{
    if (idx > len(s)) LIBFPP_PANIC("index out-of-bounds");
    InsertInfo<S, STR> info;
    info.refcount = 0;
    info.idx      = idx;
    info.data     = &c;
    info.next     = nullptr;
    info.insert   = utf8_tree_insert;
    return F::seq_insert(s, &info);
}
template <typename S>
const UTF8_SEQ<S> *INTERNAL<S>::str_assign(const UTF8_SEQ<S> *s, size_t idx, char32_t c)
{
    if (idx >= len(s)) LIBFPP_PANIC("index out-of-bounds");
    InsertInfo<S, STR> info;
    info.refcount = 0;
    info.idx      = idx;
    info.data     = &c;
    info.next     = nullptr;
    info.insert   = utf8_tree_assign;
    return F::seq_insert(s, &info);
}
template <typename S>
const UTF8_SEQ<S> *INTERNAL<S>::str_insert(const UTF8_SEQ<S> *s, size_t idx, const char *cstr)
{
    auto r1 = str_split(s, idx);
    const auto *r2 = str_push_back(r1.a, cstr, nullptr);
    const auto *r3 = str_append(r2, r1.b);
    GC(r3, r1.a, r1.b, r2);
    return r3;
}
template <typename S>
const UTF8_SEQ<S> *INTERNAL<S>::str_insert(const UTF8_SEQ<S> *s, size_t idx, const UTF8_SEQ<S> *t)
{
    auto r1 = str_split(s, idx);
    const auto *r2 = str_append(r1.a, t);
    const auto *r3 = str_append(r2, r1.b);
    GC(r3, r1.a, r1.b, r2);
    return r3;
}

template <typename S, typename M = EMPTY>
struct EraseInfo
{
    size_t idx;
    bool reduced, left;
    const TREE<S, M> *orphan;
    result<const TREE<S, M> *, const TREE<S, M> *> (*erase)(const TREE<S, M> *, const TREE<S, M> *, size_t);
};

template <typename S, typename M = EMPTY>
static LIBFPP_NOINLINE const TREE<S, M> *tree_fix_2(const TREE<S, M> *t, size_t idx, const TREE<S, M> *u, bool *reduced, const void *max = nullptr)
{
    const TREE<S, M> *v;
    switch (idx)
    {
        case 0:
            if (u == nullptr) return t->t[1];
            v = t->t[1];
            switch (v->tag)
            {
                case TREE<S, M>::TREE_2:
                    return tree(u, v->t[0], v->t[1], max);
                case TREE<S, M>::TREE_3:
                    *reduced = false;
                    return tree(tree(u, v->t[0], max), tree(v->t[1], v->t[2], max), max);
                default:
                    LIBFPP_UNREACHABLE();
            }
        case 1:
            if (u == nullptr) return t->t[0];
            v = t->t[0];
            switch (v->tag)
            {
                case TREE<S, M>::TREE_2:
                    return tree(v->t[0], v->t[1], u, max);
                case TREE<S, M>::TREE_3:
                    *reduced = false;
                    return tree(tree(v->t[0], v->t[1], max), tree(v->t[2], u, max), max);
                default:
                    LIBFPP_UNREACHABLE();
            }
    }
    LIBFPP_UNREACHABLE();
}

template <typename S, typename M = EMPTY>
static LIBFPP_NOINLINE const TREE<S, M> *tree_fix_3(const TREE<S, M> *t, size_t idx, const TREE<S, M> *u, bool *reduced, const void *max = nullptr)
{
    *reduced = false;
    const TREE<S, M> *v;
    switch (idx)
    {
        case 0:
            if (u == nullptr) return tree(t->t[1], t->t[2], max);
            v = t->t[1];
            switch (v->tag)
            {
                case TREE<S, M>::TREE_2:
                    return tree(tree(u, v->t[0], v->t[1], max), t->t[2], max);
                case TREE<S, M>::TREE_3:
                    return tree(tree(u, v->t[0], max), tree(v->t[1], v->t[2], max), t->t[2], max);
                default:
                    LIBFPP_UNREACHABLE();
            }
        case 1:
            if (u == nullptr) return tree(t->t[0], t->t[2], max);
            v = t->t[0];
            switch (v->tag)
            {
                case TREE<S, M>::TREE_2:
                    return tree(tree(v->t[0], v->t[1], u, max), t->t[2], max);
                case TREE<S, M>::TREE_3:
                    return tree(tree(v->t[0], v->t[1], max), tree(v->t[2], u, max), t->t[2], max);
                default:
                    LIBFPP_UNREACHABLE();
            }
        case 2:
            if (u == nullptr) return tree(t->t[0], t->t[1], max);
            v = t->t[1];
            switch (v->tag)
            {
                case TREE<S, M>::TREE_2:
                    return tree(t->t[0], tree(v->t[0], v->t[1], u, max), max);
                case TREE<S, M>::TREE_3:
                    return tree(t->t[0], tree(v->t[0], v->t[1], max), tree(v->t[2], u, max), max);
                default:
                    LIBFPP_UNREACHABLE();
            }
    }
    LIBFPP_UNREACHABLE();
}

template <typename S, typename M = EMPTY>
static LIBFPP_NOINLINE const DIG<S, M> *dig_fix_2(const DIG<S, M> *d, size_t idx, const TREE<S, M> *u, bool *reduced, const void *max = nullptr)
{
    *reduced = false;
    const TREE<S, M> *v;
    switch (idx)
    {
        case 0:
            if (u == nullptr) return dig(d->t[1], max);
            v = d->t[1];
            switch (v->tag)
            {
                case TREE<S, M>::TREE_2:
                    return dig(tree(u, v->t[0], v->t[1], max), max);
                case TREE<S, M>::TREE_3:
                    return dig(tree(u, v->t[0], max), tree(v->t[1], v->t[2], max), max);
                default:
                    LIBFPP_UNREACHABLE();
            }
        case 1:
            if (u == nullptr) return dig(d->t[0]);
            v = d->t[0];
            switch (v->tag)
            {
                case TREE<S, M>::TREE_2:
                    return dig(tree(v->t[0], v->t[1], u, max), max);
                case TREE<S, M>::TREE_3:
                    return dig(tree(v->t[0], v->t[1], max), tree(v->t[2], u, max), max);
                default:
                    LIBFPP_UNREACHABLE();
            }
    }
    LIBFPP_UNREACHABLE();
}

template <typename S, typename M = EMPTY>
static LIBFPP_NOINLINE const DIG<S, M> *dig_fix_3(const DIG<S, M> *d, size_t idx, const TREE<S, M> *u, bool *reduced, const void *max = nullptr)
{
    *reduced = false;
    const TREE<S, M> *v;
    switch (idx)
    {
        case 0:
            if (u == nullptr) return dig(d->t[1], d->t[2], max);
            v = d->t[1];
            switch (v->tag)
            {
                case TREE<S, M>::TREE_2:
                    return dig(tree(u, v->t[0], v->t[1], max), d->t[2], max);
                case TREE<S, M>::TREE_3:
                    return dig(tree(u, v->t[0], max), tree(v->t[1], v->t[2], max), d->t[2], max);
                default:
                    LIBFPP_UNREACHABLE();
            }
        case 1:
            if (u == nullptr) return dig(d->t[0], d->t[2], max);
            v = d->t[2];
            switch (v->tag)
            {
                case TREE<S, M>::TREE_2:
                    return dig(d->t[0], tree(u, v->t[0], v->t[1], max), max);
                case TREE<S, M>::TREE_3:
                    return dig(d->t[0], tree(u, v->t[0], max), tree(v->t[1], v->t[2], max), max);
                default:
                    LIBFPP_UNREACHABLE();
            }
        case 2:
            if (u == nullptr) return dig(d->t[0], d->t[1], max);
            v = d->t[1];
            switch (v->tag)
            {
                case TREE<S, M>::TREE_2:
                    return dig(d->t[0], tree(v->t[0], v->t[1], u, max), max);
                case TREE<S, M>::TREE_3:
                    return dig(d->t[0], tree(v->t[0], v->t[1], max), tree(v->t[2], u, max), max);
                default:
                    LIBFPP_UNREACHABLE();
            }
    }
    LIBFPP_UNREACHABLE();
}

template <typename S, typename M = EMPTY>
static LIBFPP_NOINLINE const DIG<S, M> *dig_fix_4(const DIG<S, M> *d, size_t idx, const TREE<S, M> *u, bool *reduced, const void *max = nullptr)
{
    *reduced = false;
    const TREE<S, M> *v;
    switch (idx)
    {
        case 0:
            if (u == nullptr) return dig(d->t[1], d->t[2], d->t[3], max);
            v = d->t[1];
            switch (v->tag)
            {
                case TREE<S, M>::TREE_2:
                    return dig(tree(u, v->t[0], v->t[1], max), d->t[2], d->t[3], max);
                case TREE<S, M>::TREE_3:
                    return dig(tree(u, v->t[0], max), tree(v->t[1], v->t[2], max), d->t[2], d->t[3], max);
                default:
                    LIBFPP_UNREACHABLE();
            }
        case 1:
            if (u == nullptr) return dig(d->t[0], d->t[2], d->t[3], max);
            v = d->t[2];
            switch (v->tag)
            {
                case TREE<S, M>::TREE_2:
                    return dig(d->t[0], tree(u, v->t[0], v->t[1], max), d->t[3], max);
                case TREE<S, M>::TREE_3:
                    return dig(d->t[0], tree(u, v->t[0], max), tree(v->t[1], v->t[2], max), d->t[3], max);
                default:
                    LIBFPP_UNREACHABLE();
            }
        case 2:
            if (u == nullptr) return dig(d->t[0], d->t[1], d->t[3], max);
            v = d->t[3];
            switch (v->tag)
            {
                case TREE<S, M>::TREE_2:
                    return dig(d->t[0], d->t[1], tree(u, v->t[0], v->t[1], max), max);
                case TREE<S, M>::TREE_3:
                    return dig(d->t[0], d->t[1], tree(u, v->t[0], max), tree(v->t[1], v->t[2], max), max);
                default:
                    LIBFPP_UNREACHABLE();
            }
        case 3:
            if (u == nullptr) return dig(d->t[0], d->t[1], d->t[2], max);
            v = d->t[2];
            switch (v->tag)
            {
                case TREE<S, M>::TREE_2:
                    return dig(d->t[0], d->t[1], tree(v->t[0], v->t[1], u, max), max);
                case TREE<S, M>::TREE_3:
                    return dig(d->t[0], d->t[1], tree(v->t[0], v->t[1], max), tree(v->t[2], u, max), max);
                default:
                    LIBFPP_UNREACHABLE();
            }
    }
    LIBFPP_UNREACHABLE();
}

template <typename S, typename M = EMPTY>
static const TREE<S, M> *tree_erase(const TREE<S, M> *t, EraseInfo<S, M> *info)
{
    const TREE<S, M> *u;
    switch (t->tag)
    {
        case TREE<S, M>::ELEMENT:
        {
            auto r = info->erase(t, nullptr, info->idx);
            info->reduced = (r.a == nullptr);
            return r.a;
        }
        case TREE<S, M>::TREE_2:
            if (t->t[0]->tag == TREE<S, M>::ELEMENT)
            {
                auto r = info->erase(t->t[0], t->t[1], info->idx);
                info->reduced = (r.b == nullptr);
                return (info->reduced? r.a: tree(r.a, r.b));
            }
            if (info->idx < len(t->t[0]))
            {
                u = tree_erase(t->t[0], info);
                return (info->reduced? tree_fix_2(t, 0, u, &info->reduced): tree(u, t->t[1]));
            }
            info->idx -= len(t->t[0]);
            u = tree_erase(t->t[1], info);
            return (info->reduced? tree_fix_2(t, 1, u, &info->reduced): tree(t->t[0], u));
        case TREE<S, M>::TREE_3:
            if (t->t[0]->tag == TREE<S, M>::ELEMENT)
            {
                if (info->idx < len(t->t[0]))
                {
                    auto r = info->erase(t->t[0], t->t[1], info->idx);
                    return (r.b == nullptr? tree(r.a, t->t[2]): tree(r.a, r.b, t->t[2]));
                }
                info->idx -= len(t->t[0]);
                auto r = info->erase(t->t[1], t->t[2], info->idx);
                return (r.b == nullptr? tree(t->t[0], r.a): tree(t->t[0], r.a, r.b));
            }
            if (info->idx < len(t->t[0]))
            {
                u = tree_erase(t->t[0], info);
                return (LIBFPP_UNLIKELY(info->reduced)? tree_fix_3(t, 0, u, &info->reduced): tree(u, t->t[1], t->t[2]));
            }
            info->idx -= len(t->t[0]);
            if (info->idx < len(t->t[1]))
            {
                u = tree_erase(t->t[1], info);
                return (LIBFPP_UNLIKELY(info->reduced)? tree_fix_3(t, 1, u, &info->reduced): tree(t->t[0], u, t->t[2]));
            }
            info->idx -= len(t->t[1]);
            u = tree_erase(t->t[2], info);
            return (LIBFPP_UNLIKELY(info->reduced)? tree_fix_3(t, 2, u, &info->reduced): tree(t->t[0], t->t[1], u));
    }
    LIBFPP_UNREACHABLE();
}

template <typename S, typename M = EMPTY>
static const DIG<S, M> *dig_erase(const DIG<S, M> *d, EraseInfo<S, M> *info)
{
    const TREE<S, M> *u;
    switch (d->tag)
    {
        case DIG<S, M>::DIG_1:
            u = tree_erase(d->t[0], info);
            if (info->reduced)
            {
                info->orphan = u;
                return nullptr;
            }
            return dig(u);
        case DIG<S, M>::DIG_2:
            if (d->t[0]->tag == TREE<S, M>::ELEMENT)
            {
                auto r = info->erase(d->t[0], d->t[1], info->idx);
                return (r.b == nullptr? dig(r.a): dig(r.a, r.b));
            }
            if (info->idx < len(d->t[0]))
            {
                u = tree_erase(d->t[0], info);
                return (LIBFPP_UNLIKELY(info->reduced)? dig_fix_2(d, 0, u, &info->reduced): dig(u, d->t[1]));
            }
            info->idx -= len(d->t[0]);
            u = tree_erase(d->t[1], info);
            return (LIBFPP_UNLIKELY(info->reduced)? dig_fix_2(d, 1, u, &info->reduced): dig(d->t[0], u));
        case DIG<S, M>::DIG_3:
            if (d->t[0]->tag == TREE<S, M>::ELEMENT)
            {
                if (info->idx < len(d->t[0]))
                {
                    auto r = info->erase(d->t[0], d->t[1], info->idx);
                    return (r.b == nullptr? dig(r.a, d->t[2]): dig(r.a, r.b, d->t[2]));
                }
                info->idx -= len(d->t[0]);
                auto r = info->erase(d->t[1], d->t[2], info->idx);
                return (r.b == nullptr? dig(d->t[0], r.a): dig(d->t[0], r.a, r.b));
            }
            if (info->idx < len(d->t[0]))
            {
                u = tree_erase(d->t[0], info);
                return (LIBFPP_UNLIKELY(info->reduced)? dig_fix_3(d, 0, u, &info->reduced): dig(u, d->t[1], d->t[2])); 
            }
            info->idx -= len(d->t[0]);
            if (info->idx < len(d->t[1]))
            {
                u = tree_erase(d->t[1], info);
                return (LIBFPP_UNLIKELY(info->reduced)? dig_fix_3(d, 1, u, &info->reduced): dig(d->t[0], u, d->t[2]));
            }
            info->idx -= len(d->t[1]);
            u = tree_erase(d->t[2], info);
            return (LIBFPP_UNLIKELY(info->reduced)? dig_fix_3(d, 2, u, &info->reduced): dig(d->t[0], d->t[1], u));
        case DIG<S, M>::DIG_4:
            if (d->t[0]->tag == TREE<S, M>::ELEMENT)
            {
                if (info->idx < len(d->t[0]))
                {
                    auto r = info->erase(d->t[0], d->t[1], info->idx);
                    return (r.b == nullptr? dig(r.a, d->t[2], d->t[3]): dig(r.a, r.b, d->t[2], d->t[3]));
                }
                info->idx -= len(d->t[0]);
                if (info->idx < len(d->t[1]))
                {
                    auto r = info->erase(d->t[1], d->t[2], info->idx);
                    return (r.b == nullptr? dig(d->t[0], r.a, d->t[3]): dig(d->t[0], r.a, r.b, d->t[3]));
                }
                info->idx -= len(d->t[1]);
                auto r = info->erase(d->t[2], d->t[3], info->idx);
                return (r.b == nullptr? dig(d->t[0], d->t[1], r.a): dig(d->t[0], d->t[1], r.a, r.b));
            }
            if (info->idx < len(d->t[0]))
            {
                u = tree_erase(d->t[0], info);
                return (LIBFPP_UNLIKELY(info->reduced)? dig_fix_4(d, 0, u, &info->reduced): dig(u, d->t[1], d->t[2], d->t[3]));
            }
            info->idx -= len(d->t[0]);
            if (info->idx < len(d->t[1]))
            {
                u = tree_erase(d->t[1], info);
                return (LIBFPP_UNLIKELY(info->reduced)? dig_fix_4(d, 1, u, &info->reduced): dig(d->t[0], u, d->t[2], d->t[3]));
            }
            info->idx -= len(d->t[1]);
            if (info->idx < len(d->t[2]))
            {
                u = tree_erase(d->t[2], info);
                return (LIBFPP_UNLIKELY(info->reduced)? dig_fix_4(d, 2, u, &info->reduced): dig(d->t[0], d->t[1], u, d->t[3]));
            }
            info->idx -= len(d->t[2]);
            u = tree_erase(d->t[3], info);
            return (LIBFPP_UNLIKELY(info->reduced)? dig_fix_4(d, 3, u, &info->reduced): dig(d->t[0], d->t[1], d->t[2], u));
    }
    LIBFPP_UNREACHABLE();
}

template <typename S, typename M = EMPTY>
static const SEQ<S, M> *seq_erase(const SEQ<S, M> *s, EraseInfo<S, M> *info)
{
    const DIG<S, M> *d;
    const TREE<S, M> *t, *u;
    const SEQ<S, M> *m, *n;
    info->left = false;
    switch (s->tag)
    {
        case SEQ<S, M>::SINGLE:
            t = tree_erase(s->t, info);
            if (info->reduced)
            {
                info->orphan = t;
                return nil<S, M>();
            }
            return single(t);
        case SEQ<S, M>::DEEP:
        {
            if (info->idx < len(s->l))
            {
                d = dig_erase(s->l, info);
                info->left = true;
                return (LIBFPP_UNLIKELY(info->reduced)? deep(s->m, s->r): deep(d, s->m, s->r));
            }
            info->idx -= len(s->l);
            if (info->idx < len(s->m))
            {
                m = seq_erase(s->m, info);
                if (LIBFPP_UNLIKELY(info->reduced && info->left))
                {
                    info->reduced = false;
                    d = s->l;
                    switch (d->tag)
                    {
                        case DIG<S, M>::DIG_1: return deep(dig(d->t[0], info->orphan), m, s->r);
                        case DIG<S, M>::DIG_2: return deep(dig(d->t[0], d->t[1], info->orphan), m, s->r);
                        case DIG<S, M>::DIG_3: return deep(dig(d->t[0], d->t[1], d->t[2], info->orphan), m, s->r);
                        case DIG<S, M>::DIG_4:
                            u = tree(d->t[3], info->orphan);
                            n = seq_push_front(m, u);
                            m->gc();
                            return deep(dig(d->t[0], d->t[1], d->t[2]), n, s->r);
                    }
                    LIBFPP_UNREACHABLE();
                }
                if (LIBFPP_UNLIKELY(info->reduced && !info->left))
                {
                    info->reduced = false;
                    d = s->r;
                    switch (d->tag)
                    {
                        case DIG<S, M>::DIG_1: return deep(s->l, m, dig(info->orphan, d->t[0]));
                        case DIG<S, M>::DIG_2: return deep(s->l, m, dig(info->orphan, d->t[0], d->t[1]));
                        case DIG<S, M>::DIG_3: return deep(s->l, m, dig(info->orphan, d->t[0], d->t[1], d->t[2]));
                        case DIG<S, M>::DIG_4:
                            u = tree(info->orphan, d->t[0]);
                            n = seq_push_back(m, u);
                            m->gc();
                            return deep(s->l, n, dig(d->t[1], d->t[2], d->t[3]));
                    }
                    LIBFPP_UNREACHABLE();
                }
                return deep(s->l, m, s->r);
            }
            info->idx -= len(s->m);
            d = dig_erase(s->r, info);
            auto *tmp = (LIBFPP_UNLIKELY(info->reduced)? deep(s->l, s->m): deep(s->l, s->m, d));
            return tmp;
        }
        default:
            LIBFPP_UNREACHABLE();
    }
}

template <typename S>
static result<const UTF8_TREE<S> *, const UTF8_TREE<S> *> utf8_tree_erase(const UTF8_TREE<S> *t, const UTF8_TREE<S> *u, size_t idx)
{
    if (LIBFPP_UNLIKELY(len(t) == 1 && idx == 0))
        return {u, nullptr};
    if (LIBFPP_UNLIKELY(idx == len(t) && len(u) == 1))
        return {t, nullptr};
    size_t i, j;
    UTF8_TREE<S> *v = S::template allocate<UTF8_TREE<S>>(sizeof(unsigned char));
    char *cptr = v->str;
    if (LIBFPP_UNLIKELY(u == nullptr || t->len + u->len - 1 <= UTF8_TREE<S>::STR_SIZE))
    {
        const char *dptr = t->str;
        for (i = 0, j = 0; i < len(t); i++)
        {
            if (i != idx)
            {
                cptr = utf8_encode(cptr, utf8_decode(dptr));
                j++;
            }
            dptr += utf8_decode_len(dptr);
        }
        v->len = (cptr - v->str);
        v->monoid.len = j;
        if (u == nullptr)
            return {v, nullptr};
        idx -= len(t);
        dptr = u->str;
        for (i = 0; i < len(u); i++)
        {
            if (i != idx)
            {
                cptr = utf8_encode(cptr, utf8_decode(dptr));
                j++;
            }
            dptr += utf8_decode_len(dptr);
        }
        v->len = (cptr - v->str);
        v->monoid.len = j;
        return {v, nullptr};
    }
    if (LIBFPP_LIKELY(idx < len(t)))
    {
        const char *dptr = t->str;
        for (i = 0, j = 0; i < len(t); i++)
        {
            if (i != idx)
            {
                cptr = utf8_encode(cptr, utf8_decode(dptr));
                j++;
            }
            dptr += utf8_decode_len(dptr);
        }
        v->len = (cptr - v->str);
        v->monoid.len = j;
        return {v, u};
    }
    idx -= len(t);
    const char *dptr = u->str;
    for (i = 0, j = 0; i < len(u); i++)
    {
        if (i != idx)
        {
            cptr = utf8_encode(cptr, utf8_decode(dptr));
            j++;
        }
        dptr += utf8_decode_len(dptr);
    }
    v->len = (cptr - v->str);
    v->monoid.len = j;
    return {t, v};
}

template <typename S>
const SEQ<S> *INTERNAL<S>::seq_erase(const SEQ<S> *s, size_t idx, result<const TREE<S> *, const TREE<S> *> (*erase)(const TREE<S> *, const TREE<S> *, size_t))
{
    if (idx >= len(s)) return nullptr;
    EraseInfo<S> info;
    info.idx     = idx;
    info.reduced = false;
    info.erase   = erase;
    return F::seq_erase<S>(s, &info);
}
template <typename S>
const SEQ<S, MIN> *INTERNAL<S>::seq_erase(const SEQ<S, MIN> *s, size_t idx, result<const TREE<S, MIN> *, const TREE<S, MIN> *> (*erase)(const TREE<S, MIN> *, const TREE<S, MIN> *, size_t))
{
    if (idx >= len(s)) return nullptr;
    EraseInfo<S, MIN> info;
    info.idx     = idx;
    info.reduced = false;
    info.erase   = erase;
    return F::seq_erase<S, MIN>(s, &info);
}
template <typename S>
const UTF8_SEQ<S> *INTERNAL<S>::str_erase(const UTF8_SEQ<S> *s, size_t idx)
{
    if (idx >= len(s)) return nullptr;
    EraseInfo<S, STR> info;
    info.idx     = idx;
    info.reduced = false;
    info.erase   = utf8_tree_erase;
    return F::seq_erase(s, &info);
}
template <typename S>
const UTF8_SEQ<S> *INTERNAL<S>::str_erase(const UTF8_SEQ<S> *s, size_t lo, size_t hi)
{
    if (hi > len(s) || lo > len(s)) return nullptr;
    if (lo >= hi) return s;
    if (hi == lo + 1) return INTERNAL<S>::str_erase(s, lo);
    const auto *r1 = str_left(s, lo);
    const auto *r2 = str_right(s, hi);
    const auto *r  = str_append(r1, r2);
    GC(r, r1, r2);
    return r;
}

template <typename S>
const UTF8_SEQ<S> *utf8_pop_back(const UTF8_SEQ<S> *s)
{
    if (LIBFPP_UNLIKELY(s->len == 0)) LIBFPP_PANIC("string is empty");
    auto r = seq_pop_back(s);
    const auto *t = r.b;
    if (LIBFPP_UNLIKELY(t->monoid.len == 1))
        return r.a;
    size_t clen = 1;
    for (; (t->str[t->len - clen] & 0xC0) == 0x80; clen++)
        ;
    UTF8_TREE<S> *u = S::template allocate<UTF8_TREE<S>>(sizeof(unsigned char));
    memcpy(u->str, t->str, t->len - clen);
    u->len = t->len - clen;
    u->monoid.len = t->monoid.len - 1;
    s = seq_push_back(r.a, u);
    r.a->gc();
    return s;
}

template <typename S>
const UTF8_SEQ<S> *INTERNAL<S>::str_pop_back(const UTF8_SEQ<S> *s)
{
    return utf8_pop_back(s);
}

template <typename S>
const UTF8_SEQ<S> *utf8_pop_front(const UTF8_SEQ<S> *s)
{
    if (LIBFPP_UNLIKELY(s->len == 0)) LIBFPP_PANIC("string is empty");
    auto r = seq_pop_front(s);
    const auto *t = r.b;
    if (LIBFPP_UNLIKELY(t->monoid.len == 1))
        return r.a;
    UTF8_TREE<S> *u = S::template allocate<UTF8_TREE<S>>(sizeof(unsigned char));
    size_t clen = utf8_decode_len(t->str);
    memcpy(u->str, t->str + clen, t->len - clen);
    u->len = t->len - clen;
    u->monoid.len = t->monoid.len - 1;
    s = seq_push_front(r.a, u);
    r.a->gc();
    return s;
}

template <typename S>
const UTF8_SEQ<S> *INTERNAL<S>::str_pop_front(const UTF8_SEQ<S> *s)
{
    return utf8_pop_front(s);
}

template <typename S, typename M = EMPTY>
static size_t seq_append_middle(const DIG<S, M> *s, const TREE<S, M> **m, size_t m_len, const DIG<S, M> *t)
{
    const TREE<S, M> *n[12];
    size_t n_len = 0;
    switch (s->tag)
    {
        case DIG<S, M>::DIG_1:
            n[n_len++] = s->t[0];
            break;
        case DIG<S, M>::DIG_2:
            n[n_len++] = s->t[0];
            n[n_len++] = s->t[1];
            break;
        case DIG<S, M>::DIG_3:
            n[n_len++] = s->t[0];
            n[n_len++] = s->t[1];
            n[n_len++] = s->t[2];
            break;
        case DIG<S, M>::DIG_4:
            n[n_len++] = s->t[0];
            n[n_len++] = s->t[1];
            n[n_len++] = s->t[2];
            n[n_len++] = s->t[3];
            break;
    }
    for (size_t i = 0; i < m_len; i++)
        n[n_len++] = m[i];
    switch (t->tag)
    {
        case DIG<S, M>::DIG_1:
            n[n_len++] = t->t[0];
            break;
        case DIG<S, M>::DIG_2:
            n[n_len++] = t->t[0];
            n[n_len++] = t->t[1];
            break;
        case DIG<S, M>::DIG_3:
            n[n_len++] = t->t[0];
            n[n_len++] = t->t[1];
            n[n_len++] = t->t[2];
            break;
        case DIG<S, M>::DIG_4:
            n[n_len++] = t->t[0];
            n[n_len++] = t->t[1];
            n[n_len++] = t->t[2];
            n[n_len++] = t->t[3];
            break;
    }
    m_len = 0;
    size_t i = 0;
    while (true)
    {
        switch (n_len)
        {
            case 2:
                m[m_len++] = tree(n[i], n[i+1]);
                return m_len;
            case 3:
                m[m_len++] = tree(n[i], n[i+1], n[i+2]);
                return m_len;
            case 4:
                m[m_len++] = tree(n[i], n[i+1]);
                m[m_len++] = tree(n[i+2], n[i+3]);
                return m_len;
            case 5:
                // Nb: This case is missing from the paper?
                m[m_len++] = tree(n[i], n[i+1], n[i+2]);
                m[m_len++] = tree(n[i+3], n[i+4]);
                return m_len;
            default:
                m[m_len++] = tree(n[i], n[i+1], n[i+2]);
                n_len -= 3;
                i += 3;
                continue;
        }
    }
    LIBFPP_UNREACHABLE();
}

template <typename S, typename M = EMPTY>
static LIBFPP_NOINLINE const SEQ<S, M> *seq_append(const SEQ<S, M> *s, const TREE<S, M> **m, size_t m_len, const SEQ<S, M> *t)
{
    const SEQ<S, M> *s0 = s, *t0 = t;
    switch (s->tag)
    {
        case SEQ<S, M>::NIL: case SEQ<S, M>::SINGLE:
            for (size_t i = 0; i < m_len; i++)
            {
                const auto *old = t;
                t = seq_push_front(t, m[m_len - i - 1]);
                if (old != t0) old->gc();
            }
            if (s->tag == SEQ<S, M>::SINGLE)
            {
                const auto *old = t;
                t = seq_push_front(t, s->t);
                if (old != t0) old->gc();
                if (s != s0) s->gc();
            }
            return t;
        case SEQ<S, M>::DEEP:
        {
            switch (t->tag)
            {
                case SEQ<S, M>::NIL: case SEQ<S, M>::SINGLE:
                    for (size_t i = 0; i < m_len; i++)
                    {
                        const auto *old = s;
                        s = seq_push_back(s, m[i]);
                        if (old != s0) old->gc();
                    }
                    if (t->tag == SEQ<S, M>::SINGLE)
                    {
                        const auto *old = s;
                        s = seq_push_back(s, t->t);
                        if (old != s0) old->gc();
                        if (t != t0) t->gc();
                    }
                    return s;
                case SEQ<S, M>::DEEP:
                {
                    m_len = seq_append_middle(s->r, m, m_len, t->l);
                    const SEQ<S, M> *u = seq_append(s->m, m, m_len, t->m);
                    return deep(s->l, u, t->r);
                }
            }
        }
    }
    LIBFPP_UNREACHABLE();
}

template <typename S>
const SEQ<S> *INTERNAL<S>::seq_append(const SEQ<S> *s, const SEQ<S> *t)
{
    if (s->len == 0) return t;
    if (t->len == 0) return s;
    if (s->len + t->len > LEN_MAX)
        LIBFPP_PANIC("max length exceeded");
    const TREE<S> *m[4];
    size_t m_len = 0;
    return F::seq_append(s, m, m_len, t);
}
template <typename S>
const UTF8_SEQ<S> *INTERNAL<S>::str_append(const UTF8_SEQ<S> *s, const UTF8_SEQ<S> *t)
{
    if (s->len == 0) return t;
    if (t->len == 0) return s;
    if (s->len + t->len > STR_MAX)
        LIBFPP_PANIC("max length exceeded");
    const UTF8_TREE<S> *m[4];
    size_t m_len = 0;
    return F::seq_append(s, m, m_len, t);
}

template <typename S, typename M = EMPTY>
struct SplitInfo
{
    size_t idx;
    bool l, r;
    struct
    {
        const DIG<S, M> *l;
        const DIG<S, M> *r;
    } d;
    struct
    {
        const SEQ<S, M> *l;
        const SEQ<S, M> *r;
    } s;
    const TREE<S, M> *t;
};

template <typename S, typename M = EMPTY>
static void dig_split(const DIG<S, M> *d, SplitInfo<S, M> *info)
{
    switch (d->tag)
    {
        case DIG<S, M>::DIG_1:
            info->t = d->t[0];
            return;
        case DIG<S, M>::DIG_2:
            if (info->idx < len(d->t[0]))
            {
                info->t = d->t[0];
                if (info->r) info->d.r = dig(d->t[1]);
                return;
            }
            info->idx -= len(d->t[0]);
            info->t = d->t[1];
            if (info->l) info->d.l = dig(d->t[0]);
            return;
        case DIG<S, M>::DIG_3:
            if (info->idx < len(d->t[0]))
            {
                info->t = d->t[0];
                if (info->r) info->d.r = dig(d->t[1], d->t[2]);
                return;
            }
            info->idx -= len(d->t[0]);
            if (info->idx < len(d->t[1]))
            {
                info->t = d->t[1];
                if (info->l) info->d.l = dig(d->t[0]);
                if (info->r) info->d.r = dig(d->t[2]);
                return;
            }
            info->idx -= len(d->t[1]);
            info->t = d->t[2];
            if (info->l) info->d.l = dig(d->t[0], d->t[1]);
            return;
        case DIG<S, M>::DIG_4:
            if (info->idx < len(d->t[0]))
            {
                info->t = d->t[0];
                if (info->r) info->d.r = dig(d->t[1], d->t[2], d->t[3]);
                return;
            }
            info->idx -= len(d->t[0]);
            if (info->idx < len(d->t[1]))
            {
                info->t = d->t[1];
                if (info->l) info->d.l = dig(d->t[0]);
                if (info->r) info->d.r = dig(d->t[2], d->t[3]);
                return;
            }
            info->idx -= len(d->t[1]);
            if (info->idx < len(d->t[2]))
            {
                info->t = d->t[2];
                if (info->l) info->d.l = dig(d->t[0], d->t[1]);
                if (info->r) info->d.r = dig(d->t[3]);
                return;
            }
            info->idx -= len(d->t[2]);
            info->t = d->t[3];
            if (info->l) info->d.l = dig(d->t[0], d->t[1], d->t[2]);
            return;
    }
    LIBFPP_UNREACHABLE();
}

template <typename S, typename M = EMPTY>
static void seq_split(const SEQ<S, M> *s, SplitInfo<S, M> *info)
{
    switch (s->tag)
    {
        case SEQ<S, M>::SINGLE:
            if (info->l) info->s.l = nil<S, M>();
            if (info->r) info->s.r = nil<S, M>();
            info->t = s->t;
            return;
        case SEQ<S, M>::DEEP:
            if (info->idx < len(s->l))
            {
                info->d.r = info->d.l = nullptr;
                dig_split(s->l, info);
                if (info->r) info->s.r = (info->d.r == nullptr? deep(s->m, s->r): deep(info->d.r, s->m, s->r));
                if (info->l) info->s.l = (info->d.l == nullptr? nil<S, M>(): dig_to_seq(info->d.l));
                if (info->d.l != nullptr) info->d.l->gc();
                return;
            }
            info->idx -= len(s->l);
            if (info->idx < len(s->m))
            {
                info->s.r = info->s.l = nullptr;
                seq_split(s->m, info);
                const auto *t = info->t;
                const SEQ<S, M> *sl, *sr;
                switch (t->tag)
                {
                    case TREE<S, M>::TREE_2:
                        if (info->idx < len(t->t[0]))
                        {
                            sl = info->s.l;
                            if (info->l) info->s.l = deep(s->l, sl);
                            if (info->r) info->s.r = deep(dig(t->t[1]), info->s.r, s->r);
                            info->t = t->t[0];
                            if (sl != nullptr) sl->gc();
                            return;
                        }
                        info->idx -= len(t->t[0]);
                        sr = info->s.r;
                        if (info->l) info->s.l = deep(s->l, info->s.l, dig(t->t[0]));
                        if (info->r) info->s.r = deep(sr, s->r);
                        info->t = t->t[1];
                        if (sr != nullptr) sr->gc();
                        return;
                    case TREE<S, M>::TREE_3:
                        if (info->idx < len(t->t[0]))
                        {
                            sl = info->s.l;
                            if (info->l) info->s.l = deep(s->l, sl);
                            if (info->r) info->s.r = deep(dig(t->t[1], t->t[2]), info->s.r, s->r);
                            info->t = t->t[0];
                            if (sl != nullptr) sl->gc();
                            return;
                        }
                        info->idx -= len(t->t[0]);
                        if (info->idx < len(t->t[1]))
                        {
                            if (info->l) info->s.l = deep(s->l, info->s.l, dig(t->t[0]));
                            if (info->r) info->s.r = deep(dig(t->t[2]), info->s.r, s->r);
                            info->t = t->t[1];
                            return;
                        }
                        info->idx -= len(t->t[1]);
                        sr = info->s.r;
                        if (info->l) info->s.l = deep(s->l, info->s.l, dig(t->t[0], t->t[1]));
                        if (info->r) info->s.r = deep(sr, s->r);
                        if (sr != nullptr) sr->gc();
                        info->t = t->t[2];
                        return;
                    default:
                        LIBFPP_UNREACHABLE();
                }
            }
            info->idx -= len(s->m);
            info->d.r = info->d.l = nullptr;
            dig_split(s->r, info);
            if (info->l) info->s.l = (info->d.l == nullptr? deep(s->l, s->m): deep(s->l, s->m, info->d.l));
            if (info->r) info->s.r = (info->d.r == nullptr? nil<S, M>(): dig_to_seq(info->d.r));
            if (info->d.r != nullptr) info->d.r->gc();
            return;
        default:
            LIBFPP_UNREACHABLE();
    }
}

template <typename S>
static const UTF8_TREE<S> *utf8_tree_left(const UTF8_TREE<S> *t, size_t idx)
{
    if (idx == 0)
        return nullptr;
    if (idx >= t->monoid.len)
        return t; 
    UTF8_TREE<S> *u = S::template allocate<UTF8_TREE<S>>(sizeof(unsigned char));
    const char *cptr = t->str;
    if (LIBFPP_LIKELY(t->len == t->monoid.len))
        cptr += idx;
    else
    {
        for (size_t i = 0; i < idx; i++)
            cptr += utf8_decode_len(cptr);
    }
    memcpy(u->str, t->str, cptr - t->str);
    u->len = (cptr - t->str);
    u->monoid.len = idx; 
    return u;
}
template <typename S>
static const UTF8_TREE<S> *utf8_tree_right(const UTF8_TREE<S> *t, size_t idx)
{
    if (idx == 0)
        return t;
    if (idx >= t->monoid.len)
        return nullptr;
    UTF8_TREE<S> *u = S::template allocate<UTF8_TREE<S>>(sizeof(unsigned char));
    const char *cptr = t->str;
    if (LIBFPP_LIKELY(t->len == t->monoid.len))
        cptr += idx;
    else
    {
        for (size_t i = 0; i < idx; i++)
            cptr += utf8_decode_len(cptr);
    }
    size_t len = (cptr - t->str);
    memcpy(u->str, t->str + len, t->len - len);
    u->len = t->len - len;
    u->monoid.len = t->monoid.len - idx;
    return u;
}

template <typename S>
result<const SEQ<S> *, const SEQ<S> *> INTERNAL<S>::seq_split(const SEQ<S> *s, size_t idx, const TREE<S> *(*left)(const TREE<S> *, size_t), const TREE<S> *(*right)(const TREE<S> *, size_t))
{
    auto r = seq_split(reinterpret_cast<const SEQ<S, MIN> *>(s), idx, reinterpret_cast<const TREE<S, MIN> *(*)(const TREE<S, MIN> *, size_t)>(left), reinterpret_cast<const TREE<S, MIN> *(*)(const TREE<S, MIN> *, size_t)>(right));
    return {reinterpret_cast<const SEQ<S> *>(r.a), reinterpret_cast<const SEQ<S> *>(r.b)};
}
template <typename S>
result<const SEQ<S, MIN> *, const SEQ<S, MIN> *> INTERNAL<S>::seq_split(const SEQ<S, MIN> *s, size_t idx, const TREE<S, MIN> *(*left)(const TREE<S, MIN> *, size_t), const TREE<S, MIN> *(*right)(const TREE<S, MIN> *, size_t))
{
    if (idx == 0) return {nil<S, MIN>(), s};
    if (idx == len(s)) return {s, nil<S, MIN>()};
    if (idx > len(s)) LIBFPP_PANIC("index out-of-bounds");
    SplitInfo<S, MIN> info;
    info.idx = idx;
    info.l = info.r = true;
    F::seq_split(s, &info);
    const auto *u = left(info.t, info.idx);
    const auto *v = right(info.t, info.idx);
    const auto *l = info.s.l;
    const auto *r = info.s.r;
    if (u != nullptr)
    {
        const auto *tmp = F::seq_push_back(l, u);
        GC(tmp, l);
        l = tmp;
    }
    if (v != nullptr)
    {
        const auto *tmp = F::seq_push_front(r, v);
        GC(tmp, r);
        r = tmp;
    }
    return {l, r};
}
template <typename S>
result<const UTF8_SEQ<S> *, const UTF8_SEQ<S> *> INTERNAL<S>::str_split(const UTF8_SEQ<S> *s, size_t idx)
{
    if (idx == 0) return {nil<S, STR>(), s};
    if (idx == len(s)) return {s, nil<S, STR>()};
    if (idx > len(s)) LIBFPP_PANIC("index out-of-bounds");
    SplitInfo<S, STR> info;
    info.idx = idx;
    info.l = info.r = true;
    F::seq_split(s, &info);
    const auto *u = utf8_tree_left(info.t, info.idx);
    const auto *v = utf8_tree_right(info.t, info.idx);
    const auto *l = info.s.l;
    const auto *r = info.s.r;
    if (u != nullptr)
    {
        const auto *tmp = F::seq_push_back(l, u);
        GC(tmp, l);
        l = tmp;
    }
    if (v != nullptr)
    {
        const auto *tmp = F::seq_push_front(r, v);
        GC(tmp, r);
        r = tmp;
    }
    return {l, r};
}
template <typename S>
const SEQ<S> *INTERNAL<S>::seq_left(const SEQ<S> *s, size_t idx, const TREE<S> *(*left)(const TREE<S> *, size_t))
{
    const auto *r = seq_left(reinterpret_cast<const SEQ<S, MIN> *>(s), idx, reinterpret_cast<const TREE<S, MIN> *(*)(const TREE<S, MIN> *, size_t)>(left));
    return reinterpret_cast<const SEQ<S> *>(r);
}
template <typename S>
const SEQ<S, MIN> *INTERNAL<S>::seq_left(const SEQ<S, MIN> *s, size_t idx, const TREE<S, MIN> *(*left)(const TREE<S, MIN> *, size_t))
{
    if (idx == 0) return nil<S, MIN>();
    if (idx == len(s)) return s;
    if (idx > len(s)) LIBFPP_PANIC("index out-of-bounds");
    SplitInfo<S, MIN> info;
    info.idx = idx;
    info.l = true;
    info.r = false;
    F::seq_split(s, &info);
    const auto *u = left(info.t, info.idx);
    const auto *l = info.s.l;
    if (u != nullptr)
    {
        const auto *tmp = F::seq_push_back(l, u);
        GC(tmp, l);
        l = tmp;
    }
    return l;
}
template <typename S>
const UTF8_SEQ<S> *INTERNAL<S>::str_left(const UTF8_SEQ<S> *s, size_t idx)
{
    if (idx == 0) return nil<S, STR>();
    if (idx == len(s)) return s;
    if (idx > len(s)) LIBFPP_PANIC("index out-of-bounds");
    SplitInfo<S, STR> info;
    info.idx = idx;
    info.l = true;
    info.r = false;
    F::seq_split(s, &info);
    const auto *u = utf8_tree_left(info.t, info.idx);
    const auto *l = info.s.l;
    if (u != nullptr)
    {
        const auto *tmp = F::seq_push_back(l, u);
        GC(tmp, l);
        l = tmp;
    }
    return l;
}
template <typename S>
const SEQ<S> *INTERNAL<S>::seq_right(const SEQ<S> *s, size_t idx, const TREE<S> *(*right)(const TREE<S> *, size_t))
{
    const auto *r = seq_right(reinterpret_cast<const SEQ<S, MIN> *>(s), idx, reinterpret_cast<const TREE<S, MIN> *(*)(const TREE<S, MIN> *, size_t)>(right));
    return reinterpret_cast<const SEQ<S> *>(r);
}
template <typename S>
const SEQ<S, MIN> *INTERNAL<S>::seq_right(const SEQ<S, MIN> *s, size_t idx, const TREE<S, MIN> *(*right)(const TREE<S, MIN> *, size_t))
{
    if (idx == 0) return s;
    if (idx == len(s)) return nil<S, MIN>();
    if (idx > len(s)) LIBFPP_PANIC("index out-of-bounds");
    SplitInfo<S, MIN> info;
    info.idx = idx;
    info.l = false;
    info.r = true;
    F::seq_split(s, &info);
    const auto *u = right(info.t, info.idx);
    const auto *r = info.s.r;
    if (u != nullptr)
    {
        const auto *tmp = F::seq_push_front(r, u);
        GC(tmp, r);
        r = tmp;
    }
    return r;
}
template <typename S>
const UTF8_SEQ<S> *INTERNAL<S>::str_right(const UTF8_SEQ<S> *s, size_t idx)
{
    if (idx == 0) return s;
    if (idx == len(s)) return nil<S, STR>();
    if (idx > len(s)) LIBFPP_PANIC("index out-of-bounds");
    SplitInfo<S, STR> info;
    info.idx = idx;
    info.l = false;
    info.r = true;
    F::seq_split(s, &info);
    const auto *u = utf8_tree_right(info.t, info.idx);
    const auto *r = info.s.r;
    if (u != nullptr)
    {
        const auto *tmp = F::seq_push_front(r, u);
        GC(tmp, r);
        r = tmp;
    }
    return r;
}

struct SearchInfo
{
    const void *key;
    order (*compare)(const void *, const void *);

    bool go_right(const TREE<LOCAL, MIN> *t) const
    {
        return (compare(key, MIN::get_min(t)) >= 0);
    }
    bool go_right(const DIG<LOCAL, MIN> *d) const
    {
        return (compare(key, MIN::get_min(d)) >= 0);
    }
    bool go_right(const SEQ<LOCAL, MIN> *s) const
    {
        if (s->tag == SEQ<LOCAL, MIN>::NIL) return false;
        return (compare(key, MIN::get_min(s)) >= 0);
    }
};

static const void *tree_search(const TREE<LOCAL, MIN> *t, SearchInfo *info)
{
    switch (t->tag)
    {
        case TREE<LOCAL, MIN>::ELEMENT:
        {
            ssize_t lo = 0, hi = t->len - 1;
            while (lo <= hi)
            {
                ssize_t mid = lo + (hi - lo) / 2;
                const void *elem = t->at(mid);
                order cmp = info->compare(info->key, elem);
                if (cmp < 0)
                    hi = mid - 1;
                else if (cmp > 0)
                    lo = mid + 1;
                else
                    return elem;
            }
            return nullptr;
        }
        case TREE<LOCAL, MIN>::TREE_3:
            if (info->go_right(t->t[2]))
                return tree_search(t->t[2], info);
            /* Fallthrough */
        case TREE<LOCAL, MIN>::TREE_2:
            return (info->go_right(t->t[1])? tree_search(t->t[1], info): tree_search(t->t[0], info));
    }
    LIBFPP_UNREACHABLE();
}
static const void *dig_search(const DIG<LOCAL, MIN> *d, SearchInfo *info)
{
    switch (d->tag)
    {
        case DIG<LOCAL, MIN>::DIG_1:
            return tree_search(d->t[0], info);
        case DIG<LOCAL, MIN>::DIG_3:
            if (info->go_right(d->t[2]))
                return tree_search(d->t[2], info);
            // Fallthrough
        case DIG<LOCAL, MIN>::DIG_2:
            return (info->go_right(d->t[1])? tree_search(d->t[1], info): tree_search(d->t[0], info));
        case DIG<LOCAL, MIN>::DIG_4:
            if (info->go_right(d->t[2]))
                return (info->go_right(d->t[3])? tree_search(d->t[3], info): tree_search(d->t[2], info));
            else
                return (info->go_right(d->t[1])? tree_search(d->t[1], info): tree_search(d->t[0], info));
    }
    LIBFPP_UNREACHABLE();
}
static const void *seq_search(const SEQ<LOCAL, MIN> *s, SearchInfo *info)
{
    switch (s->tag)
    {
        case SEQ<LOCAL, MIN>::NIL:
            return nullptr;
        case SEQ<LOCAL, MIN>::SINGLE:
            return tree_search(s->t, info);
        case SEQ<LOCAL, MIN>::DEEP:
            if (info->go_right(s->r))
                return dig_search(s->r, info);
            else if (info->go_right(s->m))
                return seq_search(s->m, info);
            else
                return dig_search(s->l, info);
    }
    LIBFPP_UNREACHABLE();
}

template <typename S>
LIBFPP_PURE const void *INTERNAL<S>::seq_search(const SEQ<S, MIN> *s, const void *key, order (*compare)(const void *, const void *))
{
    SearchInfo info;
    info.key     = key;
    info.compare = compare;
    return F::seq_search(reinterpret_cast<const SEQ<LOCAL, MIN> *>(s), &info);
}

struct FindInfo
{
    const void *key;
    size_t idx;
    order (*compare)(const void *, const void *);

    bool go_right(const TREE<LOCAL, MIN> *t) const
    {
        return (compare(key, MIN::get_min(t)) >= 0);
    }
    bool go_right(const DIG<LOCAL, MIN> *d) const
    {
        return (compare(key, MIN::get_min(d)) >= 0);
    }
    bool go_right(const SEQ<LOCAL, MIN> *s) const
    {
        return (compare(key, MIN::get_min(s)) >= 0);
    }
};

static ssize_t tree_find(const TREE<LOCAL, MIN> *t, FindInfo *info)
{
    switch (t->tag)
    {
        case TREE<LOCAL, MIN>::ELEMENT:
        {
            ssize_t lo = 0, hi = t->len - 1, hit = -1;
            while (lo <= hi)
            {
                ssize_t mid = lo + (hi - lo) / 2;
                const void *elem = t->at(mid);
                order cmp = info->compare(info->key, elem);
                if (cmp < 0)
                    hi = mid - 1;
                else if (cmp > 0)
                    lo = mid + 1;
                else
                {
                    hit = info->idx + mid;
                    hi = mid - 1;
                }
            }
            return hit;
        }
        case TREE<LOCAL, MIN>::TREE_2:
            if (!info->go_right(t->t[1]))
                return tree_find(t->t[0], info);
            info->idx += t->t[0]->len;
            return tree_find(t->t[1], info);
        case TREE<LOCAL, MIN>::TREE_3:
            if (!info->go_right(t->t[1]))
                return tree_find(t->t[0], info);
            info->idx += t->t[0]->len;
            if (!info->go_right(t->t[2]))
                return tree_find(t->t[1], info);
            info->idx += t->t[1]->len;
            return tree_find(t->t[2], info);
    }
    LIBFPP_UNREACHABLE();
}
static ssize_t dig_find(const DIG<LOCAL, MIN> *d, FindInfo *info)
{
    for (size_t i = 0; i < d->tag; i++)
    {
        if (!info->go_right(d->t[i+1]))
            return tree_find(d->t[i], info);
        info->idx += d->t[i]->len;
    }
    return tree_find(d->t[d->tag], info);
}
static ssize_t seq_find(const SEQ<LOCAL, MIN> *s, FindInfo *info)
{
    switch (s->tag)
    {
        case SEQ<LOCAL, MIN>::NIL:
            return -1;
        case SEQ<LOCAL, MIN>::SINGLE:
            return tree_find(s->t, info);
        case SEQ<LOCAL, MIN>::DEEP:
            if (s->m->tag == SEQ<LOCAL, MIN>::NIL)
            {
                if (!info->go_right(s->r))
                    return dig_find(s->l, info);
                info->idx += s->l->len;
                return dig_find(s->r, info);
            }
            if (!info->go_right(s->m))
                return dig_find(s->l, info);
            info->idx += s->l->len;
            if (!info->go_right(s->r))
                return seq_find(s->m, info);
            info->idx += s->m->len;
            return dig_find(s->r, info);
    }
    LIBFPP_UNREACHABLE();
}

template <typename S>
LIBFPP_PURE ssize_t INTERNAL<S>::seq_find(const SEQ<S, MIN> *s, const void *key, order (*compare)(const void *, const void *))
{
    FindInfo info;
    info.key     = key;
    info.idx     = 0;
    info.compare = compare;
    ssize_t r = F::seq_find(reinterpret_cast<const SEQ<LOCAL, MIN> *>(s), &info);
    return r;
}

struct FindBelowInfo
{
    const void *key;
    size_t idx;
    order (*compare)(const void *, const void *);
    bool eq;

    bool go_right(const TREE<LOCAL, MIN> *t)
    {
        auto r = compare(key, MIN::get_min(t));
        if (r == 0) eq = true;
        return (r > 0);
    }
    bool go_right(const DIG<LOCAL, MIN> *d)
    {
        auto r = compare(key, MIN::get_min(d));
        if (r == 0) eq = true;
        return (r > 0);
    }
    bool go_right(const SEQ<LOCAL, MIN> *s)
    {
        auto r = compare(key, MIN::get_min(s));
        if (r == 0) eq = true;
        return (r > 0);
    }
};

static ssize_t tree_below_find(const TREE<LOCAL, MIN> *t, FindBelowInfo *info)
{
    switch (t->tag)
    {
        case TREE<LOCAL, MIN>::ELEMENT:
        {
            ssize_t lo = 0, hi = t->len - 1, hit = -1;
            while (lo <= hi)
            {
                ssize_t mid = lo + (hi - lo) / 2;
                const void *elem = t->at(mid);
                order cmp = info->compare(info->key, elem);
                if (cmp < 0)
                    hi = mid - 1;
                else if (cmp == 0)
                {
                    info->eq = true;
                    hi = mid - 1;
                }
                else
                {
                    hit = info->idx + mid;
                    lo = mid + 1;
                }
            }
            return hit;
        }
        case TREE<LOCAL, MIN>::TREE_2:
            if (!info->go_right(t->t[1]))
                return tree_below_find(t->t[0], info);
            info->idx += t->t[0]->len;
            return tree_below_find(t->t[1], info);
        case TREE<LOCAL, MIN>::TREE_3:
            if (!info->go_right(t->t[1]))
                return tree_below_find(t->t[0], info);
            info->idx += t->t[0]->len;
            if (!info->go_right(t->t[2]))
                return tree_below_find(t->t[1], info);
            info->idx += t->t[1]->len;
            return tree_below_find(t->t[2], info);
    }
    LIBFPP_UNREACHABLE();
}
static ssize_t dig_below_find(const DIG<LOCAL, MIN> *d, FindBelowInfo *info)
{
    for (size_t i = 0; i < d->tag; i++)
    {
        if (!info->go_right(d->t[i+1]))
            return tree_below_find(d->t[i], info);
        info->idx += d->t[i]->len;
    }
    return tree_below_find(d->t[d->tag], info);
}
static ssize_t seq_below_find(const SEQ<LOCAL, MIN> *s, FindBelowInfo *info)
{
    switch (s->tag)
    {
        case SEQ<LOCAL, MIN>::NIL:
            return -1;
        case SEQ<LOCAL, MIN>::SINGLE:
            return tree_below_find(s->t, info);
        case SEQ<LOCAL, MIN>::DEEP:
            if (s->m->tag == SEQ<LOCAL, MIN>::NIL)
            {
                if (!info->go_right(s->r))
                    return dig_below_find(s->l, info);
                info->idx += s->l->len;
                return dig_below_find(s->r, info);
            }
            if (!info->go_right(s->m))
                return dig_below_find(s->l, info);
            info->idx += s->l->len;
            if (!info->go_right(s->r))
                return seq_below_find(s->m, info);
            info->idx += s->m->len;
            return dig_below_find(s->r, info);
    }
    LIBFPP_UNREACHABLE();
}

template <typename S>
LIBFPP_PURE ssize_t INTERNAL<S>::seq_multi_find(const SEQ<S, MIN> *s, const void *key, order (*compare)(const void *, const void *))
{
    FindBelowInfo info;
    info.key     = key;
    info.idx     = 0;
    info.compare = compare;
    info.eq      = false;
    ssize_t r = F::seq_below_find(reinterpret_cast<const SEQ<LOCAL, MIN> *>(s), &info);
    return (info.eq? r+1: -1);
}

template <typename S>
struct InsertSearchInfo
{
    size_t refcount;
    bool fast;
    const void *key;
    order (*compare)(const void *, const void *);
    const TREE<S, MIN> *next;
    result <const TREE<S, MIN> *, const TREE<S, MIN> *, const TREE<S, MIN> *> (*insert)(const TREE<S, MIN> *, const TREE<S, MIN> *, const void *, size_t);
    const void *max;

    bool go_right(const TREE<S, MIN> *t) const
    {
        return (compare(key, MIN::get_min(t)) >= 0);
    }
    bool go_right(const DIG<S, MIN> *d) const
    {
        return (compare(key, MIN::get_min(d)) >= 0);
    }
    bool go_right(const SEQ<S, MIN> *s) const
    {
        if (s->tag == SEQ<LOCAL, MIN>::NIL) return false;
        return (compare(key, MIN::get_min(s)) >= 0);
    }
};

template <typename S>
static const TREE<S, MIN> *tree_insert(const TREE<S, MIN> *t, InsertSearchInfo<S> *info)
{
    const TREE<S, MIN> *u;
    auto *v = const_cast<TREE<S, MIN> *>(t);
    info->refcount |= t->refcount;
    const void *max = info->max;
    switch (t->tag)
    {
        case TREE<S, MIN>::ELEMENT:
        {
            auto r = info->insert(t, nullptr, info->key, info->refcount);
            info->next = r.b;
            info->fast = (r.b == nullptr && r.a == t);
            return r.a;
        }
        case TREE<S, MIN>::TREE_2:
            if (t->t[0]->tag == TREE<S, MIN>::ELEMENT)
            {
                auto r = info->insert(t->t[0], t->t[1], info->key, info->refcount);
                if (r.a == nullptr) return nullptr;
                info->fast = (r.a == t->t[0] && r.b == t->t[1]);
                if (info->fast) goto fast;
                u = (r.c == nullptr? tree(r.a, r.b, max): tree(r.a, r.b, r.c, max));
            }
            else if (info->go_right(t->t[1]))
            {
                u = tree_insert(t->t[1], info);
                if (LIBFPP_LIKELY(info->fast)) goto fast;
                if (u == nullptr) return nullptr;
                u = (info->next != nullptr? tree(t->t[0], u, info->next, max): tree(t->t[0], u, max));
                info->next = nullptr;
            }
            else
            {
                u = tree_insert(t->t[0], info);
                if (LIBFPP_LIKELY(info->fast)) goto fast;
                if (u == nullptr) return nullptr;
                u = (info->next != nullptr? tree(u, info->next, t->t[1], max): tree(u, t->t[1], max));
                info->next = nullptr;
            }
            return u;
        case TREE<S, MIN>::TREE_3:
            if (t->t[2]->tag == TREE<S, MIN>::ELEMENT)
            {
                if (info->go_right(t->t[2]))
                {
                    auto r = info->insert(t->t[1], t->t[2], info->key, info->refcount);
                    if (r.a == nullptr) return nullptr;
                    info->fast = (r.a == t->t[1] && r.b == t->t[2]);
                    if (info->fast) goto fast;
                    info->next = (r.c == nullptr? nullptr: tree(r.b, r.c, max));
                    u = (r.c == nullptr? tree(t->t[0], r.a, r.b, max): tree(t->t[0], r.a, max));
                }
                else
                {
                    auto r = info->insert(t->t[0], t->t[1], info->key, info->refcount);
                    if (r.a == nullptr) return nullptr;
                    info->fast = (r.a == t->t[0] && r.b == t->t[1]);
                    if (info->fast) goto fast;
                    info->next = (r.c == nullptr? nullptr: tree(r.c, t->t[2], max));
                    u = (r.c == nullptr? tree(r.a, r.b, t->t[2], max): tree(r.a, r.b, max));
                }
            }
            else if (info->go_right(t->t[2]))
            {
                u = tree_insert(t->t[2], info);
                if (LIBFPP_LIKELY(info->fast)) goto fast;
                if (u == nullptr) return nullptr;
                info->next = (info->next == nullptr? nullptr: tree(u, info->next, max));
                u = (info->next == nullptr? tree(t->t[0], t->t[1], u, max): tree(t->t[0], t->t[1], max));
            }
            else if (info->go_right(t->t[1]))
            {
                u = tree_insert(t->t[1], info);
                if (LIBFPP_LIKELY(info->fast)) goto fast;
                if (u == nullptr) return nullptr;
                info->next = (info->next == nullptr? nullptr: tree(info->next, t->t[2], max));
                u = (info->next == nullptr? tree(t->t[0], u, t->t[2], max): tree(t->t[0], u, max));
            }
            else
            {
                u = tree_insert(t->t[0], info);
                if (LIBFPP_LIKELY(info->fast)) goto fast;
                if (u == nullptr) return nullptr;
                u = (info->next == nullptr? tree(u, t->t[1], t->t[2], max): tree(u, info->next, max));
                info->next = (info->next == nullptr? nullptr: tree(t->t[1], t->t[2], max));
            }
            return u;
        fast:
            v->len++;
            v->monoid.max = MIN::init_max(v, max);
            return v;
    }
    LIBFPP_UNREACHABLE();
}

template <typename S>
static const DIG<S, MIN> *dig_insert_left(const DIG<S, MIN> *d, InsertSearchInfo<S> *info)
{
    const TREE<S, MIN> *t;
    const DIG<S, MIN> *e;
    auto *f = const_cast<DIG<S, MIN> *>(d);
    info->refcount |= d->refcount;
    const void *max = info->max;
    switch (d->tag)
    {
        case DIG<S>::DIG_1:
            t = tree_insert(d->t[0], info);
            if (LIBFPP_LIKELY(info->fast)) goto fast;
            if (t == nullptr) return nullptr;
            e = (info->next != nullptr? dig(t, info->next, max): dig(t, max));
            info->next = nullptr;
            return e;
        case DIG<S>::DIG_2:
            if (d->t[1]->tag == TREE<S, MIN>::ELEMENT)
            {
                auto r = info->insert(d->t[0], d->t[1], info->key, info->refcount);
                if (r.a == nullptr) return nullptr;
                info->fast = (r.a == d->t[0] && r.b == d->t[1]);
                if (info->fast) goto fast;
                e = (r.c == nullptr? dig(r.a, r.b, max): dig(r.a, r.b, r.c, max));
            }
            else if (info->go_right(d->t[1]))
            {
                t = tree_insert(d->t[1], info);
                if (LIBFPP_LIKELY(info->fast)) goto fast;
                if (t == nullptr) return nullptr;
                e = (info->next != nullptr? dig(d->t[0], t, info->next, max):
                dig(d->t[0], t, max));
                info->next = nullptr;
            }
            else
            {
                t = tree_insert(d->t[0], info);
                if (LIBFPP_LIKELY(info->fast)) goto fast;
                if (t == nullptr) return nullptr;
                e = (info->next != nullptr? dig(t, info->next, d->t[1], max): dig(t, d->t[1], max));
                info->next = nullptr;
            }
            return e;
        case DIG<S>::DIG_3:
            if (d->t[2]->tag == TREE<S, MIN>::ELEMENT)
            {
                if (info->go_right(d->t[2]))
                {
                    auto r = info->insert(d->t[1], d->t[2], info->key, info->refcount);
                    if (r.a == nullptr) return nullptr;
                    info->fast = (r.a == d->t[1] && r.b == d->t[2]);
                    if (info->fast) goto fast;
                    e = (r.c == nullptr? dig(d->t[0], r.a, r.b, max): dig(d->t[0], r.a, r.b, r.c, max));
                }
                else
                {
                    auto r = info->insert(d->t[0], d->t[1], info->key, info->refcount);
                    if (r.a == nullptr) return nullptr;
                    info->fast = (r.a == d->t[0] && r.b == d->t[1]);
                    if (info->fast) goto fast;
                    e = (r.c == nullptr? dig(r.a, r.b, d->t[2], max): dig(r.a, r.b, r.c, d->t[2], max));
                }
            }
            else if (info->go_right(d->t[2]))
            {
                t = tree_insert(d->t[2], info);
                if (LIBFPP_LIKELY(info->fast)) goto fast;
                if (t == nullptr) return nullptr;
                e = (info->next != nullptr? dig(d->t[0], d->t[1], t, info->next, max): dig(d->t[0], d->t[1], t, max));
                info->next = nullptr;
            }
            else if (info->go_right(d->t[1]))
            {
                t = tree_insert(d->t[1], info);
                if (LIBFPP_LIKELY(info->fast)) goto fast;
                if (t == nullptr) return nullptr;
                e = (info->next != nullptr? dig(d->t[0], t, info->next, d->t[2], max): dig(d->t[0], t, d->t[2], max));
                info->next = nullptr;
            }
            else
            {
                t = tree_insert(d->t[0], info);
                if (LIBFPP_LIKELY(info->fast)) goto fast;
                if (t == nullptr) return nullptr;
                e = (info->next != nullptr? dig(t, info->next, d->t[1], d->t[2], max): dig(t, d->t[1], d->t[2], max));
                info->next = nullptr;
            }
            return e;
        case DIG<S>::DIG_4:
            if (d->t[2]->tag == TREE<S, MIN>::ELEMENT)
            {
                if (info->go_right(d->t[2]))
                {
                    auto r = info->insert(d->t[2], d->t[3], info->key, info->refcount);
                    if (r.a == nullptr) return nullptr;
                    info->fast = (r.a == d->t[2] && r.b == d->t[3]);
                    if (info->fast) goto fast;
                    e = (r.c == nullptr? dig(d->t[0], d->t[1], r.a, r.b, max): dig(d->t[0], d->t[1], r.a, max));
                    info->next = (r.c == nullptr? nullptr: tree(r.b, r.c, max));
                }
                else
                {
                    auto r = info->insert(d->t[0], d->t[1], info->key, info->refcount);
                    if (r.a == nullptr) return nullptr;
                    info->fast = (r.a == d->t[0] && r.b == d->t[1]);
                    if (info->fast) goto fast;
                    e = (r.c == nullptr? dig(r.a, r.b, d->t[2], d->t[3], max): dig(r.a, r.b, r.c, max));
                    info->next = (r.c == nullptr? nullptr: tree(d->t[2], d->t[3], max));
                }
            }
            else if (info->go_right(d->t[2]))
            {
                if (info->go_right(d->t[3]))
                {
                    t = tree_insert(d->t[3], info);
                    if (LIBFPP_LIKELY(info->fast)) goto fast;
                    if (t == nullptr) return nullptr;
                    e = (info->next == nullptr? dig(d->t[0], d->t[1], d->t[2], t, max): dig(d->t[0], d->t[1], max));
                    info->next = (info->next == nullptr? nullptr: tree(d->t[2], t, info->next, max));
                }
                else
                {
                    t = tree_insert(d->t[2], info);
                    if (LIBFPP_LIKELY(info->fast)) goto fast;
                    if (t == nullptr) return nullptr;
                    e = (info->next == nullptr? dig(d->t[0], d->t[1], t, d->t[3], max): dig(d->t[0], d->t[1], max));
                    info->next = (info->next == nullptr? nullptr: tree(t, info->next, d->t[3], max));
                }
            }
            else
            {
                if (info->go_right(d->t[1]))
                {
                    t = tree_insert(d->t[1], info);
                    if (LIBFPP_LIKELY(info->fast)) goto fast;
                    if (t == nullptr) return nullptr;
                    e = (info->next == nullptr? dig(d->t[0], t, d->t[2], d->t[3], max): dig(d->t[0], t, max));
                    info->next = (info->next == nullptr? nullptr: tree(info->next, d->t[2], d->t[3], max));
                }
                else
                {
                    t = tree_insert(d->t[0], info);
                    if (LIBFPP_LIKELY(info->fast)) goto fast;
                    if (t == nullptr) return nullptr;
                    e = (info->next == nullptr? dig(t, d->t[1], d->t[2], d->t[3], max): dig(t, info->next, max));
                    info->next = (info->next == nullptr? nullptr: tree(d->t[1], d->t[2], d->t[3], max));
                }
            }
            return e;
        fast:
            f->len++;
            f->monoid.max = MIN::init_max(f, max);
            return f;
    }
    LIBFPP_UNREACHABLE();
}

template <typename S>
static const DIG<S, MIN> *dig_insert_right(const DIG<S, MIN> *d, InsertSearchInfo<S> *info)
{
    const TREE<S, MIN> *t;
    const DIG<S, MIN> *e;
    auto *f = const_cast<DIG<S, MIN> *>(d);
    info->refcount |= d->refcount;
    const void *max = info->max;
    switch (d->tag)
    {
        case DIG<S>::DIG_1: case DIG<S>::DIG_2: case DIG<S>::DIG_3:
            return dig_insert_left(d, info);
        case DIG<S>::DIG_4:
            if (d->t[2]->tag == TREE<S, MIN>::ELEMENT)
            {
                if (info->go_right(d->t[2]))
                {
                    auto r = info->insert(d->t[2], d->t[3], info->key, info->refcount);
                    if (r.a == nullptr) return nullptr;
                    info->fast = (r.a == d->t[2] && r.b == d->t[3]);
                    if (info->fast) goto fast;
                    e = (r.c == nullptr? dig(d->t[0], d->t[1], r.a, r.b, max): dig(r.a, r.b, r.c, max));
                    info->next = (r.c == nullptr? nullptr: tree(d->t[0], d->t[1], max));
                }
                else
                {
                    auto r = info->insert(d->t[0], d->t[1], info->key, info->refcount);
                    if (r.a == nullptr) return nullptr;
                    info->fast = (r.a == d->t[0] && r.b == d->t[1]);
                    if (info->fast) goto fast;
                    e = (r.c == nullptr? dig(r.a, r.b, d->t[2], d->t[3], max): dig(r.c, d->t[2], d->t[3], max));
                    info->next = (r.c == nullptr? nullptr: tree(r.a, r.b, max));
                }
            }
            else if (info->go_right(d->t[2]))
            {
                if (info->go_right(d->t[3]))
                {
                    t = tree_insert(d->t[3], info);
                    if (LIBFPP_LIKELY(info->fast)) goto fast;
                    if (t == nullptr) return nullptr;
                    e = (info->next == nullptr? dig(d->t[0], d->t[1], d->t[2], t, max): dig(t, info->next, max));
                    info->next = (info->next == nullptr? nullptr: tree(d->t[0], d->t[1], d->t[2], max));
                }
                else
                {
                    t = tree_insert(d->t[2], info);
                    if (LIBFPP_LIKELY(info->fast)) goto fast;
                    if (t == nullptr) return nullptr;
                    e = (info->next == nullptr? dig(d->t[0], d->t[1], t, d->t[3], max): dig(info->next, d->t[3], max));
                    info->next = (info->next == nullptr? nullptr: tree(d->t[0], d->t[1], t, max));
                }
            }
            else
            {
                if (info->go_right(d->t[1]))
                {
                    t = tree_insert(d->t[1], info);
                    if (LIBFPP_LIKELY(info->fast)) goto fast;
                    if (t == nullptr) return nullptr;
                    e = (info->next == nullptr? dig(d->t[0], t, d->t[2], d->t[3], max): dig(d->t[2], d->t[3], max));
                    info->next = (info->next == nullptr? nullptr: tree(d->t[0], t, info->next, max));
                }
                else
                {
                    t = tree_insert(d->t[0], info);
                    if (LIBFPP_LIKELY(info->fast)) goto fast;
                    if (t == nullptr) return nullptr;
                    e = (info->next == nullptr? dig(t, d->t[1], d->t[2], d->t[3], max): dig(d->t[2], d->t[3], max));
                    info->next = (info->next == nullptr? nullptr: tree(t, info->next, d->t[1], max));
                }
            }
            return e;
        fast:
            f->len++;
            f->monoid.max = MIN::init_max(f, max);
            return f;
    }
    LIBFPP_UNREACHABLE();
}

template <typename S>
static const SEQ<S, MIN> *seq_insert(const SEQ<S, MIN> *s, InsertSearchInfo<S> *info)
{
    const TREE<S, MIN> *t;
    const DIG<S, MIN> *d;
    auto *r = const_cast<SEQ<S, MIN> *>(s);
    info->refcount |= s->refcount;
    const void *max = info->max;
    switch (s->tag)
    {
        case SEQ<S, MIN>::NIL:
        {
            auto r = info->insert(nullptr, nullptr, info->key, 0);
            return single(r.a, max);
        }
        case SEQ<S, MIN>::SINGLE:
            t = tree_insert(s->t, info);
            if (LIBFPP_LIKELY(info->fast)) goto fast;
            if (t == nullptr) return nullptr;
            if (info->next != nullptr)
                return deep(dig(t, max), nil<S, MIN>(), dig(info->next, max), max);
            return single(t, max);
        case SEQ<S, MIN>::DEEP:
            if (info->go_right(s->r))
            {
                d = dig_insert_right(s->r, info);
                if (LIBFPP_LIKELY(info->fast)) goto fast;
                if (d == nullptr) return nullptr;
                if (info->next != nullptr)
                {
                    const auto *m = seq_push_back(s->m, info->next, max);
                    return deep(s->l, m, d, max);
                }
                return deep(s->l, s->m, d, max);
            }
            else if (info->go_right(s->m))
            {
                const auto *m = seq_insert(s->m, info);
                if (LIBFPP_LIKELY(info->fast)) goto fast;
                if (m == nullptr) return nullptr;
                return deep(s->l, m, s->r, max);
            }
            else
            {
                d = dig_insert_left(s->l, info);
                if (LIBFPP_LIKELY(info->fast)) goto fast;
                if (d == nullptr) return nullptr;
                if (info->next != nullptr)
                {
                    const auto *m = seq_push_front(s->m, info->next, max);
                    return deep(d, m, s->r, max);
                }
                return deep(d, s->m, s->r, max);
            }
        fast:
            r->len++;
            r->monoid.max = MIN::init_max(r, max);
            return r;
    }
    LIBFPP_UNREACHABLE();
}

template <typename S>
result<const SEQ<S, MIN> *, bool> INTERNAL<S>::seq_search_insert(const SEQ<S, MIN> *s, const void *key, order (*compare)(const void *, const void *), result<const TREE<S, MIN> *, const TREE<S, MIN> *, const TREE<S, MIN> *> (*insert)(const TREE<S, MIN> *, const TREE<S, MIN> *, const void *, size_t), const void *max)
{
    InsertSearchInfo<S> info;
    info.refcount = 0;
    info.fast     = false;
    info.key      = key;
    info.compare  = compare;
    info.next     = nullptr;
    info.insert   = insert;
    info.max      = max;
    const auto *r = F::seq_insert(s, &info);
    return {(r == nullptr? s: r), r != nullptr};
}

template <typename S>
LIBFPP_PURE result<size_t, size_t, size_t, size_t> INTERNAL<S>::tree_rebalance(size_t t_len, size_t u_len, size_t idx)
{
    size_t mid, rem;
    size_t r[3];
    if (u_len == 0)
    {
        mid = t_len / 2;
        rem = t_len % 2;
        r[0] = r[1] = mid;
        r[2] = 0;
    }
    else
    {
        mid = (t_len + u_len) / 3;
        rem = (t_len + u_len) % 3;
        r[0] = r[1] = r[2] = mid;
    }
    size_t pos;
    if (idx <= r[0])
    {
        pos = 0;
        if (rem) r[1] += 1, rem--;
        if (rem) r[2] += 1, rem--;
    }
    else if (idx <= r[0] + r[1] || (rem > 0 && idx == r[0] + r[1] + 1))
    {
        pos = 1;
        if (rem) r[0] += 1, rem--;
        if (rem) r[2] += 1, rem--;
    }
    else
    {
        pos = 2;
        if (rem) r[0] += 1, rem--;
        if (rem) r[1] += 1, rem--;
    }
    return {r[0], r[1], r[2], pos};
}

template <typename S>
struct EraseSearchInfo
{
    bool reduced, left, erased;
    const TREE<S, MIN> *orphan;
    const void *key;
    order (*compare)(const void *, const void *);
    result<const TREE<S, MIN> *, const TREE<S, MIN> *> (*erase)(const TREE<S, MIN> *, const TREE<S, MIN> *, const void *);
    const void *max;

    bool go_right(const TREE<S, MIN> *t) const
    {
        return (compare(key, MIN::get_min(t)) >= 0);
    }
    bool go_right(const DIG<S, MIN> *d) const
    {
        return (compare(key, MIN::get_min(d)) >= 0);
    }
    bool go_right(const SEQ<S, MIN> *s) const
    {
        if (s->tag == SEQ<S, MIN>::NIL) return false;
        return (compare(key, MIN::get_min(s)) >= 0);
    }
};

template <typename S>
static const TREE<S, MIN> *tree_erase(const TREE<S, MIN> *t, EraseSearchInfo<S> *info)
{
    const TREE<S, MIN> *u;
    const void *max = info->max;
    switch (t->tag)
    {
        case TREE<S, MIN>::ELEMENT:
        {
            auto r = info->erase(t, nullptr, info->key);
            info->erased  = (r.a != t);
            info->reduced = (r.a == nullptr);
            return r.a;
        }
        case TREE<S, MIN>::TREE_2:
            if (t->t[0]->tag == TREE<S, MIN>::ELEMENT)
            {
                auto r = info->erase(t->t[0], t->t[1], info->key);
                info->erased  = (r.a != t->t[0] || r.b != t->t[1]);
                info->reduced = (r.b == nullptr);
                if (!info->erased) return t;
                return (info->reduced? r.a: tree(r.a, r.b, max));
            }
            if (info->go_right(t->t[1]))
            {
                u = tree_erase(t->t[1], info);
                if (!info->erased) return t;
                return (info->reduced? tree_fix_2(t, 1, u, &info->reduced, max): tree(t->t[0], u, max));
            }
            u = tree_erase(t->t[0], info);
            if (!info->erased) return t;
            return (info->reduced? tree_fix_2(t, 0, u, &info->reduced, max): tree(u, t->t[1], max));
        case TREE<S, MIN>::TREE_3:
            if (t->t[0]->tag == TREE<S, MIN>::ELEMENT)
            {
                if (info->go_right(t->t[1]))
                {
                    auto r = info->erase(t->t[1], t->t[2], info->key);
                    info->erased  = (r.a != t->t[1] || r.b != t->t[2]);
                    if (!info->erased) return t;
                    return (r.b == nullptr? tree(t->t[0], r.a, max): tree(t->t[0], r.a, r.b, max));
                }
                auto r = info->erase(t->t[0], t->t[1], info->key);
                info->erased = (r.a != t->t[0] || r.b != t->t[1]);
                if (!info->erased) return t;
                return (r.b == nullptr? tree(r.a, t->t[2], max): tree(r.a, r.b, t->t[2], max));
            }
            if (info->go_right(t->t[2]))
            {
                u = tree_erase(t->t[2], info);
                if (!info->erased) return t;
                return (LIBFPP_UNLIKELY(info->reduced)? tree_fix_3(t, 2, u, &info->reduced, max): tree(t->t[0], t->t[1], u, max));
            }
            if (info->go_right(t->t[1]))
            {
                u = tree_erase(t->t[1], info);
                if (!info->erased) return t;
                return (LIBFPP_UNLIKELY(info->reduced)? tree_fix_3(t, 1, u, &info->reduced, max): tree(t->t[0], u, t->t[2], max));
            }
            u = tree_erase(t->t[0], info);
            if (!info->erased) return t;
            return (LIBFPP_UNLIKELY(info->reduced)? tree_fix_3(t, 0, u, &info->reduced, max): tree(u, t->t[1], t->t[2], max));
    }
    LIBFPP_UNREACHABLE();
}

template <typename S>
static const DIG<S, MIN> *dig_erase(const DIG<S, MIN> *d, EraseSearchInfo<S> *info)
{
    const TREE<S, MIN> *u;
    const void *max = info->max;
    switch (d->tag)
    {
        case DIG<S, MIN>::DIG_1:
            u = tree_erase(d->t[0], info);
            if (!info->erased) return d;
            if (info->reduced)
            {
                info->orphan = u;
                return nullptr;
            }
            return dig(u, max);
        case DIG<S, MIN>::DIG_2:
            if (d->t[0]->tag == TREE<S, MIN>::ELEMENT)
            {
                auto r = info->erase(d->t[0], d->t[1], info->key);
                info->erased = (r.a != d->t[0] || r.b != d->t[1]);
                if (!info->erased) return d;
                return (r.b == nullptr? dig(r.a, max): dig(r.a, r.b, max));
            }
            if (info->go_right(d->t[1]))
            {
                u = tree_erase(d->t[1], info);
                if (!info->erased) return d;
                return (LIBFPP_UNLIKELY(info->reduced)? dig_fix_2(d, 1, u, &info->reduced, max): dig(d->t[0], u, max));
            }
            u = tree_erase(d->t[0], info);
            if (!info->erased) return d;
            return (LIBFPP_UNLIKELY(info->reduced)? dig_fix_2(d, 0, u, &info->reduced, max): dig(u, d->t[1], max));
        case DIG<S, MIN>::DIG_3:
            if (d->t[0]->tag == TREE<S, MIN>::ELEMENT)
            {
                if (info->go_right(d->t[1]))
                {
                    auto r = info->erase(d->t[1], d->t[2], info->key);
                    info->erased = (r.a != d->t[1] || r.b != d->t[2]);
                    if (!info->erased) return d;
                    return (r.b == nullptr? dig(d->t[0], r.a, max): dig(d->t[0], r.a, r.b, max));
                }
                auto r = info->erase(d->t[0], d->t[1], info->key);
                info->erased = (r.a != d->t[0] || r.b != d->t[1]);
                if (!info->erased) return d;
                return (r.b == nullptr? dig(r.a, d->t[2], max): dig(r.a, r.b, d->t[2], max));
            }
            if (info->go_right(d->t[2]))
            {
                u = tree_erase(d->t[2], info);
                if (!info->erased) return d;
                return (LIBFPP_UNLIKELY(info->reduced)? dig_fix_3(d, 2, u, &info->reduced, max): dig(d->t[0], d->t[1], u, max));
            }
            if (info->go_right(d->t[1]))
            {
                u = tree_erase(d->t[1], info);
                if (!info->erased) return d;
                return (LIBFPP_UNLIKELY(info->reduced)? dig_fix_3(d, 1, u, &info->reduced, max): dig(d->t[0], u, d->t[2], max));
            }
            u = tree_erase(d->t[0], info);
            if (!info->erased) return d;
            return (LIBFPP_UNLIKELY(info->reduced)? dig_fix_3(d, 0, u, &info->reduced, max): dig(u, d->t[1], d->t[2], max)); 
        case DIG<S, MIN>::DIG_4:
            if (d->t[0]->tag == TREE<S, MIN>::ELEMENT)
            {
                if (info->go_right(d->t[2]))
                {
                    auto r = info->erase(d->t[2], d->t[3], info->key);
                    info->erased = (r.a != d->t[2] || r.b != d->t[3]);
                    if (!info->erased) return d;
                    return (r.b == nullptr? dig(d->t[0], d->t[1], r.a, max): dig(d->t[0], d->t[1], r.a, r.b, max));
                }
                if (info->go_right(d->t[1]))
                {
                    auto r = info->erase(d->t[1], d->t[2], info->key);
                    if (!info->erased) return d;
                    info->erased  = (r.a != d->t[1] || r.b != d->t[2]);
                    return (r.b == nullptr? dig(d->t[0], r.a, d->t[3], max): dig(d->t[0], r.a, r.b, d->t[3], max));
                }
                auto r = info->erase(d->t[0], d->t[1], info->key);
                if (!info->erased) return d;
                info->erased = (r.a != d->t[0] || r.b != d->t[1]);
                return (r.b == nullptr? dig(r.a, d->t[2], d->t[3], max): dig(r.a, r.b, d->t[2], d->t[3], max));
            }
            if (info->go_right(d->t[2]))
            {
                if (info->go_right(d->t[3]))
                {
                    u = tree_erase(d->t[3], info);
                    if (!info->erased) return d;
                    return (LIBFPP_UNLIKELY(info->reduced)? dig_fix_4(d, 3, u, &info->reduced, max): dig(d->t[0], d->t[1], d->t[2], u, max));
                }
                else
                {
                    u = tree_erase(d->t[2], info);
                    if (!info->erased) return d;
                    return (LIBFPP_UNLIKELY(info->reduced)? dig_fix_4(d, 2, u, &info->reduced, max): dig(d->t[0], d->t[1], u, d->t[3], max));
                }
            }
            else
            {
                if (info->go_right(d->t[1]))
                {
                    u = tree_erase(d->t[1], info);
                    if (!info->erased) return d;
                    return (LIBFPP_UNLIKELY(info->reduced)? dig_fix_4(d, 1, u, &info->reduced, max): dig(d->t[0], u, d->t[2], d->t[3], max));
                }
                else
                {
                    u = tree_erase(d->t[0], info);
                    if (!info->erased) return d;
                    return (LIBFPP_UNLIKELY(info->reduced)? dig_fix_4(d, 0, u, &info->reduced, max): dig(u, d->t[1], d->t[2], d->t[3], max));
                }
            }
    }
    LIBFPP_UNREACHABLE();
}

template <typename S>
static const SEQ<S, MIN> *seq_erase(const SEQ<S, MIN> *s, EraseSearchInfo<S> *info)
{
    const DIG<S, MIN> *d;
    const TREE<S, MIN> *t, *u;
    const SEQ<S, MIN> *m, *n;
    info->left = false;
    const void *max = info->max;
    switch (s->tag)
    {
        case SEQ<S, MIN>::NIL:
            return s;
        case SEQ<S, MIN>::SINGLE:
            t = tree_erase(s->t, info);
            if (!info->erased) return s;
            if (info->reduced)
            {
                info->orphan = t;
                return nil<S, MIN>();
            }
            return single(t);
        case SEQ<S, MIN>::DEEP:
            if (info->go_right(s->r))
            {
                d = dig_erase(s->r, info);
                if (!info->erased) return s;
                return (LIBFPP_UNLIKELY(info->reduced)? deep(s->l, s->m, max): deep(s->l, s->m, d, max));
            }
            if (info->go_right(s->m))
            {
                m = seq_erase(s->m, info);
                if (!info->erased) return s;
                if (LIBFPP_UNLIKELY(info->reduced && info->left))
                {
                    info->reduced = false;
                    d = s->l;
                    switch (d->tag)
                    {
                        case DIG<S, MIN>::DIG_1: return deep(dig(d->t[0], info->orphan, max), m, s->r, max);
                        case DIG<S, MIN>::DIG_2: return deep(dig(d->t[0], d->t[1], info->orphan, max), m, s->r, max);
                        case DIG<S, MIN>::DIG_3: return deep(dig(d->t[0], d->t[1], d->t[2], info->orphan, max), m, s->r, max);
                        case DIG<S, MIN>::DIG_4:
                            u = tree(d->t[3], info->orphan, max);
                            n = seq_push_front(m, u, max);
                            m->gc();
                            return deep(dig(d->t[0], d->t[1], d->t[2], max), n, s->r, max);
                    }
                    LIBFPP_UNREACHABLE();
                }
                if (LIBFPP_UNLIKELY(info->reduced && !info->left))
                {
                    info->reduced = false;
                    d = s->r;
                    switch (d->tag)
                    {
                        case DIG<S, MIN>::DIG_1: return deep(s->l, m, dig(info->orphan, d->t[0], max), max);
                        case DIG<S, MIN>::DIG_2: return deep(s->l, m, dig(info->orphan, d->t[0], d->t[1], max), max);
                        case DIG<S, MIN>::DIG_3: return deep(s->l, m, dig(info->orphan, d->t[0], d->t[1], d->t[2], max), max);
                        case DIG<S, MIN>::DIG_4:
                            u = tree(info->orphan, d->t[0], max);
                            n = seq_push_back(m, u, max);
                            m->gc();
                            return deep(s->l, n, dig(d->t[1], d->t[2], d->t[3], max), max);
                    }
                    LIBFPP_UNREACHABLE();
                }
                return deep(s->l, m, s->r, max);
            }
            d = dig_erase(s->l, info);
            info->left = true;
            if (!info->erased) return s;
            return (LIBFPP_UNLIKELY(info->reduced)? deep(s->m, s->r, max): deep(d, s->m, s->r, max));
    }
    LIBFPP_UNREACHABLE();
}

template <typename S>
result<const SEQ<S, MIN> *, bool> INTERNAL<S>::seq_search_erase(const SEQ<S, MIN> *s, const void *key, order (*compare)(const void *, const void *), result<const TREE<S, MIN> *, const TREE<S, MIN> *> (*erase)(const TREE<S, MIN> *, const TREE<S, MIN> *, const void *), const void *max)
{
    EraseSearchInfo<S> info;
    info.reduced = false;
    info.erased  = false;
    info.key     = key;
    info.compare = compare;
    info.erase   = erase;
    info.max     = max;
    auto r = F::seq_erase(s, &info);
    return {r, info.erased};
}

template <typename S, typename M = EMPTY>
static result<const ITR<S, M> *, const OBJECT<S> *> itr_restore(const ITR<S, M> *i, ssize_t pos, const void *max = nullptr)
{
    if (i->next == nullptr)
        return {i, i->obj};
    size_t size = 0;
    const OBJECT<S> *obj = i->obj;
    size_t path = i->path;
    i = i->next;
    while (true)
    {
        switch (i->tag)
        {
            case ITR<S, M>::TREE:
            {
                const TREE<S, M> *u = reinterpret_cast<const TREE<S, M> *>(obj);
                const TREE<S, M> *t = i->t;
                switch (t->tag)
                {
                    case TREE<S, M>::TREE_2:
                        t = (path == 0? tree(u, t->t[1], max): tree(t->t[0], u, max));
                        break;
                    case TREE<S, M>::TREE_3:
                        switch (path)
                        {
                            case 0: t = tree(u, t->t[1], t->t[2], max); break;
                            case 1: t = tree(t->t[0], u, t->t[2], max); break;
                            case 2: t = tree(t->t[0], t->t[1], u, max); break;
                            default: LIBFPP_UNREACHABLE();
                        }
                        break;
                    default:
                        LIBFPP_UNREACHABLE();
                }
                obj = reinterpret_cast<const OBJECT<S> *>(t);
                size = len(t);
                break;
            }
            case ITR<S, M>::DIG:
            {
                const TREE<S, M> *u = reinterpret_cast<const TREE<S, M> *>(obj);
                const DIG<S, M> *d  = i->d;
                switch (d->tag)
                {
                    case DIG<S, M>::DIG_1:
                        d = dig(u, max);
                        break;
                    case DIG<S, M>::DIG_2:
                        d = (path == 0? dig(u, d->t[1], max): dig(d->t[0], u, max));
                        break;
                    case DIG<S, M>::DIG_3:
                        switch (path)
                        {
                            case 0: d = dig(u, d->t[1], d->t[2], max); break;
                            case 1: d = dig(d->t[0], u, d->t[2], max); break;
                            case 2: d = dig(d->t[0], d->t[1], u, max); break;
                            default: LIBFPP_UNREACHABLE();
                        }
                        break;
                    case DIG<S, M>::DIG_4:
                        switch (path)
                        {
                            case 0: d = dig(u, d->t[1], d->t[2], d->t[3], max); break;
                            case 1: d = dig(d->t[0], u, d->t[2], d->t[3], max); break;
                            case 2: d = dig(d->t[0], d->t[1], u, d->t[3], max); break;
                            case 3: d = dig(d->t[0], d->t[1], d->t[2], u, max); break;
                            default: LIBFPP_UNREACHABLE();
                        }
                        break;
                }
                obj = reinterpret_cast<const OBJECT<S> *>(d);
                size = len(d);
                break;
            }
            case ITR<S, M>::SEQ:
            {
                const SEQ<S, M> *s = i->s;
                switch (s->tag)
                {
                    case SEQ<S, M>::SINGLE:
                    {
                        const TREE<S, M> *u = reinterpret_cast<const TREE<S, M> *>(obj);
                        s = single(u, max);
                        break;
                    }
                    case SEQ<S, M>::DEEP:
                    {
                        switch (path)
                        {
                            case 0:
                            {
                                const DIG<S, M> *d = reinterpret_cast<const DIG<S, M> *>(obj);
                                s = deep(d, s->m, s->r, max);
                                break;
                            }
                            case 1:
                            {
                                const SEQ<S, M> *m = reinterpret_cast<const SEQ<S, M> *>(obj);
                                s = deep(s->l, m, s->r, max);
                                break;
                            }
                            case 2:
                            {
                                const DIG<S, M> *d = reinterpret_cast<const DIG<S, M> *>(obj);
                                s = deep(s->l, s->m, d, max);
                                break;
                            }
                            default: LIBFPP_UNREACHABLE();
                        }
                        break;
                    }
                    default:
                        LIBFPP_UNREACHABLE();
                }
                obj = reinterpret_cast<const OBJECT<S> *>(s);
                size = len(s);
                break;
            }
            default:
                LIBFPP_UNREACHABLE();
        }
        if (i->next == nullptr)
            return {i, obj};
        ssize_t lo = i->lo, hi = lo + size;
        if (LIBFPP_LIKELY(pos >= lo && pos < hi))
            return {i, obj};
        path = i->path;
        i = i->next;
    }
}

template <typename S, typename M = EMPTY>
static const ITR<S, M> *itr_move(const ITR<S, M> *i, const void *max = nullptr)
{
    ssize_t pos = i->pos;
    while (i->next != nullptr)
    {
        if (LIBFPP_LIKELY(pos >= ssize_t(i->lo) && pos < ssize_t(i->hi)))
            break;
        if (LIBFPP_UNLIKELY(i->dirty))
        {
            auto r = itr_restore(i, pos, max);
            i = r.a;
            i = S::template allocate<ITR<S, M>>(r.b, i->tag, i->path, i->lo, pos, i->next, true);
            break;
        }
        i = i->next;
    }
    ssize_t lo = i->lo;
    ssize_t hi = lo;
    if (pos < lo)
        goto oob;
    switch (i->tag)
    {
        case ITR<S, M>::ELEMENT: case ITR<S, M>::TREE:
            hi += slen(i->t);
            break;
        case ITR<S, M>::DIG: hi += slen(i->d); break;
        case ITR<S, M>::SEQ: hi += slen(i->s); break;
    }
    if (pos > hi)
        goto oob;
    while (true)
    {
        if (i->tag == ITR<S, M>::ELEMENT)
            break;
        if (i->tag == ITR<S, M>::SEQ && i->s->tag == SEQ<S, M>::NIL)
            break;
        switch (i->tag)
        {
            case ITR<S, M>::TREE:
            {
                const TREE<S, M> *t = i->t;
                if (pos - lo < slen(t->t[0]))
                {
                    i = S::template allocate<ITR<S, M>>(t->t[0], 0, lo, pos, i);
                    continue;
                }
                lo += slen(t->t[0]);
                if (t->tag == TREE<S, M>::TREE_2 || pos - lo < slen(t->t[1]))
                {
                    i = S::template allocate<ITR<S, M>>(t->t[1], 1, lo, pos, i);
                    continue;
                }
                lo += slen(t->t[1]);
                i = S::template allocate<ITR<S, M>>(t->t[2], 2, lo, pos, i);
                continue;
            }
            case ITR<S, M>::DIG:
            {
                const DIG<S, M> *d = i->d;
                for (size_t j = 0; ; j++)
                {
                    if (j == d->tag || pos - lo < slen(d->t[j]))
                    {
                        i = S::template allocate<ITR<S, M>>(d->t[j], j, lo, pos, i);
                        break;
                    }
                    lo += slen(d->t[j]);
                }
                continue;
            }
            case ITR<S, M>::SEQ:
            {
                const SEQ<S, M> *s = i->s;
                switch (s->tag)
                {
                    case SEQ<S, M>::SINGLE:
                        i = S::template allocate<ITR<S, M>>(s->t, 0, lo, pos, i);
                        continue;
                    case SEQ<S, M>::DEEP:
                        if (pos - lo < slen(s->l))
                        {
                            i = S::template allocate<ITR<S, M>>(s->l, 0, lo, pos, i);
                            continue;
                        }
                        lo += slen(s->l);
                        if (pos - lo < slen(s->m))
                        {
                            i = S::template allocate<ITR<S, M>>(s->m, 1, lo, pos, i);
                            continue;
                        }
                        lo += slen(s->m);
                        i = S::template allocate<ITR<S, M>>(s->r, 2, lo, pos, i);
                        continue;
                    default:
                        LIBFPP_UNREACHABLE();
                }
            }
            default:
                LIBFPP_UNREACHABLE();
        }
    }
oob:
    if (pos != ssize_t(i->pos))
    {
        const ITR<S, M> *j = i;
        i = S::template allocate<ITR<S, M>>(i->obj, i->tag, i->path, i->lo, pos, i->next);
        j->gc();
    }
    return i;
}

template <typename S>
const ITR<S> *INTERNAL<S>::itr_move(const ITR<S> *i, const void *arg)
{
    return F::itr_move<S>(i, arg);
}
template <typename S>
const ITR<S, MIN> *INTERNAL<S>::itr_move(const ITR<S, MIN> *i, const void *arg)
{
    return F::itr_move<S>(i, arg);
}
template <typename S>
const ITR<S, STR> *INTERNAL<S>::itr_move(const ITR<S, STR> *i, const void *arg)
{
    return F::itr_move<S>(i, arg);
}

template <typename S>
static result<const UTF8_ITR<S> *, char32_t> itr_str_slow_get(const UTF8_ITR<S> *i)
{
    if (i == nullptr)
    {
    error:
        LIBFPP_PANIC("iterator out-of-bounds");
    }
    i = itr_move(i);
    if (LIBFPP_UNLIKELY(i->pos < i->lo || i->pos > i->hi || i->tag != UTF8_ITR<S>::ELEMENT))
        goto error;
    size_t idx = i->pos - i->lo;
    const auto *t = i->t;
    if (LIBFPP_LIKELY(t->len == t->monoid.len))
        return {i, static_cast<char32_t>(t->str[idx])};
    else
        return {i, utf8_get(t->str, idx)};
}

template <typename S>
result<const UTF8_ITR<S> *, char32_t> INTERNAL<S>::itr_str_slow_get(const UTF8_ITR<S> *i)
{
    return F::itr_str_slow_get<S>(i);
}

template <typename S, typename M = EMPTY>
static const ITR<S, M> *itr_insert(const ITR<S, M> *i, size_t slack, const void *data, result<const TREE<S, M> *, const TREE<S, M> *, const TREE<S, M> *> (*insert)(const TREE<S, M> *, const TREE<S, M> *, size_t, const void *, size_t))
{
    if (i == nullptr)
    {
    error:
        LIBFPP_PANIC("iterator out-of-bounds");
    }
    i = itr_move(i);
    const auto *old = i;
    ssize_t pos = i->pos;
    size_t refcount = SIZE_MAX;
    if (LIBFPP_UNLIKELY(pos == 0 && i->tag == ITR<S, M>::SEQ && i->s->tag == SEQ<S, M>::NIL))
    {   // NIL special case:
        auto r = insert(nullptr, nullptr, 0, data, refcount);
        const auto *s = single(r.a);
        const auto *j = S::template allocate<ITR<S, M>>(s, 0, 0, 1, nullptr);
        old->gc();
        return j;
    }
    if (LIBFPP_UNLIKELY(pos < ssize_t(i->lo) || pos > ssize_t(i->hi) || i->tag != ITR<S, M>::ELEMENT))
        goto error;

    const TREE<S, M> *t = i->t, *u = nullptr;
    if (LIBFPP_LIKELY(t->len + t->size + slack < TREE<S, M>::DATA_SIZE))
    {   // FAST PATH:
        auto r = insert(i->t, nullptr, pos - i->lo, data, refcount);
        pos++;
        const auto *j = S::template allocate<ITR<S, M>>(r.a, i->path, i->lo, pos, i->next, true);
        old->gc();
        return j;
    }

    // MEDIUM PATH:
    size_t path = i->path;
    const auto *j = i->next;
    ssize_t idx = pos - j->lo;
    pos++;
    result<const TREE<S, M> *, const TREE<S, M> *, const TREE<S, M> *> r;
    const DIG<S, M> *d = nullptr;
    switch (j->tag)
    {
        case ITR<S, M>::TREE:
        {
            u = j->t;
            switch (u->tag)
            {
                case TREE<S, M>::TREE_2:
                {
                    r = (path == 0? insert(t, u->t[1], idx, data, refcount): insert(u->t[0], t, idx, data, refcount));
                    u = (r.c == nullptr? tree(r.a, r.b): tree(r.a, r.b, r.c));
                    const auto *k = S::template allocate<ITR<S, M>>(u, j->path, j->lo, pos, j->next, true);
                    old->gc();
                    return k;
                }
                case TREE<S, M>::TREE_3:
                {
                    switch (path)
                    {
                        case 0:
                            r = insert(t, u->t[1], idx, data, refcount);
                            t = (r.c == nullptr? tree(r.a, r.b, u->t[2]): tree(r.a, r.b));
                            u = (r.c == nullptr? nullptr: tree(r.c, u->t[2]));
                            break;
                        case 1:
                            r = insert(u->t[0], t, idx, data, refcount);
                            t = (r.c == nullptr? tree(r.a, r.b, u->t[2]): tree(r.a, r.b));
                            u = (r.c == nullptr? nullptr: tree(r.c, u->t[2]));
                            break;
                        case 2:
                            r = insert(u->t[1], t, idx - len(u->t[0]), data, refcount);
                            t = (r.c == nullptr? tree(u->t[0], r.a, r.b): tree(u->t[0], r.a));
                            u = (r.c == nullptr? nullptr: tree(r.b, r.c));
                            break;
                        default:
                            LIBFPP_UNREACHABLE();
                    }
                    if (u == nullptr)
                    {
                        const auto *k = S::template allocate<ITR<S, M>>(t, j->path, j->lo, pos, j->next, true);
                        old->gc();
                        return k;
                    }
                    break;
                }
                default:
                    LIBFPP_UNREACHABLE();
            }
            break;
        }
        case ITR<S, M>::DIG:
        {
            d = j->d;
            switch (d->tag)
            {
                case DIG<S, M>::DIG_1:
                {
                    r = insert(t, nullptr, idx, data, refcount);
                    d = (r.b == nullptr? dig(r.a): dig(r.a, r.b));
                    const auto *k = S::template allocate<ITR<S, M>>(d, j->path, j->lo, pos, j->next, true);
                    old->gc();
                    return k;
                }
                case DIG<S, M>::DIG_2:
                {
                    r = (path == 0? insert(t, d->t[1], idx, data, refcount): insert(d->t[0], t, idx, data, refcount));
                    d = (r.c == nullptr? dig(r.a, r.b): dig(r.a, r.b, r.c));
                    const auto *k = S::template allocate<ITR<S, M>>(d, j->path, j->lo, pos, j->next, true);
                    old->gc();
                    return k;
                }
                case DIG<S, M>::DIG_3:
                {
                    switch (path)
                    {
                        case 0:
                            r = insert(t, d->t[1], idx, data, refcount);
                            d = (r.c == nullptr? dig(r.a, r.b, d->t[2]): dig(r.a, r.b, r.c, d->t[2]));
                            break;
                        case 1:
                            r = insert(d->t[0], t, idx, data, refcount);
                            d = (r.c == nullptr? dig(r.a, r.b, d->t[2]): dig(r.a, r.b, r.c, d->t[2]));
                            break;
                        case 2:
                            r = insert(d->t[1], t, idx - len(d->t[0]), data, refcount);
                            d = (r.c == nullptr? dig(d->t[0], r.a, r.b): dig(d->t[0], r.a, r.b, r.c));
                            break;
                        default:
                            LIBFPP_UNREACHABLE();
                    }
                    const auto *k = S::template allocate<ITR<S, M>>(d, j->path, j->lo, pos, j->next, true);
                    old->gc();
                    return k;
                }
                case DIG<S, M>::DIG_4:
                {
                    bool left = (j->path == 0);
                    switch (path)
                    {
                        case 0: case 1:
                            r = (path == 0? insert(t, d->t[1], idx, data, refcount): insert(d->t[0], t, idx, data, refcount));
                            if (left)
                            {
                                t = (r.c == nullptr? nullptr: tree(d->t[2], d->t[3]));
                                d = (r.c == nullptr? dig(r.a, r.b, d->t[2], d->t[3]): dig(r.a, r.b, r.c));
                            }
                            else
                            {
                                t = (r.c == nullptr? nullptr: tree(r.a, r.b));
                                d = (r.c == nullptr? dig(r.a, r.b, d->t[2], d->t[3]): dig(r.c, d->t[2], d->t[3]));
                            }
                            break;
                        case 2:
                            r = insert(d->t[1], t, idx - len(d->t[0]), data, refcount);
                            if (left)
                            {
                                t = (r.c == nullptr? nullptr: tree(r.c, d->t[3]));
                                d = (r.c == nullptr? dig(d->t[0], r.a, r.b, d->t[3]): dig(d->t[0], r.a, r.b));
                            }
                            else
                            {
                                t = (r.c == nullptr? nullptr: tree(d->t[0], r.a));
                                d = (r.c == nullptr? dig(d->t[0], r.a, r.b, d->t[3]): dig(r.b, r.c, d->t[3]));
                            }
                            break;
                        case 3:
                            r = insert(d->t[2], t, idx - len(d->t[0]) - len(d->t[1]), data, refcount);
                            if (left)
                            {
                                t = (r.c == nullptr? nullptr: tree(r.b, r.c));
                                d = (r.c == nullptr? dig(d->t[0], d->t[1], r.a, r.b): dig(d->t[0], d->t[1], r.a));
                            }
                            else
                            {
                                t = (r.c == nullptr? nullptr: tree(d->t[0], d->t[1]));
                                d = (r.c == nullptr? dig(d->t[0], d->t[1], r.a, r.b): dig(r.a, r.b, r.c));
                            }
                            break;
                        default:
                            LIBFPP_UNREACHABLE();
                    }
                    if (t == nullptr)
                    {
                        const auto *k = S::template allocate<ITR<S, M>>(d, j->path, j->lo, pos, j->next, true);
                        old->gc();
                        return k;
                    }
                    break;
                }
            }
            break;
        }
        case ITR<S, M>::SEQ:
        {
            const auto *s = j->s;
            switch (s->tag)
            {
                case SEQ<S, M>::SINGLE:
                {
                    r = insert(t, nullptr, idx, data, refcount);
                    if (r.b == nullptr)
                        s = single(r.a);
                    else
                        s = deep(dig(r.a), nil<S, M>(), dig(r.b));
                    const auto *k = S::template allocate<ITR<S, M>>(s, j->path, j->lo, pos, j->next, true);
                    old->gc();
                    return k;
                }
                default:
                    LIBFPP_UNREACHABLE();
            }
            break;
        }
        default:
            LIBFPP_UNREACHABLE();
    }

    // SLOW PATH (rebalance):
    i = j;
    while (true)
    {
        size_t path = i->path;
        i = i->next;
        switch (i->tag)
        {
            case ITR<S, M>::TREE:
            {
                const auto *v = i->t;
                switch (v->tag)
                {
                    case TREE<S, M>::TREE_2:
                    {
                        t = (path == 0? tree(t, u, v->t[1]): tree(v->t[0], t, u));
                        const auto *j = S::template allocate<ITR<S, M>>(t, i->path, i->lo, pos, i->next, true);
                        old->gc();
                        return j;
                    }
                    case TREE<S, M>::TREE_3:
                        switch (path)
                        {
                            case 0:
                                t = tree(t, u);
                                u = tree(v->t[1], v->t[2]);
                                continue;
                            case 1:
                                t = tree(v->t[0], t);
                                u = tree(u, v->t[2]);
                                continue;
                            case 2:
                                u = tree(t, u);
                                t = tree(v->t[0], v->t[1]);
                                continue;
                            default:
                                LIBFPP_UNREACHABLE();
                        }
                    default:
                        LIBFPP_UNREACHABLE();
                }
            }
            case ITR<S, M>::DIG:
                d = i->d;
                switch (d->tag)
                {
                    case DIG<S, M>::DIG_1:
                    {
                        d = dig(t, u);
                        const auto *j = S::template allocate<ITR<S, M>>(d, i->path, i->lo, pos, i->next, true);
                        old->gc();
                        return j;
                    }
                    case DIG<S, M>::DIG_2:
                    {
                        d = (path == 0? dig(t, u, d->t[1]): dig(d->t[0], t, u));
                        const auto *j = S::template allocate<ITR<S, M>>(d, i->path, i->lo, pos, i->next, true);
                        old->gc();
                        return j;
                    }
                    case DIG<S, M>::DIG_3:
                    {
                        switch (path)
                        {
                            case 0: d = dig(t, u, d->t[1], d->t[2]); break;
                            case 1: d = dig(d->t[0], t, u, d->t[2]); break;
                            case 2: d = dig(d->t[0], d->t[1], t, u); break;
                            default: LIBFPP_UNREACHABLE();
                        }
                        const auto *j = S::template allocate<ITR<S, M>>(d, i->path, i->lo, pos, i->next, true);
                        old->gc();
                        return j;
                    }
                    case DIG<S, M>::DIG_4:
                    {
                        const DIG<S, M> *e;
                        bool left = (i->path == 0);
                        switch (path)
                        {
                            case 0:
                                e = (left? dig(t, u, d->t[1]): dig(d->t[1], d->t[2], d->t[3]));
                                t = (left? tree(d->t[2], d->t[3]): tree(t, u));
                                break;
                            case 1:
                                e = (left? dig(d->t[0], t, u): dig(u, d->t[2], d->t[3]));
                                t = (left? tree(d->t[2], d->t[3]): tree(d->t[0], t));
                                break;
                            case 2:
                                e = (left? dig(d->t[0], d->t[1], t): dig(t, u, d->t[3]));
                                t = (left? tree(u, d->t[3]): tree(d->t[0], d->t[1]));
                                break;
                            case 3:
                                e = (left? dig(d->t[0], d->t[1], d->t[2]): dig(d->t[2], t, u));
                                t = (left? tree(t, u): tree(d->t[0], d->t[1]));
                                break;
                            default:
                                LIBFPP_UNREACHABLE();
                        }
                        d = e;
                        continue;
                    }
                }
                LIBFPP_UNREACHABLE();
            case ITR<S, M>::SEQ:
            {
                const auto *s = i->s;
                switch (s->tag)
                {
                    case SEQ<S, M>::SINGLE:
                    {
                        s = deep(dig(t), nil<S, M>(), dig(u));
                        const auto *j = S::template allocate<ITR<S, M>>(s, i->path, i->lo, pos, i->next, true);
                        old->gc();
                        return j;
                    }
                    case SEQ<S, M>::DEEP:
                    {
                        switch (path)
                        {
                            case 0:
                            {
                                const auto *m = seq_push_front(s->m, t);
                                s = deep(d, m, s->r);
                                break;
                            }
                            case 2:
                            {
                                const auto *m = seq_push_back(s->m, t);
                                s = deep(s->l, m, d);
                                break;
                            }
                            default:
                                LIBFPP_UNREACHABLE();
                        }
                        const auto *j = S::template allocate<ITR<S, M>>(s, i->path, i->lo, pos, i->next, true);
                        old->gc();
                        return j;
                    }
                    default:
                        LIBFPP_UNREACHABLE();
                }
            }
            default:
                LIBFPP_UNREACHABLE();
        }
    }
}

template <typename S>
const ITR<S> *INTERNAL<S>::itr_insert(const ITR<S> *i, const void *data, result<const TREE<S> *, const TREE<S> *, const TREE<S> *> (*insert)(const TREE<S> *, const TREE<S> *, size_t, const void *, size_t))
{
    return F::itr_insert(i, 0, data, insert);
}
template <typename S>
const ITR<S, MIN> *INTERNAL<S>::itr_insert(const ITR<S, MIN> *i, const void *data, result<const TREE<S, MIN> *, const TREE<S, MIN> *, const TREE<S, MIN> *> (*insert)(const TREE<S, MIN> *, const TREE<S, MIN> *, size_t, const void *, size_t))
{
    return F::itr_insert(i, 0, data, insert);
}
template <typename S>
const UTF8_ITR<S> *INTERNAL<S>::itr_str_insert(const UTF8_ITR<S> *i, char32_t c)
{
    return F::itr_insert(i, /*slack=*/12, &c, utf8_tree_insert);
}
template <typename S>
const UTF8_ITR<S> *INTERNAL<S>::itr_str_assign(const UTF8_ITR<S> *i, char32_t c)
{
    if (i == nullptr || (i->tag == UTF8_ITR<S>::SEQ && i->s->len == 0))
        LIBFPP_PANIC("iterator out-of-bounds");
    return F::itr_insert(i, /*slack=*/12, &c, utf8_tree_assign);
}

template <typename S, typename M = EMPTY>
static const ITR<S, M> *itr_erase(const ITR<S, M> *i, result<const TREE<S, M> *, const TREE<S, M> *> (*erase)(const TREE<S, M> *, const TREE<S, M> *, size_t), const void *max)
{
    if (i == nullptr)
    {
    error:
        LIBFPP_PANIC("iterator out-of-bounds");
    }
    i = itr_move(i, max);
    const auto *old = i;
    ssize_t pos = i->pos;
    if (LIBFPP_UNLIKELY(pos < ssize_t(i->lo) || pos >= ssize_t(i->hi) || i->tag != ITR<S, M>::ELEMENT))
        goto error;
    const auto *t = i->t;
    if (LIBFPP_LIKELY(t->len >= TREE<S, M>::DATA_SIZE / (2 * t->size) && t->len > 1))
    {   // FAST PATH:
        auto r = erase(t, nullptr, i->pos - i->lo);
        const auto *j = S::template allocate<ITR<S, M>>(r.a, i->path, i->lo, pos, i->next, true);
        old->gc();
        return j;
    }

    // MEDIUM PATH:
    size_t path = i->path;
    const auto *j = i->next;
    ssize_t idx = pos - j->lo;
    result<const TREE<S, M> *, const TREE<S, M> *> r;
    switch (j->tag)
    {
        case ITR<S, M>::TREE:
        {
            const auto *u = j->t;
            switch (u->tag)
            {
                case TREE<S, M>::TREE_2:
                    r = (path == 0? erase(t, u->t[1], idx): erase(u->t[0], t, idx));
                    if (r.b != nullptr)
                    {
                        const auto *k = S::template allocate<ITR<S, M>>(tree(r.a, r.b, max), j->path, j->lo, pos, j->next, true);
                        old->gc();
                        return k;
                    }
                    t = r.a;
                    break;
                case TREE<S, M>::TREE_3:
                {
                    switch (path)
                    {
                        case 0:
                            r = erase(t, u->t[1], idx);
                            u = (r.b == nullptr? tree(r.a, u->t[2], max): tree(r.a, r.b, u->t[2], max));
                            break;
                        case 1:
                            r = erase(u->t[0], t, idx);
                            u = (r.b == nullptr? tree(r.a, u->t[2], max): tree(r.a, r.b, u->t[2], max));
                            break;
                        case 2:
                            r = erase(u->t[1], t, idx - len(u->t[0]));
                            u = (r.b == nullptr? tree(u->t[0], r.a, max): tree(u->t[0], r.a, r.b, max));
                            break;
                        default:
                            LIBFPP_UNREACHABLE();
                    }
                    const auto *k = S::template allocate<ITR<S, M>>(u, j->path, j->lo, pos, j->next, true);
                    old->gc();
                    return k;
                }
                default:
                    LIBFPP_UNREACHABLE();
            }
            break;
        }
        case ITR<S, M>::DIG:
        {
            const auto *d = j->d;
            switch (d->tag)
            {
                case DIG<S, M>::DIG_1:
                    r = erase(t, nullptr, idx);
                    if (r.a != nullptr)
                    {
                        d = dig(r.a, max);
                        const auto *k = S::template allocate<ITR<S, M>>(d, j->path, j->lo, pos, j->next, true);
                        old->gc();
                        return k;
                    }
                    t = r.a;
                    break;
                case DIG<S, M>::DIG_2:
                {
                    r = (path == 0? erase(t, d->t[1], idx): erase(d->t[0], t, idx));
                    d = (r.b == nullptr? dig(r.a, max): dig(r.a, r.b, max));
                    const auto *k = S::template allocate<ITR<S, M>>(d, j->path, j->lo, pos, j->next, true);
                    old->gc();
                    return k;
                }
                case DIG<S, M>::DIG_3:
                {
                    switch (path)
                    {
                        case 0:
                            r = erase(t, d->t[1], idx);
                            d = (r.b == nullptr? dig(r.a, d->t[2], max): dig(r.a, r.b, d->t[2], max));
                            break;
                        case 1:
                            r = erase(d->t[0], t, idx);
                            d = (r.b == nullptr? dig(r.a, d->t[2], max): dig(r.a, r.b, d->t[2], max));
                            break;
                        case 2:
                            r = erase(d->t[1], t, idx - len(d->t[0]));
                            d = (r.b == nullptr? dig(d->t[0], r.a, max): dig(d->t[0], r.a, r.b, max));
                            break;
                        default:
                            LIBFPP_UNREACHABLE();
                    }
                    const auto *k = S::template allocate<ITR<S, M>>(d, j->path, j->lo, pos, j->next, true);
                    old->gc();
                    return k;
                }
                case DIG<S, M>::DIG_4:
                {
                    switch (path)
                    {
                        case 0:
                            r = erase(t, d->t[1], idx);
                            d = (r.b == nullptr? dig(r.a, d->t[2], d->t[3], max): dig(r.a, r.b, d->t[2], d->t[3], max));
                            break;
                        case 1:
                            r = erase(d->t[0], t, idx);
                            d = (r.b == nullptr? dig(r.a, d->t[2], d->t[3], max): dig(r.a, r.b, d->t[2], d->t[3], max));
                            break;
                        case 2:
                            r = erase(d->t[1], t, idx - len(d->t[0]));
                            d = (r.b == nullptr? dig(d->t[0], r.a, d->t[3], max): dig(d->t[0], r.a, r.b, d->t[3], max));
                            break;
                        case 3:
                            r = erase(d->t[2], t, idx - len(d->t[0]) - len(d->t[1]));
                            d = (r.b == nullptr? dig(d->t[0], d->t[1], r.a, max): dig(d->t[0], d->t[1], r.a, r.b, max));
                            break;
                        default:
                            LIBFPP_UNREACHABLE();
                    }
                    const auto *k = S::template allocate<ITR<S, M>>(d, j->path, j->lo, pos, j->next, true);
                    old->gc();
                    return k;
                }
            }
            break;
        }
        case ITR<S, M>::SEQ:
        {
            const auto *s = j->s;
            switch (s->tag)
            {
                case SEQ<S, M>::SINGLE:
                    r = erase(t, nullptr, idx);
                    if (r.a != nullptr)
                    {
                        s = single(r.a);
                        const auto *k = S::template allocate<ITR<S, M>>(s, j->path, j->lo, pos, j->next, true);
                        old->gc();
                        return k;
                    }
                    t = r.a;
                    break;
                default:
                    LIBFPP_UNREACHABLE();
            }
            break;
        }
        default:
            LIBFPP_UNREACHABLE();
    }

    // SLOW PATH (rebalance):
    bool reduced = true, left = true;
    const SEQ<S, M> *m = nullptr, *n;
    while (j->next != nullptr)
    {
        path = j->path;
        j = j->next;
        switch (j->tag)
        {
            case ITR<S, M>::TREE:
            {
                const auto *u = j->t;
                switch (u->tag)
                {
                    case TREE<S, M>::TREE_2:
                        t = tree_fix_2(u, path, t, &reduced, max);
                        break;
                    case TREE<S, M>::TREE_3:
                        t = tree_fix_3(u, path, t, &reduced, max);
                        break;
                    default:
                        LIBFPP_UNREACHABLE();
                }
                if (!reduced)
                {
                    const auto *k = S::template allocate<ITR<S, M>>(t, j->path, j->lo, pos, j->next, true);
                    old->gc();
                    return k;
                }
                break;
            }
            case ITR<S, M>::DIG:
            {
                const auto *d = j->d;
                switch (d->tag)
                {
                    case DIG<S, M>::DIG_1:
                        d = nullptr;
                        break;
                    case DIG<S, M>::DIG_2:
                        d = dig_fix_2(d, path, t, &reduced, max);
                        break;
                    case DIG<S, M>::DIG_3:
                        d = dig_fix_3(d, path, t, &reduced, max);
                        break;
                    case DIG<S, M>::DIG_4:
                        d = dig_fix_4(d, path, t, &reduced, max);
                        break;
                }
                if (!reduced)
                {
                    const auto *k = S::template allocate<ITR<S, M>>(d, j->path, j->lo, pos, j->next, true);
                    old->gc();
                    return k;
                }
                break;
            }
            case ITR<S, M>::SEQ:
            {
                const auto *s = j->s;
                switch (s->tag)
                {
                    case SEQ<S, M>::SINGLE:
                        m = nil<S, M>();
                        break;
                    case SEQ<S, M>::DEEP:
                        switch (path)
                        {
                            case 0:
                                left = true;
                                m = s = deep(s->m, s->r, max);
                                break;
                            case 1:
                            {
                                if (left)
                                {
                                    const auto *d = s->l;
                                    switch (d->tag)
                                    {
                                        case DIG<S, M>::DIG_1: s = deep(dig(d->t[0], t, max), m, s->r, max); break;
                                        case DIG<S, M>::DIG_2: s = deep(dig(d->t[0], d->t[1], t, max), m, s->r, max); break;
                                        case DIG<S, M>::DIG_3: s = deep(dig(d->t[0], d->t[1], d->t[2], t, max), m, s->r, max); break;
                                        case DIG<S, M>::DIG_4:
                                            t = tree(d->t[3], t, max);
                                            n = seq_push_front(m, t, max);
                                            m->gc();
                                            s = deep(dig(d->t[0], d->t[1], d->t[2], max), n, s->r, max);
                                            break;
                                    }
                                }
                                else
                                {
                                    const auto *d = s->r;
                                    switch (d->tag)
                                    {
                                        case DIG<S, M>::DIG_1: s = deep(s->l, m, dig(t, d->t[0], max), max); break;
                                        case DIG<S, M>::DIG_2: s = deep(s->l, m, dig(t, d->t[0], d->t[1], max), max); break;
                                        case DIG<S, M>::DIG_3: s = deep(s->l, m, dig(t, d->t[0], d->t[1], d->t[2], max), max); break;
                                        case DIG<S, M>::DIG_4:
                                            t = tree(t, d->t[0], max);
                                            n = seq_push_back(m, t, max);
                                            m->gc();
                                            s = deep(s->l, n, dig(d->t[1], d->t[2], d->t[3], max), max);
                                            break;
                                    }
                                }
                                const auto *k = S::template allocate<ITR<S, M>>(s, j->path, j->lo, pos, j->next, true);
                                old->gc();
                                return k;
                            }
                            case 2:
                            {
                                left = false;
                                m = s = deep(s->l, s->m, max);
                                break;
                            }
                        }
                        break;
                    default:
                        LIBFPP_UNREACHABLE();
                }
                break;
            }
            default:
                LIBFPP_UNREACHABLE();
        }
    }
    m = (m == nullptr? nil<S, M>(): m);
    const auto *k = S::template allocate<ITR<S, M>>(m, 0, 0, pos, nullptr, true);
    old->gc();
    return k;
}

template <typename S>
const ITR<S> *INTERNAL<S>::itr_erase(const ITR<S> *i, result<const TREE<S> *, const TREE<S> *> (*erase)(const TREE<S> *, const TREE<S> *, size_t), const void *arg)
{
    return F::itr_erase(i, erase, arg);
}
template <typename S>
const ITR<S, MIN> *INTERNAL<S>::itr_erase(const ITR<S, MIN> *i, result<const TREE<S, MIN> *, const TREE<S, MIN> *> (*erase)(const TREE<S, MIN> *, const TREE<S, MIN> *, size_t), const void *arg)
{
    return F::itr_erase(i, erase, arg);
}

template <typename S, typename M = EMPTY>
static result<const ITR<S, M> *, const ITR<S, M> *> itr_split(const ITR<S, M> *i, const TREE<S, M> *(*l)(const TREE<S, M> *, size_t), const TREE<S, M> *(*r)(const TREE<S, M> *, size_t))
{
    if (i == nullptr)
    {
    error:
        LIBFPP_PANIC("iterator out-of-bounds");
    }
    i = itr_move(i);
    const auto *old = i;
    if (LIBFPP_UNLIKELY(i->pos == 0 && i->tag == ITR<S, M>::SEQ && i->s->tag == SEQ<S, M>::NIL))
    {   // NIL special case:
        return {i, i};
    }
    ssize_t pos = i->pos;
    if (LIBFPP_UNLIKELY(pos < ssize_t(i->lo) || pos > ssize_t(i->hi) || i->tag != ITR<S, M>::ELEMENT))
        goto error;
    size_t idx = pos - i->lo;
    const auto *t = i->t;
    bool dirty = i->dirty;
    size_t path = i->path;
    i = i->next;
    while (i->tag == ITR<S, M>::TREE)
    {
        if (LIBFPP_LIKELY(!dirty))
            t = i->t;
        else
        {
            const auto *u = i->t;
            switch (u->tag)
            {
                case TREE<S, M>::TREE_2:
                    switch (path)
                    {
                        case 0: t = tree(t, u->t[1]); break;
                        case 1: t = tree(u->t[0], t); break;
                        default: LIBFPP_UNREACHABLE();
                    }
                    break;
                case TREE<S, M>::TREE_3:
                    switch (path)
                    {
                        case 0: t = tree(t, u->t[1], u->t[2]); break;
                        case 1: t = tree(u->t[0], t, u->t[2]); break;
                        case 2: t = tree(u->t[0], u->t[1], t); break;
                        default: LIBFPP_UNREACHABLE();
                    }
                    break;
                default:
                    LIBFPP_UNREACHABLE();
            }
        }
        idx   = pos - i->lo;
        dirty = (dirty || i->dirty);
        path  = i->path;
        i     = i->next;
    }

    const DIG<S, M> *dl = nullptr, *dr = nullptr;
    if (i->tag == ITR<S, M>::DIG)
    {
        const auto *d = i->d;
        switch (d->tag)
        {
            case DIG<S, M>::DIG_1:
                break;
            case DIG<S, M>::DIG_2:
                switch (path)
                {
                    case 0:
                        if (r) dr = dig(d->t[1]);
                        break;
                    case 1:
                        if (l) dl = dig(d->t[0]);
                        break;
                    default:
                        LIBFPP_UNREACHABLE();
                }
                break;
            case DIG<S, M>::DIG_3:
                switch (path)
                {
                    case 0:
                        if (r) dr = dig(d->t[1], d->t[2]);
                        break;
                    case 1:
                        if (l) dl = dig(d->t[0]);
                        if (r) dr = dig(d->t[2]);
                        break;
                    case 2:
                        if (l) dl = dig(d->t[0], d->t[1]);
                        break;
                    default:
                        LIBFPP_UNREACHABLE();
                }
                break;
            case DIG<S, M>::DIG_4:
                switch (path)
                {
                    case 0:
                        if (r) dr = dig(d->t[1], d->t[2], d->t[3]);
                        break;
                    case 1:
                        if (l) dl = dig(d->t[0]);
                        if (r) dr = dig(d->t[2], d->t[3]);
                        break;
                    case 2:
                        if (l) dl = dig(d->t[0], d->t[1]);
                        if (r) dr = dig(d->t[3]);
                        break;
                    case 3:
                        if (l) dl = dig(d->t[0], d->t[1], d->t[2]);
                        break;
                    default:
                        LIBFPP_UNREACHABLE();
                }
                break;
        }
        path = i->path;
        i    = i->next;
    }
    const SEQ<S, M> *sl = nullptr, *sr = nullptr;
    for (; i != nullptr; i = i->next)
    {
        const auto *s = i->s;
        switch (s->tag)
        {
            case SEQ<S, M>::SINGLE:
                if (l) sl = nil<S, M>();
                if (r) sr = nil<S, M>();
                break;
            case SEQ<S, M>::DEEP:
                switch (path)
                {
                    case 0:
                        if (r) sr = (dr == nullptr? deep(s->m, s->r): deep(dr, s->m, s->r));
                        if (l) sl = (dl == nullptr? nil<S, M>(): dig_to_seq(dl));
                        if (dl != nullptr) dl->gc();
                        break;
                    case 1:
                    {
                        switch (t->tag)
                        {
                            case TREE<S, M>::TREE_2:
                                if (idx < len(t->t[0]))
                                {
                                    const auto *old = sl;
                                    if (l) sl = deep(s->l, sl);
                                    if (r) sr = deep(dig(t->t[1]), sr, s->r);
                                    t = t->t[0];
                                    if (old != nullptr) old->gc();
                                }
                                else
                                {
                                    idx -= len(t->t[0]);
                                    const auto *old = sr;
                                    if (l) sl = deep(s->l, sl, dig(t->t[0]));
                                    if (r) sr = deep(sr, s->r);
                                    t = t->t[1];
                                    if (old != nullptr) old->gc();
                                }
                                break;
                            case TREE<S, M>::TREE_3:
                                if (idx < len(t->t[0]))
                                {
                                    const auto *old = sl;
                                    if (l) sl = deep(s->l, sl);
                                    if (r) sr = deep(dig(t->t[1], t->t[2]), sr, s->r);
                                    t = t->t[0];
                                    if (old != nullptr) old->gc();
                                }
                                else
                                {
                                    idx -= len(t->t[0]);
                                    if (idx < len(t->t[1]))
                                    {
                                        if (l) sl = deep(s->l, sl, dig(t->t[0]));
                                        if (r) sr = deep(dig(t->t[2]), sr, s->r);
                                        t = t->t[1];
                                    }
                                    else
                                    {
                                        idx -= len(t->t[1]);
                                        const auto *old = sr;
                                        if (l) sl = deep(s->l, sl, dig(t->t[0], t->t[1]));
                                        if (r) sr = deep(sr, s->r);
                                        t = t->t[2];
                                        if (old != nullptr) old->gc();
                                    }
                                }
                                break;
                            default:
                                LIBFPP_UNREACHABLE();
                        }
                        break;
                    }
                    case 2:
                        if (r) sr = (dr == nullptr? nil<S, M>(): dig_to_seq(dr));
                        if (l) sl = (dl == nullptr? deep(s->l, s->m): deep(s->l, s->m, dl));
                        if (dr != nullptr) dr->gc();
                        break;
                    default:
                        LIBFPP_UNREACHABLE();
                }
                break;
            default:
                LIBFPP_UNREACHABLE();
        }
        path = i->path;
    }

    const ITR<S, M> *il = nullptr, *ir = nullptr;
    if (l)
    {
        const auto *tl = l(t, idx);
        if (tl != nullptr)
        {
            const auto *old = sl;
            sl = seq_push_back(sl, tl);
            old->gc();
        }
        il = S::template allocate<ITR<S, M>>(sl, 0, 0, pos, nullptr);
    }
    if (r)
    {
        const auto *tr = r(t, idx);
        if (tr != nullptr)
        {
            const auto *old = sr;
            sr = seq_push_front(sr, tr);
            old->gc();
        }
        ir = S::template allocate<ITR<S, M>>(sr, 0, 0, 0, nullptr);
    }
    if (dirty)
        t->gc();
    old->gc();
    return {il, ir};
}


template <typename S>
const ITR<S> *INTERNAL<S>::itr_left(const ITR<S> *i, const TREE<S> *(*left)(const TREE<S> *, size_t))
{
    auto r = F::itr_split(i, left, (false? left: nullptr));
    return r.a;
}
template <typename S>
const ITR<S, MIN> *INTERNAL<S>::itr_left(const ITR<S, MIN> *i, const TREE<S, MIN> *(*left)(const TREE<S, MIN> *, size_t))
{
    auto r = F::itr_split(i, left, (false? left: nullptr));
    return r.a;
}
template <typename S>
const UTF8_ITR<S> *INTERNAL<S>::itr_str_left(const UTF8_ITR<S> *i)
{
    auto r = F::itr_split(i, F::utf8_tree_left<S>, (false? F::utf8_tree_right<S>: nullptr));
    return r.a;
}
template <typename S>
const ITR<S> *INTERNAL<S>::itr_right(const ITR<S> *i, const TREE<S> *(*right)(const TREE<S> *, size_t))
{
    auto r = F::itr_split(i, (false? right: nullptr), right);
    return r.b;
}
template <typename S>
const ITR<S, MIN> *INTERNAL<S>::itr_right(const ITR<S, MIN> *i, const TREE<S, MIN> *(*right)(const TREE<S, MIN> *, size_t))
{
    auto r = F::itr_split(i, (false? right: nullptr), right);
    return r.b;
}
template <typename S>
const UTF8_ITR<S> *INTERNAL<S>::itr_str_right(const UTF8_ITR<S> *i)
{
    auto r = F::template itr_split<S>(i, (false? F::utf8_tree_left<S>: nullptr), F::utf8_tree_right<S>);
    return r.b;
}
template <typename S>
const UTF8_ITR<S> *INTERNAL<S>::itr_str_erase(const UTF8_ITR<S> *i)
{
    return F::itr_erase(i, utf8_tree_erase, nullptr);
}

template <typename S>
static const UTF8_ITR<S> *itr_insert(const UTF8_ITR<S> *i, const UTF8_SEQ<S> *s, const UTF8_TREE<S> *(*left)(const UTF8_TREE<S> *, size_t), const UTF8_TREE<S> *(*right)(const UTF8_TREE<S> *, size_t))
{
    if (s->len == 0) return i;
    size_t slen = len(s);
    auto r = itr_split<S>(i, left, right);
    const auto *t = INTERNAL<S>::str_append(r.a->s, s);
    s             = INTERNAL<S>::str_append(t, r.b->s);
    const auto *k = S::template allocate<UTF8_ITR<S>>(s, 0, 0, i->pos + slen, nullptr);
    r.a->gc();
    r.b->gc();
    t->gc();
    return k;
}

template <typename S>
static const UTF8_ITR<S> *itr_insert(const UTF8_ITR<S> *i, const char *cstr, const UTF8_TREE<S> *(*left)(const UTF8_TREE<S> *, size_t), const UTF8_TREE<S> *(*right)(const UTF8_TREE<S> *, size_t))
{
    if (cstr[0] == '\0') return i;
    auto r = itr_split<S>(i, left, right);
    const auto *t = INTERNAL<S>::str_push_back(r.a->s, cstr, nullptr);
    size_t clen = t->len - r.a->s->len;
    const auto *s = INTERNAL<S>::str_append(t, r.b->s);
    const auto *k = S::template allocate<UTF8_ITR<S>>(s, 0, 0, i->pos + clen, nullptr);
    r.a->gc();
    r.b->gc();
    t->gc();
    return k;
}

template <typename S>
const UTF8_ITR<S> *INTERNAL<S>::itr_str_insert(const UTF8_ITR<S> *i, const UTF8_SEQ<S> *s)
{
    return F::itr_insert(i, s, utf8_tree_left<S>, utf8_tree_right<S>);
}
template <typename S>
const UTF8_ITR<S> *INTERNAL<S>::itr_str_insert(const UTF8_ITR<S> *i, const char *cstr)
{
    return F::itr_insert(i, cstr, utf8_tree_left<S>, utf8_tree_right<S>);
}

template <typename S, typename M = EMPTY>
static const ITR<S, M> *itr_replace(const ITR<S, M> *i, const ITR<S, M> *j, const SEQ<S, M> *s, const TREE<S, M> *(*left)(const TREE<S, M> *, size_t), const TREE<S, M> *(*right)(const TREE<S, M> *, size_t))
{
    auto r1 = itr_split<S, M>(i, left, nullptr);
    auto r2 = itr_split<S, M>(j, nullptr, right);
    const auto *t = INTERNAL<S>::str_append(r1.a->s, s);
    s             = INTERNAL<S>::str_append(t, r2.b->s);
    const auto *k = S::template allocate<ITR<S, M>>(s, 0, 0, i->pos, nullptr);
    r1.a->gc();
    r2.b->gc();
    t->gc();
    return k;
}

template <typename S, typename M = EMPTY>
static const ITR<S, M> *itr_replace(const ITR<S, M> *i, const ITR<S, M> *j, const char *cstr, const TREE<S, M> *(*left)(const TREE<S, M> *, size_t), const TREE<S, M> *(*right)(const TREE<S, M> *, size_t))
{
    auto r1 = itr_split<S, M>(i, left, nullptr);
    auto r2 = itr_split<S, M>(j, nullptr, right);
    const auto *t = INTERNAL<S>::str_push_back(r1.a->s, cstr, nullptr);
    const auto *s = INTERNAL<S>::str_append(t, r2.b->s);
    const auto *k = S::template allocate<ITR<S, M>>(s, 0, 0, i->pos, nullptr);
    r1.a->gc();
    r2.b->gc();
    t->gc();
    return k;
}

template <typename S>
const UTF8_ITR<S> *INTERNAL<S>::itr_str_replace(const UTF8_ITR<S> *i, const UTF8_ITR<S> *j, const UTF8_SEQ<S> *s)
{
    return itr_replace(i, j, s, utf8_tree_left<S>, utf8_tree_right<S>);
}
template <typename S>
const UTF8_ITR<S> *INTERNAL<S>::itr_str_replace(const UTF8_ITR<S> *i, const UTF8_ITR<S> *j, const char *cstr)
{
    return itr_replace(i, j, cstr, utf8_tree_left<S>, utf8_tree_right<S>);
}

template <typename S, typename M = EMPTY>
static const ITR<S, M> *itr_erase(const ITR<S, M> *i, ssize_t n, const TREE<S, M> *(*left)(const TREE<S, M> *, size_t), const TREE<S, M> *(*right)(const TREE<S, M> *, size_t))
{
    if (i == nullptr)
        return nullptr;
    if (LIBFPP_UNLIKELY(n == 0))
        return i;
    if (LIBFPP_UNLIKELY(n == 1))
        return itr_erase<S, M>(i, utf8_tree_erase, nullptr);
    auto r = itr_split<S, M>(i, left, right);
    const SEQ<S, M> *s, *t;
    if (LIBFPP_LIKELY(n > 0))
    {
        t = INTERNAL<S>::str_right(r.b->s, n);
        s = INTERNAL<S>::str_append(r.a->s, t);
    }
    else
    {
        t = INTERNAL<S>::str_left(r.a->s, len(r.b->s) + n);
        s = INTERNAL<S>::str_append(t, r.a->s);
    }
    const auto *k = S::template allocate<ITR<S, M>>(s, 0, 0, i->pos, nullptr);
    t->gc();
    r.a->gc();
    r.b->gc();
    return k;
}

template <typename S, typename M = EMPTY>
static const ITR<S, M> *itr_erase(const ITR<S, M> *i, const ITR<S, M> *j, const TREE<S, M> *(*left)(const TREE<S, M> *, size_t), const TREE<S, M> *(*right)(const TREE<S, M> *, size_t))
{
    if (i == nullptr)
    {
        if (j == nullptr)
            return nullptr;
        auto r = itr_split<S, M>(j, nullptr, right);
        return r.b;
    }
    else if (j == nullptr)
    {
        auto r = itr_split<S, M>(i, left, nullptr);
        return r.a;
    }
    auto r1 = itr_split<S, M>(i, left, nullptr);
    auto r2 = itr_split<S, M>(j, nullptr, right);
    const auto *s = INTERNAL<S>::str_append(r1.a->s, r2.b->s);
    const auto *k = S::template allocate<ITR<S, M>>(s, 0, 0, i->pos, nullptr);
    r1.a->gc();
    r2.b->gc();
    return k;
}

template <typename S>
const UTF8_ITR<S> *INTERNAL<S>::itr_str_erase(const UTF8_ITR<S> *i, ssize_t n)
{
    return F::itr_erase(i, n, utf8_tree_left<S>, utf8_tree_right<S>);
}
template <typename S>
const UTF8_ITR<S> *INTERNAL<S>::itr_str_erase(const UTF8_ITR<S> *i, const UTF8_ITR<S> *j)
{
    return F::itr_erase(i, j, utf8_tree_left<S>, utf8_tree_right<S>);
}

template <typename S, typename M = EMPTY>
static const ITR<S, M> *itr_slice(const ITR<S, M> *i, ssize_t n, const TREE<S, M> *(*left)(const TREE<S, M> *, size_t), const TREE<S, M> *(*right)(const TREE<S, M> *, size_t))
{
    if (i == nullptr)
        return nullptr;
    const SEQ<S, M> *s;
    const ITR<S, M> *old = nullptr;
    ssize_t pos = 0;
    if (LIBFPP_UNLIKELY(n == 0)) s = nil<S, M>();
    else if (LIBFPP_LIKELY(n > 0))
    {
        auto r = itr_split<S, M>(i, nullptr, right);
        s = INTERNAL<S>::str_left(r.b->s, n);
        old = r.b;
    }
    else
    {
        auto r = itr_split<S, M>(i, left, nullptr);
        s = INTERNAL<S>::str_right(r.a->s, n);
        old = r.a;
        pos = len(s);
    }
    const auto *k = S::template allocate<ITR<S, M>>(s, 0, 0, pos, nullptr);
    if (old != nullptr)
        old->gc();
    return k;
}

template <typename S>
const UTF8_ITR<S> *INTERNAL<S>::itr_str_slice(const UTF8_ITR<S> *i, ssize_t n)
{
    return itr_slice(i, n, utf8_tree_left<S>, utf8_tree_right<S>);
}
template <typename S>
const UTF8_ITR<S> *INTERNAL<S>::itr_str_slice(const UTF8_ITR<S> *i,const UTF8_ITR<S> *j)
{
    ssize_t n = (i == nullptr? 0: (j == nullptr? 0: j->pos - i->pos));
    return itr_slice(i, n, utf8_tree_left<S>, utf8_tree_right<S>);
}

template <typename S, typename M = EMPTY>
static const SEQ<S, M> *itr_revert(const ITR<S, M> *i, const void *arg)
{
    if (i == nullptr)
        return nil<S, M>();
    while (i->next != nullptr)
    {
        if (LIBFPP_UNLIKELY(i->dirty))
        {
            auto r = itr_restore(i, -1, arg);
            return reinterpret_cast<const SEQ<S, M> *>(r.b);
        }
        i = i->next;
    }
    return i->s;
}

template <typename S>
const SEQ<S> *INTERNAL<S>::itr_revert(const ITR<S> *i, const void *arg)
{
    return F::itr_revert<S>(i, arg);
}
template <typename S>
const SEQ<S, MIN> *INTERNAL<S>::itr_revert(const ITR<S, MIN> *i, const void *arg)
{
    return F::itr_revert<S>(i, arg);
}
template <typename S>
const SEQ<S, STR> *INTERNAL<S>::itr_revert(const UTF8_ITR<S> *i, const void *arg)
{
    return F::itr_revert<S>(i, arg);
}

static LIBFPP_PURE char32_t utf8_tree_get(const UTF8_TREE<LOCAL> *t, size_t idx)
{
    size_t len;
    switch (t->tag)
    {
        case UTF8_TREE<LOCAL>::ELEMENT:
            return utf8_get(t->str, idx);
        case UTF8_TREE<LOCAL>::TREE_2: case UTF8_TREE<LOCAL>::TREE_3:
            if (t->len == t->monoid.len)
            {
                auto r = tree_get(reinterpret_cast<const TREE<LOCAL> *>(t), idx);
                return *r.a->template at<char>(r.b);
            }
            len = STR::get(t->t[0]);
            if (idx < len)
                return utf8_tree_get(t->t[0], idx);
            idx -= len;
            if (t->tag == UTF8_TREE<LOCAL>::TREE_2)
                return utf8_tree_get(t->t[1], idx);
            len = STR::get(t->t[1]);
            if (idx < len)
                return utf8_tree_get(t->t[1], idx);
            idx -= len;
            return utf8_tree_get(t->t[2], idx);
    }
    LIBFPP_UNREACHABLE();
}
static LIBFPP_PURE char32_t utf8_dig_get(const UTF8_DIG<LOCAL> *d, size_t idx)
{
    if (d->len == d->monoid.len)
    {
        auto r = dig_get(reinterpret_cast<const DIG<LOCAL> *>(d), idx);
        return *r.a->template at<char>(r.b);
    }
    for (size_t i = 0; i < d->tag; i++)
    {
        size_t len = STR::get(d->t[i]);
        if (idx < len)
            return utf8_tree_get(d->t[i], idx);
        idx -= len;
    }
    return utf8_tree_get(d->t[d->tag], idx);
    LIBFPP_UNREACHABLE();
}
static LIBFPP_PURE char32_t utf8_seq_get(const UTF8_SEQ<LOCAL> *s, size_t idx)
{
    if (s->len == s->monoid.len)
    {
        auto r = seq_get_2(reinterpret_cast<const SEQ<LOCAL> *>(s), idx);
        return *r.a->template at<char>(r.b);
    }
    size_t len;
    switch (s->tag)
    {
        case SEQ<LOCAL>::SINGLE:
            return utf8_tree_get(s->t, idx);
        case SEQ<LOCAL>::DEEP:
            len = STR::get(s->l);
            if (idx < len)
                return utf8_dig_get(s->l, idx);
            idx -= len;
            len = STR::get(s->m);
            if (idx < len)
                return utf8_seq_get(s->m, idx);
            idx -= len;
            return utf8_dig_get(s->r, idx);
        default:
            LIBFPP_UNREACHABLE();
    }
}

template <typename S>
LIBFPP_PURE char32_t INTERNAL<S>::str_get(const UTF8_SEQ<S> *s, size_t idx)
{
    if (idx >= STR::get(s)) LIBFPP_PANIC("index out-of-bounds");
    return utf8_seq_get(reinterpret_cast<const UTF8_SEQ<S> *>(s), idx);
}

template <typename S>
static const UTF8_SEQ<S> *utf8_push_back(const UTF8_SEQ<S> *s, const UTF8_TREE<S> *t)
{
    switch (s->tag)
    {
        case UTF8_SEQ<S>::NIL:
            return single(t);
        case UTF8_SEQ<S>::SINGLE:
            return deep(dig(s->t), nil<S, STR>(), dig(t));
        case UTF8_SEQ<S>::DEEP:
        {
            const auto *d = s->r;
            switch (d->tag)
            {
                case UTF8_DIG<S>::DIG_1: return deep(s->l, s->m, dig(d->t[0], t));
                case UTF8_DIG<S>::DIG_2: return deep(s->l, s->m, dig(d->t[0], d->t[1], t));
                case UTF8_DIG<S>::DIG_3: return deep(s->l, s->m, dig(d->t[0], d->t[1], d->t[2], t));
                case UTF8_DIG<S>::DIG_4:
                {
                    if (s->l->tag == UTF8_DIG<S>::DIG_1 && s->m->tag == UTF8_SEQ<S>::NIL)
                        return deep(dig(s->l->t[0], d->t[0], d->t[1], d->t[2]), nil<S, STR>(), dig(d->t[3], t));
                    const UTF8_TREE<S> *t3 = tree(d->t[0], d->t[1], d->t[2]);
                    const UTF8_SEQ<S> *m = utf8_push_back(s->m, t3);
                    return deep(s->l, m, dig(d->t[3], t));
                }
            }
        }
    }
    LIBFPP_UNREACHABLE();
}

template <typename S>
const UTF8_SEQ<S> *INTERNAL<S>::str_push_back(const UTF8_SEQ<S> *s, const UTF8_TREE<S> *t)
{
    return utf8_push_back(s, t);
}

template <typename S>
static const UTF8_SEQ<S> *utf8_replace_back(const UTF8_SEQ<S> *s, const UTF8_TREE<S> *t)
{
    switch (s->tag)
    {
        case UTF8_SEQ<S>::SINGLE:
            return single(t);
        case UTF8_SEQ<S>::DEEP:
        {
            const auto *d = s->r;
            switch (d->tag)
            {
                case UTF8_DIG<S>::DIG_1: return deep(s->l, s->m, dig(t));
                case UTF8_DIG<S>::DIG_2: return deep(s->l, s->m, dig(d->t[0], t));
                case UTF8_DIG<S>::DIG_3: return deep(s->l, s->m, dig(d->t[0], d->t[1], t));
                case UTF8_DIG<S>::DIG_4: return deep(s->l, s->m, dig(d->t[0], d->t[1], d->t[2], t));
                default:
                    LIBFPP_UNREACHABLE();
            }
        }
        default:
            LIBFPP_UNREACHABLE();
    }
}

template <typename S>
const UTF8_SEQ<S> *INTERNAL<S>::str_replace_back(const UTF8_SEQ<S> *s, const UTF8_TREE<S> *t)
{
    return utf8_replace_back(s, t);
}

template <typename S>
static const UTF8_SEQ<S> *utf8_slow_push_back(const UTF8_SEQ<S> *s, const UTF8_TREE<S> *t, char32_t c)
{
    UTF8_TREE<S> *u = S::template allocate<UTF8_TREE<S>>(sizeof(unsigned char));
    u->monoid.len = 0;
    u->len = 0;
    size_t clen = utf8_encode_len(c);
    if (LIBFPP_LIKELY(t->len + clen <= UTF8_TREE<S>::STR_SIZE))
    {
        memcpy(u->data, t->data, t->len);
        u->len  = t->len;
        u->monoid.len = t->monoid.len;
    }
    utf8_encode(u->str + u->len, c);
    u->len += clen;
    u->monoid.len++;
    if (LIBFPP_LIKELY(u->len != clen))
        return INTERNAL<S>::str_replace_back(s, u);
    else
        return INTERNAL<S>::str_push_back(s, u);
}

template <typename S>
const UTF8_SEQ<S> *INTERNAL<S>::str_slow_push_back(const UTF8_SEQ<S> *s, const UTF8_TREE<S> *t, char32_t c)
{
    return utf8_slow_push_back(s, t, c);
}

template <typename S>
const UTF8_SEQ<S> *utf8_push_back(const UTF8_SEQ<S> *s, const char *cstr, const char *cmax)
{
    if (cstr[0] == '\0' || cstr >= cmax)
        return s;
    size_t len = strnlen(cstr, cmax - cstr);
    if (s->len + len >= STR_MAX) LIBFPP_PANIC("string is full");
    const char *cptr = cstr;
    if (s->tag != UTF8_SEQ<S>::NIL)
    {
        const UTF8_DIG<S> *d = s->r;
        const UTF8_TREE<S> *t = d->t[d->tag];
        const char *cend = cstr + (UTF8_TREE<S>::STR_SIZE - t->len);
        cend = (cend > cmax? cmax: cend);
        size_t clen, cnum = 0;
        while (*cptr != '\0' && cptr + (clen = utf8_decode_len(cptr)) <= cend)
        {
            cptr += clen;
            cnum++;
        }
        if (LIBFPP_LIKELY(cptr != cstr))
        {
            UTF8_TREE<S> *u = S::template allocate<UTF8_TREE<S>>(sizeof(unsigned char));
            memcpy(u->data, t->data, t->len);
            memcpy(u->data + t->len, cstr, cptr - cstr);
            u->len = t->len + (cptr - cstr);
            u->monoid.len = t->monoid.len + cnum;
            s = INTERNAL<S>::str_replace_back(s, u);
            cstr = cptr;
        }
    }
    while (*cptr != '\0' && cptr < cmax)
    {
        const UTF8_SEQ<S> *old = s;
        const char *cend = cstr + UTF8_TREE<S>::STR_SIZE;
        cend = (cend > cmax? cmax: cend);
        size_t clen, cnum = 0;
        while (*cptr != '\0' && cptr + (clen = utf8_decode_len(cptr)) <= cend)
        {
            cptr += clen;
            cnum++;
        }
        UTF8_TREE<S> *u = S::template allocate<UTF8_TREE<S>>(sizeof(unsigned char));
        memcpy(u->data, cstr, cptr - cstr);
        u->len = (cptr - cstr);
        u->monoid.len = cnum;
        s = INTERNAL<S>::str_push_back(s, u);
        old->gc();
        cstr = cptr;
    }
    return s;
}

template <typename S>
const UTF8_SEQ<S> *INTERNAL<S>::str_push_back(const UTF8_SEQ<S> *s, const char *cstr, const char *cmax)
{
    return utf8_push_back(s, cstr, (cmax == nullptr? reinterpret_cast<const char *>(UINTPTR_MAX): cmax));
}

template <typename S>
const UTF8_SEQ<S> *utf8_push_front(const UTF8_SEQ<S> *s, const char *cstr, const char *cmax)
{
    if (cstr[0] == '\0' || cstr >= cmax)
        return s;
    size_t len = strnlen(cstr, cmax - cstr);
    if (s->len + len >= STR_MAX) LIBFPP_PANIC("string is full");
    const char *cend = cstr + len;
    const char *cptr = cend;
    if (s->tag != UTF8_SEQ<S>::NIL)
    {
        const UTF8_DIG<S> *d = s->l;
        const UTF8_TREE<S> *t = d->t[0];
        const char *cbegin = cend - (UTF8_TREE<S>::STR_SIZE - t->len);
        cbegin = (cbegin < cstr? cstr: cbegin);
        size_t clen, cnum = 0;
        while (cptr > cstr && cptr - (clen = utf8_decode_len_backward(cptr, cstr)) >= cbegin)
        {
            cptr -= clen;
            cnum++;
        }
        if (LIBFPP_LIKELY(cptr != cend))
        {
            UTF8_TREE<S> *u = S::template allocate<UTF8_TREE<S>>(sizeof(unsigned char));
            memcpy(u->data, cptr, cend - cptr);
            memcpy(u->data + (cend - cptr), t->data, t->len);
            u->len = (cend - cptr) + t->len;
            u->monoid.len = t->monoid.len + cnum;
            s = INTERNAL<S>::str_replace_front(s, u);
            cend = cptr;
        }
    }
    while (cptr > cstr)
    {
        const UTF8_SEQ<S> *old = s;
        const char *cbegin = cend - UTF8_TREE<S>::STR_SIZE;
        cbegin = (cbegin < cstr? cstr: cbegin);
        size_t clen, cnum = 0;
        while (cptr > cstr && cptr - (clen = utf8_decode_len_backward(cptr, cstr)) >= cbegin)
        {
            cptr -= clen;
            cnum++;
        }
        UTF8_TREE<S> *u = S::template allocate<UTF8_TREE<S>>(sizeof(unsigned char));
        memcpy(u->data, cptr, cend - cptr);
        u->len = (cend - cptr);
        u->monoid.len = cnum;
        s = INTERNAL<S>::str_push_front(s, u);
        old->gc();
        cend = cptr;
    }
    return s;
}

template <typename S>
const UTF8_SEQ<S> *INTERNAL<S>::str_push_front(const UTF8_SEQ<S> *s, const char *cstr, const char *cmax)
{
    return utf8_push_front(s, cstr, (cmax == nullptr? reinterpret_cast<const char *>(UINTPTR_MAX): cmax));
}

template <typename S>
static const UTF8_SEQ<S> *utf8_push_front(const UTF8_SEQ<S> *s, const UTF8_TREE<S> *t)
{
    switch (s->tag)
    {
        case UTF8_SEQ<S>::NIL:
            return single(t);
        case UTF8_SEQ<S>::SINGLE:
            return deep(dig(t), nil<S, STR>(), dig(s->t));
        case UTF8_SEQ<S>::DEEP:
        {
            const auto *d = s->l;
            switch (d->tag)
            {
                case UTF8_DIG<S>::DIG_1: return deep(dig(t, d->t[0]), s->m, s->r);
                case UTF8_DIG<S>::DIG_2: return deep(dig(t, d->t[0], d->t[1]), s->m, s->r);
                case UTF8_DIG<S>::DIG_3: return deep(dig(t, d->t[0], d->t[1], d->t[2]), s->m, s->r);
                case UTF8_DIG<S>::DIG_4:
                {
                    if (s->r->tag == UTF8_DIG<S>::DIG_1 && s->m->tag == UTF8_SEQ<S>::NIL)
                        return deep(dig(t, d->t[0]), nil<S, STR>(), dig(d->t[1], d->t[2], d->t[3], s->r->t[0]));
                    const UTF8_TREE<S> *t3 = tree(d->t[1], d->t[2], d->t[3]);
                    const UTF8_SEQ<S> *m = utf8_push_front(s->m, t3);
                    return deep(dig(t, d->t[0]), m, s->r);
                }
                default:
                    LIBFPP_UNREACHABLE();
            }
        }
    }
    LIBFPP_UNREACHABLE();
}

template <typename S>
const UTF8_SEQ<S> *INTERNAL<S>::str_push_front(const UTF8_SEQ<S> *s, const UTF8_TREE<S> *t)
{
    return utf8_push_front<S>(s, t);
}

template <typename S>
static const UTF8_SEQ<S> *utf8_replace_front(const UTF8_SEQ<S> *s, const UTF8_TREE<S> *t)
{
    switch (s->tag)
    {
        case UTF8_SEQ<S>::SINGLE:
            return single(t);
        case UTF8_SEQ<S>::DEEP:
        {
            const auto *d = s->l;
            switch (d->tag)
            {
                case UTF8_DIG<S>::DIG_1: return deep(dig(t), s->m, s->r);
                case UTF8_DIG<S>::DIG_2: return deep(dig(t, d->t[1]), s->m, s->r);
                case UTF8_DIG<S>::DIG_3: return deep(dig(t, d->t[1], d->t[2]), s->m, s->r);
                case UTF8_DIG<S>::DIG_4: return deep(dig(t, d->t[1], d->t[2], d->t[3]), s->m, s->r);
                default:
                    LIBFPP_UNREACHABLE();
            }
        }
        default:
            LIBFPP_UNREACHABLE();
    }
}

template <typename S>
const UTF8_SEQ<S> *INTERNAL<S>::str_replace_front(const UTF8_SEQ<S> *s, const UTF8_TREE<S> *t)
{
    return utf8_replace_front<S>(s, t);
}

template <typename S>
static const UTF8_SEQ<S> *utf8_slow_push_front(const UTF8_SEQ<S> *s, const UTF8_TREE<S> *t, char32_t c)
{
    UTF8_TREE<S> *u = S::template allocate<UTF8_TREE<S>>(sizeof(unsigned char));
    u->monoid.len = 0;
    u->len = 0;
    size_t clen = utf8_encode_len(c);
    if (LIBFPP_LIKELY(t->len + clen <= UTF8_TREE<S>::STR_SIZE))
    {
        memcpy(u->str + clen, t->str, t->len);
        u->len  = t->len;
        u->monoid.len = t->monoid.len;
    }
    utf8_encode(u->str, c);
    u->len += clen;
    u->monoid.len++;
    if (LIBFPP_LIKELY(u->len != clen))
        return INTERNAL<S>::str_replace_front(s, u);
    else
        return INTERNAL<S>::str_push_front(s, u);
}

template <typename S>
const UTF8_SEQ<S> *INTERNAL<S>::str_slow_push_front(const UTF8_SEQ<S> *s, const UTF8_TREE<S> *t, char32_t c)
{
    return utf8_slow_push_front(s, t, c);
}

template <typename S>
static typename string<S>::iterator str_find(typename string<S>::iterator i, typename string<S>::iterator j)
{
    typename string<S>::iterator end;
    if (j == end)
        return i;
    for (; i != end; ++i)
    {
        if (*i != *j)
            continue;
        auto i1 = i, j1 = j;
        for (++i1, ++j1; i1 != end && j1 != end; ++j1, ++i1)
        {
            if (*i1 != *j1)
                break;
        }
        if (j1 == end)
            return i;
    }
    return i;
}

template <typename S>
static typename string<S>::iterator str_find(typename string<S>::iterator i, const char *cstr)
{
    if (cstr == nullptr || cstr[0] == '\0')
        return i;
    char32_t c = utf8_decode(cstr);
    cstr += utf8_decode_len(cstr);

    typename string<S>::iterator end;
    for (; i != end; ++i)
    {
        if (*i != c)
            continue;
        auto i1 = i;
        const char *cptr = cstr;
        for (++i1; i1 != end && cptr[0] != '\0'; ++i1)
        {
            if (*i1 != utf8_decode(cptr))
                break;
            cptr += utf8_decode_len(cptr);
        }
        if (cptr[0] == '\0')
            return i;
    }
    return i;
}

template <typename S>
static typename string<S>::iterator str_find(typename string<S>::iterator i, char32_t c)
{
    typename string<S>::iterator end;
    for (; i != end; ++i)
    {
        if (*i == c)
            return i;
    }
    return i;
}

template <typename S>
void string<S>::iterator::find(const string<S> &t)
{
    *this = str_find<S>(*this, t.begin());
}
template <typename S>
void string<S>::iterator::find(const char *cstr)
{
    *this = str_find<S>(*this, cstr);
}
template <typename S>
void string<S>::iterator::find(char32_t c)
{
    *this = str_find<S>(*this, c);
}

template void string<LOCAL>::iterator::find(const string<LOCAL> &t);
template void string<LOCAL>::iterator::find(const char *cstr);
template void string<LOCAL>::iterator::find(char32_t c);

#include <fcntl.h>
#include <unistd.h>

template <typename S>
void str_read(string<S> &s, const char *filename)
{
    int fd = open(filename, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        LIBFPP_PANIC("failed to open file");
    s.clear();

    ssize_t hi = 0;
    char buf[BUFSIZ + 1];
    while (true)
    {
        ssize_t r = read(fd, buf + hi, BUFSIZ - hi);
        if (r == 0)
            break;
        if (r < 0)
            LIBFPP_PANIC("read failed");

        // Handle UTF8 crossing read boundaries:
        hi += r;
        ssize_t end = hi;
        for (; hi > 0 && (buf[hi-1] & 0xC0) == 0x80; hi--)
            ;
        if (hi > 0 && (buf[hi-1] & 0xC0) == 0xC0)
            hi--;
        if (hi > 0)
        {
            char tmp = buf[hi];
            buf[hi] = '\0';
            s.push_back(buf);
            buf[hi] = tmp;
            std::memmove(buf, buf + hi, end - hi);
            hi = end - hi;
        }
    }
    close(fd);
}

template<typename S>
void string<S>::read(const char *filename)
{
    str_read<S>(*this, filename);
}

template void string<LOCAL>::read(const char *filename);

template <typename S>
static LIBFPP_PURE int utf8_tree_compare(const UTF8_TREE<S> *t, const char **cstr)
{
    int cmp;
    switch (t->tag)
    {
        case UTF8_TREE<S>::ELEMENT:
        {
            const char *cptr = t->str, *dptr = *cstr;
            for (size_t i = 0; i < t->monoid.len; i++)
            {
                char32_t c = utf8_decode(cptr), d = utf8_decode(dptr);
                if (c < d) return -1;
                if (c > d) return 1;
                size_t clen = utf8_decode_len(cptr);
                cptr += clen;
                dptr += clen;
            }
            *cstr = dptr;
            return 0;
        }
        case UTF8_TREE<S>::TREE_2: case UTF8_TREE<S>::TREE_3:
            cmp = utf8_tree_compare(t->t[0], cstr);
            if (cmp != 0) return cmp;
            cmp = utf8_tree_compare(t->t[1], cstr);
            if (cmp != 0) return cmp;
            if (t->tag == UTF8_TREE<S>::TREE_3)
                return utf8_tree_compare(t->t[2], cstr);
            return 0;
    }
    LIBFPP_UNREACHABLE();
}

template <typename S>
static LIBFPP_PURE int utf8_dig_compare(const UTF8_DIG<S> *d, const char **cstr)
{
    for (size_t i = 0; i <= d->tag; i++)
    {
        int cmp = utf8_tree_compare(d->t[i], cstr);
        if (cmp != 0) return cmp;
    }
    return 0;
}

template <typename S>
static LIBFPP_PURE int utf8_seq_compare(const UTF8_SEQ<S> *s, const char **cstr)
{
    int cmp;
    switch (s->tag)
    {
        case UTF8_SEQ<S>::NIL:    return 0;
        case UTF8_SEQ<S>::SINGLE: return utf8_tree_compare(s->t, cstr);
        case UTF8_SEQ<S>::DEEP:
            cmp = utf8_dig_compare(s->l, cstr);
            if (cmp != 0) return cmp;
            cmp = utf8_seq_compare(s->m, cstr);
            if (cmp != 0) return cmp;
            return utf8_dig_compare(s->r, cstr);
    }
    LIBFPP_UNREACHABLE();
}

template <typename S>
LIBFPP_PURE int INTERNAL<S>::str_compare(const UTF8_SEQ<S> *s, const char *cstr)
{
    if (cstr == nullptr) return 1;
    int cmp = utf8_seq_compare(s, &cstr);
    if (cmp != 0) return cmp;
    if (cstr[0] != '\0') return 1;
    return 0;
}

template <typename S>
const void *MIN::get_max(const TREE<S, MIN> *t, const void *arg)
{
    switch (t->tag)
    {
        case TREE<S, MIN>::ELEMENT:
        {
            max_t m = reinterpret_cast<max_t>(arg);
            const void *max = reinterpret_cast<const void *>(t->data);
            for (size_t i = 1; i < t->len; i++)
            {
                const void *elem = reinterpret_cast<const void *>(t->data + i * t->size);
                max = m(max, elem);
            }
            return max;
        }
        default:
            return t->monoid.max;
    }
    LIBFPP_UNREACHABLE();
}

template <typename S>
const void *MIN::init_max(const TREE<S, MIN> *t, const void *arg)
{
    if (arg == nullptr) return nullptr;
    max_t m = reinterpret_cast<max_t>(arg);
    const void *max;
    switch (t->tag)
    {
        case TREE<S, MIN>::ELEMENT:
            return get_max(t, arg);
        case TREE<S, MIN>::TREE_2: case TREE<S, MIN>::TREE_3:
            max = m(get_max(t->t[0], arg), get_max(t->t[1], arg));
            if (t->tag == TREE<S, MIN>::TREE_3)
                max = m(max, get_max(t->t[2], arg));
            return max;
    }
    LIBFPP_UNREACHABLE();
}

template <typename S>
const void *MIN::init_max(const DIG<S, MIN> *d, const void *arg)
{
    if (arg == nullptr) return nullptr;
    max_t m = reinterpret_cast<max_t>(arg);
    const void *max = get_max(d->t[0], arg);
    for (size_t i = 1; i <= d->tag; i++)
        max = m(max, get_max(d->t[i], arg));
    return max;
}

template <typename S>
const void *MIN::init_max(const SEQ<S, MIN> *s, const void *arg)
{
    if (arg == nullptr) return nullptr;
    max_t m = reinterpret_cast<max_t>(arg);
    const void *max = nullptr;
    switch (s->tag)
    {
        case SEQ<S, MIN>::NIL:
            return nullptr;
        case SEQ<S, MIN>::SINGLE:
            return get_max(s->t, arg);
        case SEQ<S, MIN>::DEEP:
            max = get_max(s->l, arg);
            if (s->m->tag != SEQ<S, MIN>::NIL)
                max = m(max, get_max(s->m, arg));
            max = m(max, get_max(s->r, arg));
            return max;
    }
    LIBFPP_UNREACHABLE();
}

template struct INTERNAL<LOCAL>;

}           /* namespace F */

