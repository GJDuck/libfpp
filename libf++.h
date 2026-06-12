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
 *
 * LibF++ is a highly experimental persistent container library for C++.
 *
 * LibF++ provides persistent containers, including persistent versions of:
 *   - vector
 *   - multiset
 *   - set
 *   - multimap
 *   - map
 *   - string (unicode strings)
 * The libF++ versions aim to preserve the same algorithmic complexity (but not
 * the constant factors) as the STL counterparts, or as much as is possible.
 * 
 * LibF++ also provides persistent iterators, including iterators into all of
 * the above. Persistent iterators are unique in that iterators use value
 * semantics, rather than the reference semantics of the STL. This means that
 * mutation via iterators is allowed, but operates on a persistent local copy of
 * the container, rather than a reference to the original.
 *
 * For more information, please see our PLDI'2026 paper:
 *
 *    Yihe Li, Gregory J. Duck, Persistent Iterators with Value Semantics,
 *    Programming Language Design and Implementation (PLDI), 2026
 */

#ifndef __LIBFPP_H
#define __LIBFPP_H

#include <mutex>
#include <new>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#define LIBFPP_UNREACHABLE()    __builtin_unreachable()
#define LIBFPP_LIKELY(...)      __builtin_expect((__VA_ARGS__), 1)
#define LIBFPP_UNLIKELY(...)    __builtin_expect((__VA_ARGS__), 0)
#define LIBFPP_PURE             /*disabled*/
#define LIBFPP_INLINE           __attribute__((__always_inline__)) inline
#define LIBFPP_NOINLINE         __attribute__((__noinline__))
#define LIBFPP_NORETURN         __attribute__((__noreturn__))
#define LIBFPP_STRING(s)        LIBFPP_STRING_2(s)
#define LIBFPP_STRING_2(s)      #s
#define LIBFPP_PANIC(msg)       F::panic(__FILE__ ": " LIBFPP_STRING(__LINE__) ": " msg)

namespace F
{

/*
 * [INTERNAL]
 */
constexpr size_t ALLOC_SIZE = 64;
constexpr size_t TAG_BITS   = 8;
constexpr size_t SIZE_BITS  = 8;
constexpr size_t LEN_BITS   = 64 - SIZE_BITS - TAG_BITS;
constexpr size_t LEN_MAX    = (1ull << LEN_BITS)-1;
constexpr size_t STR_MAX    = LEN_MAX - 4;

/*
 * [INTERNAL]
 */
constexpr char GREY[]       = "\33[1;30m";
constexpr char RED[]        = "\33[31m";
constexpr char GREEN[]      = "\33[32m";
constexpr char YELLOW[]     = "\33[33m";
constexpr char BLUE[]       = "\33[34m";
constexpr char MAGENTA[]    = "\33[35m";
constexpr char CYAN[]       = "\33[36m";
constexpr char OFF[]        = "\33[0m";

/*
 * [INTERNAL]
 */
template <typename S> struct OBJECT;
struct LOCAL;
struct SHARE;
extern thread_local OBJECT<LOCAL> *LOCAL_HEAP;
extern OBJECT<SHARE>              *SHARE_HEAP;
extern std::mutex                  SHARE_HEAP_MUTEX;

/*
 * [INTERNAL]
 */
extern void init();
extern LIBFPP_NORETURN void panic(const char *msg);

/**
 * Thread-local allocator
 */
struct LOCAL
{
private:
    static inline uintptr_t &next(OBJECT<LOCAL> *node)
    {
        return reinterpret_cast<uintptr_t *>(node)[1];
    }

    static inline OBJECT<LOCAL> *pop()
    {
        OBJECT<LOCAL> *node = LOCAL_HEAP;
        uintptr_t nxt = next(node) + ALLOC_SIZE + reinterpret_cast<uintptr_t>(node);
        LOCAL_HEAP = reinterpret_cast<OBJECT<LOCAL> *>(nxt);
        return node;
    }

    static inline void push(OBJECT<LOCAL> *node)
    {
        next(node) = reinterpret_cast<uintptr_t>(LOCAL_HEAP) - ALLOC_SIZE - reinterpret_cast<uintptr_t>(node);
        LOCAL_HEAP = node;
    }

    static inline void *xmalloc(size_t size)
    {
        void *ptr = std::malloc(size);
        if (ptr == nullptr)
            LIBFPP_PANIC("malloc failed");
        return ptr;
    }

public:
    static void init(void *heap)
    {
        LOCAL_HEAP = reinterpret_cast<OBJECT<LOCAL> *>(heap);
    }

    template <typename T, typename... Args>
    static inline T *allocate(Args&&... args)
    {
        if constexpr (sizeof(T) <= ALLOC_SIZE)
            return new (pop()) T(args...);
        else
            return new (xmalloc(sizeof(T))) T(args...);
    }

    template <typename T>
    static inline void deallocate(const T *ptr)
    {
        if constexpr (sizeof(T) <= ALLOC_SIZE)
            push(reinterpret_cast<OBJECT<LOCAL> *>(const_cast<T *>(ptr)));
        else
            std::free(const_cast<T *>(ptr));
    }

    static inline void ref(size_t &refcount)
    {
        refcount++;
    }

    static inline bool deref(size_t &refcount)
    {
        refcount--;
        return refcount == 0;
    }

    static inline void *malloc(size_t size)
    {
        return xmalloc(size);
    }

    static inline void free(void *ptr)
    {
        std::free(ptr);
    }
};

/**
 * Thread-safe global allocator
 */
struct SHARE
{
    /*
     * This implementation uses a global free-list protected by a global mutex.
     * A better implementation (with less contention) remains future work.
     */

private:
    static inline uintptr_t &next(OBJECT<SHARE> *node)
    {
        return reinterpret_cast<uintptr_t *>(node)[1];
    }

    static inline OBJECT<SHARE> *pop()
    {
        std::lock_guard<std::mutex> lock(SHARE_HEAP_MUTEX);
        OBJECT<SHARE> *node = SHARE_HEAP;
        uintptr_t nxt = next(node) + ALLOC_SIZE + reinterpret_cast<uintptr_t>(node);
        SHARE_HEAP = reinterpret_cast<OBJECT<SHARE> *>(nxt);
        return node;
    }

    static inline void push(OBJECT<SHARE> *node)
    {
        std::lock_guard<std::mutex> lock(SHARE_HEAP_MUTEX);
        next(node) = reinterpret_cast<uintptr_t>(SHARE_HEAP)
                   - ALLOC_SIZE
                   - reinterpret_cast<uintptr_t>(node);
        SHARE_HEAP = node;
    }

    static inline void *xmalloc(size_t size)
    {
        void *ptr = std::malloc(size);
        if (ptr == nullptr)
            LIBFPP_PANIC("malloc failed");
        return ptr;
    }

public:
    static inline void init(void *heap)
    {
        std::lock_guard<std::mutex> lock(SHARE_HEAP_MUTEX);
        SHARE_HEAP = reinterpret_cast<OBJECT<SHARE> *>(heap);
    }

    template <typename T, typename... Args>
    static inline T *allocate(Args&&... args)
    {
        if constexpr (sizeof(T) <= ALLOC_SIZE)
            return new (pop()) T(args...);
        else
            return new (xmalloc(sizeof(T))) T(args...);
    }

    template <typename T>
    static inline void deallocate(const T *ptr)
    {
        if constexpr (sizeof(T) <= ALLOC_SIZE)
            push(reinterpret_cast<OBJECT<SHARE> *>(const_cast<T *>(ptr)));
        else
            std::free(const_cast<T *>(ptr));
    }

    static inline void ref(size_t &refcount)
    {
        __atomic_add_fetch(&refcount, 1, __ATOMIC_RELAXED);
    }

    static inline bool deref(size_t &refcount)
    {
        return __atomic_sub_fetch(&refcount, 1, __ATOMIC_ACQ_REL) == 0;
    }

    static inline void *malloc(size_t size)
    {
        return xmalloc(size);
    }

    static inline void free(void *ptr)
    {
        std::free(ptr);
    }
};

/*
 * [INTERNAL] Object base
 */
template <typename S>
struct OBJECT
{
    mutable size_t refcount;

    void ref() const
    {
        S::ref(refcount);
    }
    bool deref() const
    {
        return S::deref(refcount);
    }
};

/*
 * [INTERNAL] 2-3 tree structure.
 */
struct EMPTY;
template <typename S, typename M = EMPTY>
struct TREE : OBJECT<S>
{
    enum TAG : size_t
    {
        ELEMENT,
        TREE_2,
        TREE_3,
    };

    TAG tag:TAG_BITS;
    size_t size:SIZE_BITS;
    size_t len:LEN_BITS;

    static constexpr size_t DATA_SIZE  = ALLOC_SIZE - sizeof(OBJECT<S>) - sizeof(size_t);
    static constexpr size_t STR_SIZE   = DATA_SIZE - sizeof(M);

       union
    {
        struct
        {
            const TREE *t[3];
            uint8_t pad[DATA_SIZE - 3*sizeof(TREE *) - sizeof(M)];
            M monoid;
        };
        uint8_t data[DATA_SIZE];
        char    str[STR_SIZE];
    };

    TREE(size_t size) : tag(ELEMENT), size(size), len(0)
    {
        ;
    }
    TREE(const TREE *t1, const TREE *t2, const void *arg = nullptr) : tag(TREE_2), size(0), len(t1->len + t2->len), t{t1, t2}, monoid(this, arg)
    {
        t1->ref(); t2->ref();
    }
    TREE(const TREE *t1, const TREE *t2, const TREE *t3, const void *arg = nullptr) : tag(TREE_3), size(0), len(t1->len + t2->len + t3->len), t{t1, t2, t3}, monoid(this, arg)
    {
        t1->ref(); t2->ref(); t3->ref();
    }

    ~TREE()
    {
        if (tag != ELEMENT)
        {
            t[0]->deref();
            t[1]->deref();
            if (tag == TREE_3)
                t[2]->deref();
        }
    }
    void deref() const
    {
        if (LIBFPP_UNLIKELY(OBJECT<S>::deref()))
        {
            auto *self = const_cast<TREE<S, M> *>(this);
            self->~TREE();
            S::deallocate(self);
        }
    }
    void gc() const
    {
        auto *self = const_cast<TREE<S, M> *>(this);
        if (self->refcount != 0)
            return;
        self->~TREE();
        S::deallocate(self);
    }
    template <typename T>
    void DEREF() const
    {
        if (LIBFPP_LIKELY(!OBJECT<S>::deref()))
            return;
        auto *self = const_cast<TREE<S, M> *>(this);
        switch (tag)
        {
            case ELEMENT:
                for (size_t i = 0; i < self->len; i++)
                    (self->template at<T>(i))->~T();
                break;
            case TREE_3:
                t[2]->template DEREF<T>();
                /* Fallthrough */
            case TREE_2:
                t[0]->template DEREF<T>();
                t[1]->template DEREF<T>();
                break;
        }
        S::deallocate(self);
    }

    const void *at(size_t idx) const
    {
        return reinterpret_cast<const void *>(data + idx * size);
    }
    void *at(size_t idx)
    {
        return reinterpret_cast<void *>(data + idx * size);
    }
    template <typename T>
    const T *at(size_t idx) const
    {
        return reinterpret_cast<const T *>(data + idx * sizeof(T));
    }
    template <typename T>
    T *at(size_t idx)
    {
        return reinterpret_cast<T *>(data + idx * sizeof(T));
    }

    bool full(size_t size) const
    {
        return (len + 1) * size > DATA_SIZE;
    }
    template <typename T>
    bool full() const
    {
        return (len + 1) * sizeof(T) > DATA_SIZE;
    }

    void dump(void (*dumper)(const TREE *, FILE *), unsigned indent, FILE *stream = stderr, void (*mdumper)(const M *, FILE *) = nullptr) const
    {
        fputc('\n', stream);
        for (unsigned i = 0; i < indent; i++)
            fputc(' ', stream);
        switch (tag)
        {
            case TREE_2: case TREE_3:
                fprintf(stream, "%stree%d%s(", RED, (tag == TREE_2? 2: 3), OFF);
                t[0]->dump(dumper, indent+1, stream, mdumper);
                fputc(',', stream);
                t[1]->dump(dumper, indent+1, stream, mdumper);
                if (tag == TREE_3)
                {
                    fputc(',', stream);
                    t[2]->dump(dumper, indent+1, stream, mdumper);
                }
                fputc(')', stream);
                if (mdumper != nullptr) mdumper(&monoid, stream);
                return;
            case ELEMENT:
                fputc('[', stream);
                dumper(this, stream);
                fputc(']', stream);
                return;
        }
        LIBFPP_UNREACHABLE();
    }
};

/*
 * [INTERNAL] Finger-tree "digits".
 */
template <typename S, typename M = EMPTY>
struct DIG : public OBJECT<S>
{
    using OBJECT<S>::refcount;

    enum TAG : size_t
    {
        DIG_1,
        DIG_2,
        DIG_3,
        DIG_4,
    };

    TAG tag:TAG_BITS;
    size_t size:SIZE_BITS;
    size_t len:LEN_BITS;

    const TREE<S, M> *t[4];
    char pad[2 * sizeof(void *) - sizeof(M)];
    M monoid;

    DIG(const TREE<S, M> *t1, const void *arg = nullptr) : tag(DIG_1), size(0), len(t1->len), t{t1}, monoid(this, arg)
    {
        t1->ref();
    }
    DIG(const TREE<S, M> *t1, const TREE<S, M> *t2, const void *arg = nullptr) : tag(DIG_2), size(0), len(t1->len + t2->len), t{t1, t2}, monoid(this, arg)
    {
        t1->ref(); t2->ref();
    }
    DIG(const TREE<S, M> *t1, const TREE<S, M> *t2, const TREE<S, M> *t3, const void *arg = nullptr) : tag(DIG_3), size(0), len(t1->len + t2->len + t3->len), t{t1, t2, t3}, monoid(this, arg)
    {
        t1->ref(); t2->ref(); t3->ref();
    }
    DIG(const TREE<S, M> *t1, const TREE<S, M> *t2, const TREE<S, M> *t3, const TREE<S, M> *t4, const void *arg = nullptr) : tag(DIG_4), size(0), len(t1->len + t2->len + t3->len + t4->len), t{t1, t2, t3, t4}, monoid(this, arg)
    {
        t1->ref(); t2->ref(); t3->ref(); t4->ref();
    }

    ~DIG()
    {
        for (size_t i = 0; i <= tag; i++)
            t[i]->deref();
    }
    void deref() const
    {
        if (LIBFPP_UNLIKELY(OBJECT<S>::deref()))
        {
            auto *self = const_cast<DIG<S, M> *>(this);
            self->~DIG();
            S::deallocate(self);
        }
    }
    void gc() const
    {
        auto *self = const_cast<DIG<S, M> *>(this);
        if (self->refcount != 0)
            return;
        self->~DIG();
        S::deallocate(self);
    }
    template <typename T>
    void DEREF() const
    {
        if (LIBFPP_LIKELY(!OBJECT<S>::deref()))
            return;
        for (size_t i = 0; i <= tag; i++)
            t[i]->template DEREF<T>();
        auto *self = const_cast<DIG<S, M> *>(this);
        S::deallocate(self);
    }

    void dump(void (*dumper)(const TREE<S, M> *, FILE *), unsigned indent, FILE *stream = stderr, void (*mdumper)(const M *, FILE *) = nullptr) const
    {
        fputc('\n', stream);
        for (unsigned i = 0; i < indent; i++)
            fputc(' ', stream);
        fprintf(stream, "%sdig%zu%s(", YELLOW, tag + 1, OFF);
        for (size_t i = 0; i <= tag; i++)
        {
            if (i > 0) fputc(',', stream);
            t[i]->dump(dumper, indent+1, stream, mdumper);
        }
        fputc(')', stream);
        if (mdumper != nullptr) mdumper(&monoid, stream);
    }
};

/*
 * [INTERNAL] Finger-tree "sequence".
 */
template <typename S, typename M = EMPTY>
struct SEQ : OBJECT<S>
{
    enum TAG : size_t
    {
        SINGLE,
        NIL,
        DEEP,
    };

    TAG tag:TAG_BITS;
    size_t size:SIZE_BITS;
    size_t len:LEN_BITS;

    union
    {
        const TREE<S, M> *t;
        struct
        {
            const SEQ<S, M> *m;
            const DIG<S, M> *l;
            const DIG<S, M> *r; 
        };
    };

    M monoid;

    SEQ(void) : tag(NIL), size(0), len(0), monoid()
    {
        l = reinterpret_cast<DIG<S, M> *>(this);
        m = reinterpret_cast<SEQ<S, M> *>(this);
        r = reinterpret_cast<DIG<S, M> *>(this);
    }
    SEQ(const TREE<S, M> *t, const void *arg = nullptr) : tag(SINGLE), size(0), len(t->len), t(t), monoid(this, arg)
    {
        t->ref();
        l = reinterpret_cast<DIG<S, M> *>(this);
        r = reinterpret_cast<DIG<S, M> *>(this);
    }
    SEQ(const DIG<S, M> *l, const SEQ<S, M> *m, const DIG<S, M> *r, const void *arg = nullptr) : tag(DEEP), size(0), len(l->len + m->len + r->len), m(m), l(l), r(r), monoid(this, arg)
    {
        l->ref(); m->ref(); r->ref();
    }

    ~SEQ()
    {
        switch (tag)
        {
            case SINGLE:
                t->deref();
                break;
            case DEEP:
                l->deref();
                m->deref();
                r->deref();
                break;
            default:
                break;
        }
    }
    void deref() const
    {
        if (LIBFPP_UNLIKELY(OBJECT<S>::deref()))
        {
            auto *self = const_cast<SEQ<S, M> *>(this);
            self->~SEQ();
            S::deallocate(self);
        }
    }
    void gc() const
    {
        auto *self = const_cast<SEQ<S, M> *>(this);
        if (self->refcount != 0)
            return;
        self->~SEQ();
        S::deallocate(self);
    }
    template <typename T>
    void DEREF() const
    {
        if (LIBFPP_LIKELY(!OBJECT<S>::deref()))
            return;
        switch (tag)
        {
            case SINGLE:
                t->template DEREF<T>();
                break;
            case DEEP:
                l->template DEREF<T>();
                m->template DEREF<T>();
                r->template DEREF<T>();
                break;
            default:
                break;
        }
        auto *self = const_cast<SEQ<S, M> *>(this);
        S::deallocate(self);
    }

    void dump(void (*dumper)(const TREE<S, M> *, FILE *), unsigned indent, FILE *stream = stderr, void (*mdumper)(const M *, FILE *) = nullptr) const
    {
        fputc('\n', stream);
        for (unsigned i = 0; i < indent; i++)
            fputc(' ', stream);
        switch (tag)
        {
            case NIL: fprintf(stream, "%snil%s", GREEN, OFF); return;
            case SINGLE:
                fprintf(stream, "%ssingle%s(", GREEN, OFF);
                t->dump(dumper, indent+1, stream, mdumper);
                fputc(')', stream);
                break;
            case DEEP:
                fprintf(stream, "%sdeep%s(", GREEN, OFF);
                l->dump(dumper, indent+1, stream, mdumper);
                fputc(',', stream);
                m->dump(dumper, indent+1, stream, mdumper);
                fputc(',', stream);
                r->dump(dumper, indent+1, stream, mdumper);
                fputc(')', stream);
                break;
        }
        if (mdumper != nullptr) mdumper(&monoid, stream);
    }
};

/*
 * [INTERNAL] Default empty monoid
 */
struct EMPTY
{
    EMPTY()
    {
        ;
    }
    template <typename S>
    EMPTY(const TREE<S, EMPTY> *, const void * = nullptr)
    {
        ;
    }
    template <typename S>
    EMPTY(const DIG<S, EMPTY> *, const void * = nullptr)
    {
        ;
    }
    template <typename S>
    EMPTY(const SEQ<S, EMPTY> *, const void * = nullptr)
    {
        ;
    }
};

/*
 * [INTERNAL] min..max element monoid
 */
struct MIN
{
    const void *min;
    const void *max;

    MIN() : min(nullptr), max(nullptr)
    {
        ;
    }
    template <typename S>
    MIN(const TREE<S, MIN> *t, const void *arg = nullptr) : min(t->tag == TREE<S, MIN>::ELEMENT? reinterpret_cast<const void *>(t->data): get_min(t->t[0])), max(arg == nullptr? nullptr: init_max(t, arg))
    {
        ;
    }
    template <typename S>
    MIN(const DIG<S, MIN> *d, const void *arg = nullptr) : min(get_min(d->t[0])), max(arg == nullptr? nullptr: init_max(d, arg))
    {
        ;
    }
    template <typename S>
    MIN(const SEQ<S, MIN> *s, const void *arg = nullptr) : min(s->tag == SEQ<S, MIN>::NIL? nullptr: s->tag == SEQ<S, MIN>::SINGLE? get_min(s->t): get_min(s->l)), max(arg == nullptr? nullptr: init_max(s, arg))
    {
        ;
    }

    template <typename S>
    static const void *get_min(const TREE<S, MIN> *t)
    {
        switch (t->tag)
        {
            case TREE<S, MIN>::ELEMENT:
                return reinterpret_cast<const void *>(t->data);
            default:
                return t->monoid.min;
        }
    }
    template <typename S>
    static const void *get_min(const DIG<S, MIN> *d)
    {
        return d->monoid.min;
    }
    template <typename S>
    static const void *get_min(const SEQ<S, MIN> *s)
    {
        return s->monoid.min;
    }

    template <typename S>
    static const void *get_max(const TREE<S, MIN> *t, const void *arg = nullptr);
    template <typename S>
    static const void *get_max(const DIG<S, MIN> *d, const void * = nullptr)
    {
        return d->monoid.max;
    }
    template <typename S>
    static const void *get_max(const SEQ<S, MIN> *s, const void * = nullptr)
    {
        return s->monoid.max;
    }

    using max_t = const void *(*)(const void *, const void *);

    template <typename S>
    static const void *init_max(const TREE<S, MIN> *t, const void *arg);
    template <typename S>
    static const void *init_max(const DIG<S, MIN> *d, const void *arg);
    template <typename S>
    static const void *init_max(const SEQ<S, MIN> *s, const void *arg);
};

extern LIBFPP_PURE size_t utf8_len(const char *cstr, size_t max);
extern LIBFPP_PURE char32_t utf8_get(const char *cstr, size_t idx);

/*
 * [INTERNAL] string length monoid
 */
struct STR
{
    size_t len;

    STR() : len(0)
    {
        ;
    }
    template <typename S>
    STR(const TREE<S, STR> *t, const void * = nullptr) : len(t->tag == TREE<S, STR>::ELEMENT? utf8_len(t->str, t->len): get(t->t[0]) + get(t->t[1]) + (t->tag == TREE<S, STR>::TREE_3? get(t->t[2]): 0))
    {
        ;
    }
    template <typename S>
    STR(const DIG<S, STR> *d, const void * = nullptr) : len(get(d->t[0]) + (d->tag >= DIG<S, STR>::DIG_2? get(d->t[1]) + (d->tag >= DIG<S, STR>::DIG_3? get(d->t[2]) + (d->tag == DIG<S, STR>::DIG_4? get(d->t[3]): 0): 0): 0))
    {
        ;
    }
    template <typename S>
    STR(const SEQ<S, STR> *s, const void * = nullptr) : len(s->tag == SEQ<S, STR>::NIL? 0: (s->tag == SEQ<S, STR>::SINGLE? get(s->t): get(s->l) + get(s->m) + get(s->r)))
    {
        ;
    }

    template <typename S>
    static size_t get(const TREE<S, STR> *t)
    {
        return t->monoid.len;
    }
    template <typename S>
    static size_t get(const DIG<S, STR> *d)
    {
        return d->monoid.len;
    }
    template <typename S>
    static size_t get(const SEQ<S, STR> *s)
    {
        return s->monoid.len;
    }
};

/*
 * [INTERNAL] Finger-tree "iterator".
 */
template <typename S, typename M = EMPTY>
struct ITR : OBJECT<S>
{
protected:

    static ssize_t len(const TREE<S> *t)
    {
        return t->len;
    }
    static ssize_t len(const TREE<S, MIN> *t)
    {
        return t->len;
    }
    static ssize_t len(const TREE<S, STR> *t)
    {
        return t->monoid.len;
    }
    static ssize_t len(const DIG<S> *d)
    {
        return d->len;
    }
    static ssize_t len(const DIG<S, MIN> *d)
    {
        return d->len;
    }
    static ssize_t len(const DIG<S, STR> *d)
    {
        return d->monoid.len;
    }
    static ssize_t len(const SEQ<S> *s)
    {
        return s->len;
    }
    static ssize_t len(const SEQ<S, MIN> *s)
    {
        return s->len;
    }
    static ssize_t len(const SEQ<S, STR> *s)
    {
        return s->monoid.len;
    }

public:

    enum TAG : size_t
    {
        ELEMENT,
        TREE,
        DIG,
        SEQ
    };

protected:

    static ssize_t len(TAG tag, const OBJECT<S> *obj)
    {
        switch (tag)
        {
            case ELEMENT: case TREE:
                return len(reinterpret_cast<const F::TREE<S, M> *>(obj));
            case DIG:
                return len(reinterpret_cast<const F::DIG<S, M> *>(obj));
            case SEQ:
                return len(reinterpret_cast<const F::SEQ<S, M> *>(obj));
        }
        LIBFPP_UNREACHABLE();
    }

public:

    TAG tag:TAG_BITS;
    size_t path:8;
    size_t dirty:1;

    size_t lo, hi, pos;

    union
    {
        const F::TREE<S, M> *t;
        const F::DIG<S, M> *d;
        const F::SEQ<S, M> *s;
        const F::OBJECT<S> *obj;
    };

    const ITR *next;

    ITR(const F::TREE<S, M> *t, size_t path, ssize_t lo, ssize_t pos, const ITR *next, bool dirty = false) : tag(t->tag == F::TREE<S, M>::ELEMENT?  ELEMENT: TREE), path(path), dirty(dirty), lo(lo), hi(lo + len(t)), pos(pos), t(t), next(next)
    {
        t->ref();
        if (next != nullptr) next->ref();
    }
    ITR(const F::DIG<S, M> *d, size_t path, ssize_t lo, ssize_t pos, const ITR *next, bool dirty = false) : tag(DIG), path(path), dirty(dirty), lo(lo), hi(lo + len(d)), pos(pos), d(d), next(next)
    {
        d->ref();
        if (next != nullptr) next->ref();
    }
    ITR(const F::SEQ<S, M> *s, size_t path, ssize_t lo, ssize_t pos, const ITR *next, bool dirty = false) : tag(SEQ), path(path), dirty(dirty), lo(lo), hi(lo + len(s)), pos(pos), s(s), next(next)
    {
        s->ref();
        if (next != nullptr) next->ref();
    }
    ITR(const F::OBJECT<S> *obj, TAG tag, size_t path, ssize_t lo, ssize_t pos, const ITR *next, bool dirty = false) : tag(tag), path(path), dirty(dirty), lo(lo), hi(lo + len(tag, obj)), pos(pos), obj(obj), next(next)
    {
        obj->ref();
        if (next != nullptr) next->ref();
    }

    ~ITR()
    {
        switch (tag)
        {
            case ELEMENT:
            case TREE: t->deref(); break;
            case DIG:  d->deref(); break;
            case SEQ:  s->deref(); break;
        }
        if (next != nullptr)
            next->deref();
    }
    void deref() const
    {
        if (LIBFPP_UNLIKELY(OBJECT<S>::deref()))
        {
            auto *self = const_cast<ITR<S, M> *>(this);
            self->~ITR();
            S::deallocate(self);
        }
    }
    void gc() const
    {
        auto *self = const_cast<ITR<S, M> *>(this);
        if (self->refcount != 0)
            return;
        self->~ITR();
        S::deallocate(self);
    }
    template <typename T>
    void DEREF() const
    {
        if (LIBFPP_LIKELY(!OBJECT<S>::deref()))
            return;
        switch (tag)
        {
            case ELEMENT:
            case TREE: t->template DEREF<T>(); break;
            case DIG:  d->template DEREF<T>(); break;
            case SEQ:  s->template DEREF<T>(); break;
        }
        if (next != nullptr)
            next->template DEREF<T>();
        auto *self = const_cast<ITR<S, M> *>(this);
        S::deallocate(self);
    }

    void dump(FILE *stream = stderr, bool toplevel = true) const
    {
        switch (tag)
        {
            case ELEMENT: fprintf(stream, "<%sELEMENT%s", MAGENTA, OFF); break;
            case TREE:    fprintf(stream, "<%sTREE%s", RED, OFF); break;
            case DIG:     fprintf(stream, "<%sDIG%s",YELLOW, OFF); break;
            case SEQ:     fprintf(stream, "<%sSEQ%s", GREEN, OFF); break;
        }
        fprintf(stream, "%s %zd in %zd..%zd>", (dirty? "*": ""), pos, lo, hi);
        if (next != nullptr)
            next->dump(stream, false);
        if (toplevel)
            fputc('\n', stream);
    }
};

template <typename S> using UTF8_TREE = TREE<S, STR>;
template <typename S> using UTF8_DIG  = DIG<S, STR>;
template <typename S> using UTF8_SEQ  = SEQ<S, STR>;
template <typename S> using UTF8_ITR  = ITR<S, STR>;

/**
 * Result
 */
template <typename... T>
struct result { };
template <typename T>
struct result<T>
{
    T a;
};
template <typename T, typename U>
struct result<T, U>
{
    T a;
    U b;
};
template <typename T, typename U, typename V>
struct result<T, U, V>
{
    T a;
    U b;
    V c;
};
template <typename T, typename U, typename V, typename W>
struct result<T, U, V, W>
{
    T a;
    U b;
    V c;
    W d;
};
template <typename T, typename U, typename V, typename W, typename X>
struct result<T, U, V, W, X>
{
    T a;
    U b;
    V c;
    W d;
    X e;
};
template <typename T, typename U, typename V, typename W, typename X, typename Y>
struct result<T, U, V, W, X, Y>
{
    T a;
    U b;
    V c;
    W d;
    X e;
    Y f;
};
template <typename T, typename U, typename V, typename W, typename X, typename Y, typename Z>
struct result<T, U, V, W, X, Y, Z>
{
    T a;
    U b;
    V c;
    W d;
    X e;
    Y f;
    Z g;
};
template <typename T, typename U, typename V, typename W, typename X, typename Y, typename Z, typename A>
struct result<T, U, V, W, X, Y, Z, A>
{
    T a;
    U b;
    V c;
    W d;
    X e;
    Y f;
    Z g;
    A h;
};

/**
 * Index
 */
struct index
{
    static constexpr intptr_t MIN = INTPTR_MIN;
    static constexpr intptr_t MAX = INTPTR_MAX;

private:
    static constexpr ssize_t add(ssize_t a, ssize_t b)
    {
        if (LIBFPP_UNLIKELY((b > 0) && (a > MAX - b))) return MAX;
        if (LIBFPP_UNLIKELY((b < 0) && (a < MIN - b))) return MIN;
        return a + b;
    }
    static constexpr ssize_t sub(ssize_t a, ssize_t b)
    {
        if (LIBFPP_UNLIKELY((b < 0) && (a > MAX + b))) return MAX;
        if (LIBFPP_UNLIKELY((b > 0) && (a < MIN + b))) return MIN;
        return a - b;
    }

    intptr_t pos;

public:

    constexpr index() : pos(MIN)
    {
        ;
    }
    constexpr index(const index &i) = default;
    constexpr index(intptr_t v)  : pos(static_cast<ssize_t>(v)) {}

    constexpr operator ssize_t() const { return pos; }
    constexpr operator size_t() const  { return static_cast<size_t>(pos); }

    constexpr index operator+(const index x) const { return index(add(pos, x.pos)); }
    constexpr index operator-(const index x) const { return index(sub(pos, x.pos)); }
    constexpr index operator-() const { return index(pos == MIN ? MAX : -pos); }

    constexpr index operator+=(const index x)
    {
        pos = add(pos, x.pos);
        return *this;
    }
    constexpr index operator-=(const index x)
    {
        pos = sub(pos, x.pos);
        return *this;
    }

    constexpr index operator++()
    {
        *this += 1;
        return *this;
    }
    constexpr index operator++(int)
    {
        index tmp = *this;
        ++(*this);
        return tmp;
    }
    constexpr index operator--()
    {
        *this -= 1;
        return *this;
    }
    constexpr index operator--(int)
    {
        index tmp = *this;
        --(*this);
        return tmp;
    }

    constexpr bool operator==(const index x) const { return pos == x.pos; }
    constexpr bool operator!=(const index x) const { return pos != x.pos; }
    constexpr bool operator<(const index x) const { return pos < x.pos; }
    constexpr bool operator<=(const index x) const { return pos <= x.pos; }
    constexpr bool operator>(const index x) const { return pos > x.pos; }
    constexpr bool operator>=(const index x) const { return pos >= x.pos; }
    constexpr bool operator==(int x) const { return pos == x; }
    constexpr bool operator!=(int x) const { return pos != x; }
    constexpr bool operator<(int x) const { return pos < x; }
    constexpr bool operator<=(int x) const { return pos <= x; }
    constexpr bool operator>(int x) const { return pos > x; }
    constexpr bool operator>=(int x) const { return pos >= x; }
};
constexpr index operator ""_idx(unsigned long long i)
{
    return index(i);
}

/**
 * Order
 */
struct order
{
    enum value
    {
        LT = -1,
        EQ = 0,
        GT = 1,
        UNORDERED = 2,
    };

    value v;

    order() : v(UNORDERED)
    {
        ;
    }
    order(value v) : v(v)
    {
        ;
    }
    order(int c) : v(c < 0? LT: (c > 0? GT: EQ))
    {
        ;
    }

    bool operator==(std::nullptr_t) const
    {
        return (v == EQ);
    }
    bool operator!=(std::nullptr_t) const
    {
        return (v == LT || v == GT);
    }
    bool operator<(std::nullptr_t) const
    {
        return (v == LT);
    }
    bool operator<=(std::nullptr_t) const
    {
        return (v == LT || v == EQ);
    }
    bool operator>(std::nullptr_t) const
    {
        return (v == GT);
    }
    bool operator>=(std::nullptr_t) const
    {
        return (v == GT || v == EQ);
    }
    bool unordered() const
    {
        return (v >= UNORDERED);
    }
};

/**
 * Comparison.
 */
template <typename T>
struct cmp
{
    /**
     * Compare values.
     */
    LIBFPP_PURE order operator()(const T &x, const T &y) const
    {
        if (x < y)  return order(order::LT);
        if (x > y)  return order(order::GT);
        if (x == y) return order(order::EQ);
        return order(order::UNORDERED);
    }
};

/*
 * [INTERNAL]
 */
template <typename S>
struct INTERNAL
{
    static LIBFPP_PURE const char *seq_verify(const SEQ<S> *s, size_t size);
    static LIBFPP_PURE const char *seq_verify(const SEQ<S, MIN> *s, size_t size);
    static LIBFPP_PURE const char *seq_verify(const UTF8_SEQ<S> *s, size_t size);
    static void seq_write(const SEQ<S> *s, void *buf, void *(*write)(void *, const TREE<S> *));
    static void seq_write(const SEQ<S, MIN> *s, void *buf, void *(*write)(void *, const TREE<S, MIN> *));
    static void str_write(const UTF8_SEQ<S> *s, void *buf, void *(*write)(void *, const UTF8_TREE<S> *));
    static LIBFPP_PURE result<const TREE<S> *, size_t> seq_get(const SEQ<S> *s, size_t idx);
    static LIBFPP_PURE result<const TREE<S, MIN> *, size_t> seq_get(const SEQ<S, MIN> *s, size_t idx);
    static LIBFPP_PURE char32_t str_get(const UTF8_SEQ<S> *s, size_t idx);
    static LIBFPP_PURE const SEQ<S> *seq_assign(const SEQ<S> *s, size_t idx, const TREE<S> *(*set)(const TREE<S> *, size_t, const void *), const void *data);
    static LIBFPP_PURE const void *seq_search(const SEQ<S, MIN> *s, const void *key, order (*compare)(const void *, const void *));
    static LIBFPP_PURE ssize_t seq_find(const SEQ<S, MIN> *s, const void *key, order (*compare)(const void *, const void *));
    static LIBFPP_PURE ssize_t seq_multi_find(const SEQ<S, MIN> *s, const void *key, order (*compare)(const void *, const void *));
    static LIBFPP_PURE const SEQ<S> *seq_push_back(const SEQ<S> *s, const TREE<S> *t);
    static LIBFPP_PURE const SEQ<S> *seq_replace_back(const SEQ<S> *s, const TREE<S> *t, const void *arg = nullptr);
    static LIBFPP_PURE const SEQ<S, MIN> *seq_replace_back(const SEQ<S, MIN> *s, const TREE<S, MIN> *t, const void *arg = nullptr);
    static LIBFPP_PURE const UTF8_SEQ<S> *str_push_back(const UTF8_SEQ<S> *s, const UTF8_TREE<S> *t);
    static LIBFPP_PURE const UTF8_SEQ<S> *str_slow_push_back(const UTF8_SEQ<S> *s, const UTF8_TREE<S> *t, char32_t c);
    static LIBFPP_PURE const UTF8_SEQ<S> *str_push_back(const UTF8_SEQ<S> *s, const char *cstr, const char *cmax);
    static LIBFPP_PURE const UTF8_SEQ<S> *str_replace_back(const UTF8_SEQ<S> *s, const UTF8_TREE<S> *t);
    static LIBFPP_PURE const SEQ<S> *seq_push_front(const SEQ<S> *s, const TREE<S> *t);
    static LIBFPP_PURE const SEQ<S> *seq_replace_front(const SEQ<S> *s, const TREE<S> *t, const void *arg = nullptr);
    static LIBFPP_PURE const SEQ<S, MIN> *seq_replace_front(const SEQ<S, MIN> *s, const TREE<S, MIN> *t, const void *arg = nullptr);
    static LIBFPP_PURE const UTF8_SEQ<S> *str_push_front(const UTF8_SEQ<S> *s, const UTF8_TREE<S> *t);
    static LIBFPP_PURE const UTF8_SEQ<S> *str_slow_push_front(const UTF8_SEQ<S> *s, const UTF8_TREE<S> *t, char32_t c);
    static LIBFPP_PURE const UTF8_SEQ<S> *str_push_front(const UTF8_SEQ<S> *s, const char *cstr, const char *cmax);
    static LIBFPP_PURE const UTF8_SEQ<S> *str_replace_front(const UTF8_SEQ<S> *s, const UTF8_TREE<S> *t);
    static LIBFPP_PURE const SEQ<S> *seq_insert(const SEQ<S> *s, size_t idx, const void *, result<const TREE<S> *, const TREE<S> *, const TREE<S> *> (*insert)(const TREE<S> *, const TREE<S> *, size_t, const void *, size_t));
    static LIBFPP_PURE const UTF8_SEQ<S> *str_insert(const UTF8_SEQ<S> *s, size_t idx, char32_t c);
    static LIBFPP_PURE const UTF8_SEQ<S> *str_insert(const UTF8_SEQ<S> *s, size_t idx, const char *cstr);
    static LIBFPP_PURE const UTF8_SEQ<S> *str_insert(const UTF8_SEQ<S> *s, size_t idx, const UTF8_SEQ<S> *t);
    static LIBFPP_PURE const UTF8_SEQ<S> *str_assign(const UTF8_SEQ<S> *s, size_t idx, char32_t c);
    static LIBFPP_PURE int str_compare(const UTF8_SEQ<S> *s, const char *cstr);
    static LIBFPP_PURE result<const SEQ<S, MIN> *, bool> seq_search_insert(const SEQ<S, MIN> *s, const void *key, order (*compare)(const void *, const void *), result<const TREE<S, MIN> *, const TREE<S, MIN> *, const TREE<S, MIN> *> (*insert)(const TREE<S, MIN> *, const TREE<S, MIN> *, const void *, size_t), const void *arg);
    static LIBFPP_PURE result<size_t, size_t, size_t, size_t> tree_rebalance(size_t t_len, size_t u_len, size_t idx);
    static LIBFPP_PURE const SEQ<S> *seq_erase(const SEQ<S> *s, size_t idx, result<const TREE<S> *, const TREE<S> *> (*erase)(const TREE<S> *, const TREE<S> *, size_t));
    static LIBFPP_PURE const SEQ<S, MIN> *seq_erase(const SEQ<S, MIN> *s, size_t idx, result<const TREE<S, MIN> *, const TREE<S, MIN> *> (*erase)(const TREE<S, MIN> *, const TREE<S, MIN> *, size_t));
    static LIBFPP_PURE const UTF8_SEQ<S> *str_erase(const UTF8_SEQ<S> *s, size_t idx);
    static LIBFPP_PURE const UTF8_SEQ<S> *str_erase(const UTF8_SEQ<S> *s, size_t lo, size_t hi);
    static LIBFPP_PURE result<const SEQ<S, MIN> *, bool> seq_search_erase(const SEQ<S, MIN> *s, const void *key, order (*compare)(const void *, const void *), result<const TREE<S, MIN> *, const TREE<S, MIN> *> (*erase)(const TREE<S, MIN> *, const TREE<S, MIN> *, const void *), const void *arg);
    static LIBFPP_PURE result<const SEQ<S> *, const TREE<S> *> seq_pop_back(const SEQ<S> *s, const void *arg = nullptr);
    static LIBFPP_PURE result<const SEQ<S, MIN> *, const TREE<S, MIN> *> seq_pop_back(const SEQ<S, MIN> *s, const void *arg = nullptr);
    static LIBFPP_PURE result<const SEQ<S> *, const TREE<S> *> seq_pop_front(const SEQ<S> *s, const void *arg = nullptr);
    static LIBFPP_PURE result<const SEQ<S, MIN> *, const TREE<S, MIN> *> seq_pop_front(const SEQ<S, MIN> *s, const void *arg = nullptr);
    static LIBFPP_PURE const UTF8_SEQ<S> *str_pop_back(const UTF8_SEQ<S> *s);
    static LIBFPP_PURE const UTF8_SEQ<S> *str_pop_front(const UTF8_SEQ<S> *s);
    static const SEQ<S> *seq_append(const SEQ<S> *s, const SEQ<S> *t);
    static const UTF8_SEQ<S> *str_append(const UTF8_SEQ<S> *s, const UTF8_SEQ<S> *t);
    static LIBFPP_PURE result<const SEQ<S> *, const SEQ<S> *> seq_split(const SEQ<S> *s, size_t idx, const TREE<S> *(*left)(const TREE<S> *, size_t), const TREE<S> *(*right)(const TREE<S> *, size_t));
    static LIBFPP_PURE result<const SEQ<S, MIN> *, const SEQ<S, MIN> *> seq_split(const SEQ<S, MIN> *s, size_t idx, const TREE<S, MIN> *(*left)(const TREE<S, MIN> *, size_t), const TREE<S, MIN> *(*right)(const TREE<S, MIN> *, size_t));
    static LIBFPP_PURE result<const UTF8_SEQ<S> *, const UTF8_SEQ<S> *> str_split(const UTF8_SEQ<S> *s, size_t idx);
    static LIBFPP_PURE const SEQ<S> *seq_left(const SEQ<S> *s, size_t idx, const TREE<S> *(*left)(const TREE<S> *, size_t));
    static LIBFPP_PURE const SEQ<S, MIN> *seq_left(const SEQ<S, MIN> *s, size_t idx, const TREE<S, MIN> *(*left)(const TREE<S, MIN> *, size_t));
    static LIBFPP_PURE const UTF8_SEQ<S> *str_left(const UTF8_SEQ<S> *s, size_t idx);
    static LIBFPP_PURE const SEQ<S> *seq_right(const SEQ<S> *s, size_t idx, const TREE<S> *(*right)(const TREE<S> *, size_t));
    static LIBFPP_PURE const SEQ<S, MIN> *seq_right(const SEQ<S, MIN> *s, size_t idx, const TREE<S, MIN> *(*right)(const TREE<S, MIN> *, size_t));
    static LIBFPP_PURE const UTF8_SEQ<S> *str_right(const UTF8_SEQ<S> *s, size_t idx);
    static LIBFPP_PURE const ITR<S> *itr_move(const ITR<S> *i, const void *arg = nullptr);
    static LIBFPP_PURE const ITR<S, MIN> *itr_move(const ITR<S, MIN> *i, const void *arg = nullptr);
    static LIBFPP_PURE const ITR<S, STR> *itr_move(const ITR<S, STR> *i, const void *arg = nullptr);
    static LIBFPP_PURE result<const UTF8_ITR<S> *, char32_t> itr_str_slow_get(const UTF8_ITR<S> *i);
    static LIBFPP_PURE const ITR<S> *itr_insert(const ITR<S> *i, const void *data, result<const TREE<S> *, const TREE<S> *, const TREE<S> *> (*insert)(const TREE<S> *, const TREE<S> *, size_t, const void *, size_t));
    static LIBFPP_PURE const ITR<S, MIN> *itr_insert(const ITR<S, MIN> *i, const void *data, result<const TREE<S, MIN> *, const TREE<S, MIN> *, const TREE<S, MIN> *> (*insert)(const TREE<S, MIN> *, const TREE<S, MIN> *, size_t, const void *, size_t));
    static LIBFPP_PURE const UTF8_ITR<S> *itr_str_insert(const UTF8_ITR<S> *i, char32_t c);
    static LIBFPP_PURE const UTF8_ITR<S> *itr_str_insert(const UTF8_ITR<S> *i, const UTF8_SEQ<S> *s);
    static LIBFPP_PURE const UTF8_ITR<S> *itr_str_insert(const UTF8_ITR<S> *i, const char *cstr);
    static LIBFPP_PURE const UTF8_ITR<S> *itr_str_replace(const UTF8_ITR<S> *i, const UTF8_ITR<S> *j, const UTF8_SEQ<S> *s);
    static LIBFPP_PURE const UTF8_ITR<S> *itr_str_replace(const UTF8_ITR<S> *i, const UTF8_ITR<S> *j, const char *cstr);
    static LIBFPP_PURE const UTF8_ITR<S> *itr_str_assign(const UTF8_ITR<S> *i, char32_t c);
    static LIBFPP_PURE const ITR<S> *itr_erase(const ITR<S> *i, result<const TREE<S> *, const TREE<S> *> (*erase)(const TREE<S> *, const TREE<S> *, size_t), const void *arg);
    static LIBFPP_PURE const ITR<S, MIN> *itr_erase(const ITR<S, MIN> *i, result<const TREE<S, MIN> *, const TREE<S, MIN> *> (*erase)(const TREE<S, MIN> *, const TREE<S, MIN> *, size_t), const void *arg);
    static LIBFPP_PURE const UTF8_ITR<S> *itr_str_erase(const UTF8_ITR<S> *i);
    static LIBFPP_PURE const UTF8_ITR<S> *itr_str_erase(const UTF8_ITR<S> *i, ssize_t n);
    static LIBFPP_PURE const UTF8_ITR<S> *itr_str_erase(const UTF8_ITR<S> *i, const UTF8_ITR<S> *j);
    static LIBFPP_PURE const UTF8_ITR<S> *itr_str_slice(const UTF8_ITR<S> *i, ssize_t n);
    static LIBFPP_PURE const UTF8_ITR<S> *itr_str_slice(const UTF8_ITR<S> *i, const UTF8_ITR<S> *j);
    static LIBFPP_PURE const ITR<S> *itr_left(const ITR<S> *i, const TREE<S> *(*left)(const TREE<S> *, size_t));
    static LIBFPP_PURE const ITR<S, MIN> *itr_left(const ITR<S, MIN> *i, const TREE<S, MIN> *(*left)(const TREE<S, MIN> *, size_t));
    static LIBFPP_PURE const UTF8_ITR<S> *itr_str_left(const UTF8_ITR<S> *i);
    static LIBFPP_PURE const ITR<S> *itr_right(const ITR<S> *i, const TREE<S> *(*right)(const TREE<S> *, size_t));
    static LIBFPP_PURE const ITR<S, MIN> *itr_right(const ITR<S, MIN> *i, const TREE<S, MIN> *(*right)(const TREE<S, MIN> *, size_t));
    static LIBFPP_PURE const UTF8_ITR<S> *itr_str_right(const UTF8_ITR<S> *i);
    static LIBFPP_PURE const SEQ<S> *itr_revert(const ITR<S> *i, const void *arg = nullptr);
    static LIBFPP_PURE const SEQ<S, MIN> *itr_revert(const ITR<S, MIN> *i, const void *arg = nullptr);
    static LIBFPP_PURE const UTF8_SEQ<S> *itr_revert(const UTF8_ITR<S> *i, const void *arg = nullptr);

    template <typename T, typename M = EMPTY>
    static void *tree_write(void *buf, const TREE<S, M> *t)
    {
        T *ptr = reinterpret_cast<T *>(buf);
        for (size_t i = 0; i < t->len; i++)
            new (ptr + i) T(*t->template at<T>(i));
        return reinterpret_cast<void *>(ptr + t->len);
    }
    template <typename T, typename M>
    static void tree_dump(const TREE<S, M> *t, FILE *stream = stderr);
    template <typename T, typename M = EMPTY>
    static const TREE<S, M> *tree_assign(const TREE<S, M> *t, size_t idx, const void *data)
    {
        const T &val = *reinterpret_cast<const T *>(data);
        TREE<S, M> *u = S::template allocate<TREE<S, M>>(sizeof(T));
        for (size_t i = 0; i < t->len; i++)
        {
            if (i == idx)
                new (u->template at<T>(i)) T(val);
            else
                new (u->template at<T>(i)) T(*t->template at<T>(i));
        }
        u->len = t->len;
        return u;
    }
    template <typename M = EMPTY>
    static LIBFPP_PURE LIBFPP_INLINE const TREE<S, M> *seq_back(const SEQ<S, M> *s)
    {
        const DIG<S, M> *d = s->r;
        return d->t[d->tag];
    }
    template <typename M = EMPTY>
    static LIBFPP_PURE LIBFPP_INLINE const TREE<S, M> *seq_front(const SEQ<S, M> *s)
    {
        const DIG<S, M> *d = s->l;
        return d->t[0];
    }
    template <typename T>
    static LIBFPP_NOINLINE LIBFPP_PURE const SEQ<S> *seq_slow_push_back(const SEQ<S> *s, const TREE<S> *t, const T &val)
    {
        // SLOW PATH:
        TREE<S> *u = S::template allocate<TREE<S>>(sizeof(T));
        if (LIBFPP_LIKELY(!t->template full<T>()))
        {
            size_t len = t->len;
            for (size_t i = 0; i < len; i++)
                new (u->template at<T>(i)) T(*t->template at<T>(i));
            u->len = len;
        }
        new (u->template at<T>(u->len)) T(val);
        u->len++;
        if (LIBFPP_LIKELY(u->len != 1))
            return INTERNAL<S>::seq_replace_back(s, u);
        else
            return INTERNAL<S>::seq_push_back(s, u);
    }
    template <typename T>
    static LIBFPP_PURE const SEQ<S> *seq_fast_push_back(const SEQ<S> *s, const T &val)
    {
        const DIG<S>  *d = s->r;
        const TREE<S> *t = d->t[d->tag];
        size_t refcount = (s->refcount | d->refcount | t->refcount);
        bool full = t->template full<T>();
        if (LIBFPP_LIKELY(refcount == 1 && !full))
        {   // FAST PATH:
            auto *u = const_cast<TREE<S> *>(t);
            new (u->template at<T>(u->len)) T(val);
            u->len++;
            const_cast<SEQ<S> *>(s)->len++;
            const_cast<DIG<S> *>(d)->len += (reinterpret_cast<const void *>(d) == reinterpret_cast<const void *>(s)? 0: 1);
            return s;
        }
        else
            return INTERNAL<S>::seq_slow_push_back(s, t, val);
    }
    static LIBFPP_PURE const UTF8_SEQ<S> *str_fast_push_back(const UTF8_SEQ<S> *s, char32_t c)
    {
        const UTF8_DIG<S>  *d = s->r;
        const UTF8_TREE<S> *t = d->t[d->tag];
        size_t refcount = (s->refcount | d->refcount | t->refcount);
        if (LIBFPP_LIKELY(refcount == 1 && s->len < UTF8_TREE<S>::STR_SIZE && c <= 0x7F))
        {   // FAST PATH:
            auto *u = const_cast<UTF8_TREE<S> *>(t);
            u->str[u->len++] = static_cast<char>(c);
            u->monoid.len++;
            auto *s1 = const_cast<UTF8_SEQ<S> *>(s);
            s1->len++; s1->monoid.len++;
            if (reinterpret_cast<const void *>(d) != reinterpret_cast<const void *>(s))
            {
                auto *e = const_cast<UTF8_DIG<S> *>(d);
                e->len++; e->monoid.len++;
            }
            return s;
        }
        else
            return INTERNAL<S>::str_slow_push_back(s, t, c);
    }
    template <typename T>
    static LIBFPP_NOINLINE LIBFPP_PURE const SEQ<S> *seq_slow_push_front(const SEQ<S> *s, const TREE<S> *t, const T &val)
    {
        // SLOW PATH:
        TREE<S> *u = S::template allocate<TREE<S>>(sizeof(T));
        if (LIBFPP_LIKELY(!t->template full<T>()))
        {
            size_t len = t->len;
            for (size_t i = 0; i < len; i++)
                new (u->template at<T>(len - i)) T(*t->template at<T>(len - i - 1));
        }
        new (u->template at<T>(0)) T(val);
        u->len++;
        if (LIBFPP_LIKELY(u->len != 1))
            return INTERNAL<S>::seq_replace_front(s, u);
        else
            return INTERNAL<S>::seq_push_front(s, u);
    }
    template <typename T>
    static LIBFPP_PURE const SEQ<S> *seq_fast_push_front(const SEQ<S> *s, const T &val)
    {
        const DIG<S>  *d = s->l;
        const TREE<S> *t = d->t[0];
        size_t refcount = (s->refcount | d->refcount | t->refcount);
        bool full = t->template full<T>();
        if (LIBFPP_LIKELY(refcount == 1 && !full))
        {   // FAST PATH:
            auto *u = const_cast<TREE<S> *>(t);
            size_t len = u->len++;
            for (size_t i = 0; i < len; i++)
                new (u->template at<T>(len - i)) T(*u->template at<T>(len - i - 1));
            new (u->template at<T>(0)) T(val);
            const_cast<SEQ<S> *>(s)->len++;
            const_cast<DIG<S> *>(d)->len += (reinterpret_cast<const void *>(d) == reinterpret_cast<const void *>(s)? 0: 1);
            return s;
        }
        else
            return INTERNAL<S>::seq_slow_push_front(s, t, val);
    }
    static LIBFPP_PURE const UTF8_SEQ<S> *str_fast_push_front(const UTF8_SEQ<S> *s, char32_t c)
    {
        const UTF8_DIG<S>  *d = s->l;
        const UTF8_TREE<S> *t = d->t[0];
        size_t refcount = (s->refcount | d->refcount | t->refcount);
        if (LIBFPP_LIKELY(refcount == 1 && s->len < UTF8_TREE<S>::STR_SIZE && c <= 0x7F))
        {   // FAST PATH:
            auto *u = const_cast<UTF8_TREE<S> *>(t);
            std::memmove(u->str+1, u->str, u->len);
            u->str[0] = static_cast<char>(c);
            u->len++; u->monoid.len++;
            auto *s1 = const_cast<UTF8_SEQ<S> *>(s);
            s1->len++; s1->monoid.len++;
            if (reinterpret_cast<const void *>(d) != reinterpret_cast<const void *>(s))
            {
                auto *e = const_cast<UTF8_DIG<S> *>(d);
                e->len++; e->monoid.len++;
            }
            return s;
        }
        else
            return str_slow_push_front(s, t, c);
    }
    template <typename T, typename M = EMPTY>
    static result<const TREE<S, M> *, const TREE<S, M> *, const TREE<S, M> *> tree_insert(const TREE<S, M> *t, const TREE<S, M> *u, size_t idx, const void *data, size_t refcount)
    {
        const T &val = *reinterpret_cast<const T *>(data);
        if (LIBFPP_UNLIKELY(t == nullptr))
        {
            TREE<S, M> *u = S::template allocate<TREE<S, M>>(sizeof(T));
            new (u->data) T(val);
            u->len = 1;
            return {u, nullptr, nullptr};
        }
        const TREE<S, M> *v = (idx < t->len || u == nullptr? t: u);
        refcount |= v->refcount;
        bool full = v->template full<T>();
        if (LIBFPP_LIKELY(refcount == 1 && !full))
        {   // FAST PATH:
            auto *w = const_cast<TREE<S, M> *>(v);
            idx -= (v == u? t->len: 0);
            for (size_t i = w->len; i > idx; i--)
                new (w->template at<T>(i)) T(*w->template at<T>(i-1));
            new (w->template at<T>(idx)) T(val);
            w->len++;
            return {t, u, nullptr};
        }
        if (LIBFPP_UNLIKELY(full))
        {   // SLOW PATH (REBALANCE):
            auto r = tree_rebalance(t->len, (u == nullptr? 0: u->len), idx);
            size_t pos = r.d;
            TREE<S, M> *a = S::template allocate<TREE<S, M>>(sizeof(T));
            TREE<S, M> *b = S::template allocate<TREE<S, M>>(sizeof(T));
            TREE<S, M> *c = (r.c == 0 && pos != 2? nullptr: S::template allocate<TREE<S, M>>(sizeof(T)));
            size_t i = 0, j, k;
            for (j = 0, k = 0; k < r.a; i++, k++)
            {
                if (pos == 0 && i == idx) new (a->template at<T>(j++)) T(val);
                new (a->template at<T>(j++)) T(i < t->len? *t->template at<T>(i): *u->template at<T>(i - t->len));
            }
            if (pos == 0 && i == idx) new (a->template at<T>(j++)) T(val);
            a->len = j;
            for (j = 0, k = 0; k < r.b; i++, k++)
            {
                if (pos == 1 && i == idx) new (b->template at<T>(j++)) T(val);
                new (b->template at<T>(j++)) T(i < t->len? *t->template at<T>(i): *u->template at<T>(i - t->len));
            }
            if (pos == 1 && i == idx) new (b->template at<T>(j++)) T(val);
            b->len = j;
            if (c != nullptr)
            {
                for (j = 0, k = 0; k < r.c; i++, k++)
                {
                    if (pos == 2 && i == idx) new (c->template at<T>(j++)) T(val);
                    new (c->template at<T>(j++)) T(i < t->len? *t->template at<T>(i): *u->template at<T>(i - t->len));
                }
                if (pos == 2 && i == idx) new (c->template at<T>(j++)) T(val);
                c->len = j;
            }
            return {a, b, c};
        }
        TREE<S, M> *w = S::template allocate<TREE<S, M>>(sizeof(T));
        idx -= (v == u? t->len: 0);
        size_t i;
        for (i = 0; i < idx; i++)
            new (w->template at<T>(i)) T(*v->template at<T>(i));
        new (w->template at<T>(i++)) T(val);
        for (; i <= v->len; i++)
            new (w->template at<T>(i)) T(*v->template at<T>(i-1));
        w->len = v->len + 1;
        if (v == t)
            return {w, u, nullptr};
        else
            return {t, w, nullptr};
    }
    template <typename T, typename Cmp>
    static result<const TREE<S, MIN> *, const TREE<S, MIN> *, const TREE<S, MIN> *> tree_search_insert(const TREE<S, MIN> *t, const TREE<S, MIN> *u, const void *key, size_t refcount)
    {
        const T &val = *reinterpret_cast<const T *>(key);
        if (LIBFPP_UNLIKELY(t == nullptr))
        {
            TREE<S, MIN> *u = S::template allocate<TREE<S, MIN>>(sizeof(T));
            new (u->data) T(val);
            u->len = 1;
            return {u, nullptr, nullptr};
        }
        Cmp compare;
        ssize_t lo = 0, hi = t->len + (u != nullptr? u->len: 0) - 1;
        while (lo <= hi)
        {
            ssize_t mid = lo + (hi - lo) / 2;
            order cmp = compare(val, (mid < (ssize_t)t->len? *t->template at<T>(mid): *u->template at<T>(mid - t->len)));
            if (cmp < 0)
                hi = mid - 1;
            else if (cmp > 0)
                lo = mid + 1;
            else if (cmp == 0)
                return {nullptr, nullptr, nullptr};
            else
                LIBFPP_PANIC("bad ordering");
        }
        size_t idx = lo;
        return tree_insert<T, MIN>(t, u, idx, key, refcount);
    }
    template <typename T, typename Cmp>
    static result<const TREE<S, MIN> *, const TREE<S, MIN> *, const TREE<S, MIN> *> tree_search_multi_insert(const TREE<S, MIN> *t, const TREE<S, MIN> *u, const void *key, size_t refcount)
    {
        const T &val = *reinterpret_cast<const T *>(key);
        if (LIBFPP_UNLIKELY(t == nullptr))
        {
            TREE<S, MIN> *u = S::template allocate<TREE<S, MIN>>(sizeof(T));
            new (u->data) T(val);
            u->len = 1;
            return {u, nullptr, nullptr};
        }
        Cmp compare;
        ssize_t lo = 0, hi = t->len + (u != nullptr? u->len: 0) - 1;
        while (lo <= hi)
        {
            ssize_t mid = lo + (hi - lo) / 2;
            order cmp = compare(val, (mid < (ssize_t)t->len? *t->template at<T>(mid): *u->template at<T>(mid - t->len)));
            if (cmp < 0)
                hi = mid - 1;
            else if (cmp > 0)
                lo = mid + 1;
            else if (cmp == 0)
                return tree_insert<T, MIN>(t, u, mid, key, refcount);
            else
                LIBFPP_PANIC("bad ordering");
        }
        size_t idx = lo;
        return tree_insert<T, MIN>(t, u, idx, key, refcount);
    }
    template <typename T, typename Cmp>
    static result<const TREE<S, MIN> *, const TREE<S, MIN> *, const TREE<S, MIN> *> tree_search_assign(const TREE<S, MIN> *t, const TREE<S, MIN> *u, const void *key, size_t refcount)
    {
        const T &val = *reinterpret_cast<const T *>(key);
        if (LIBFPP_UNLIKELY(t == nullptr))
        {
            TREE<S, MIN> *u = S::template allocate<TREE<S, MIN>>(sizeof(T));
            new (u->data) T(val);
            u->len = 1;
            return {u, nullptr, nullptr};
        }
        Cmp compare;
        ssize_t lo = 0, hi = t->len + (u != nullptr? u->len: 0) - 1;
        while (lo <= hi)
        {
            ssize_t mid = lo + (hi - lo) / 2;
            order cmp = compare(val, (mid < ssize_t(t->len)? *t->template at<T>(mid): *u->template at<T>(mid - t->len)));
            if (cmp < 0)
                hi = mid - 1;
            else if (cmp > 0)
                lo = mid + 1;
            else if (cmp == 0)
            {
                if (mid < ssize_t(t->len))
                {
                    const auto *r = tree_assign<T, MIN>(t, mid, key);
                    return {r, u, nullptr};
                }
                else
                {
                    const auto *r = tree_assign<T, MIN>(u, mid - t->len, key);
                    return {t, r, nullptr};
                }
            }
            else
                LIBFPP_PANIC("bad ordering");
        }
        size_t idx = lo;
        return tree_insert<T, MIN>(t, u, idx, key, refcount);
    }
    template <typename T, typename M = EMPTY>
    static LIBFPP_PURE result<const TREE<S, M> *, const TREE<S, M> *> tree_erase(const TREE<S, M> *t, const TREE<S, M> *u, size_t idx)
    {
        if (LIBFPP_UNLIKELY(u == nullptr && t->len == 1))
            return {nullptr, nullptr};
        size_t i, j;
        TREE<S, M> *v = S::template allocate<TREE<S, M>>(sizeof(T));
        if (LIBFPP_UNLIKELY(u == nullptr || t->len + u->len - 1 <= TREE<S, M>::DATA_SIZE / sizeof(T)))
        {
            for (i = 0, j = 0; i < t->len; i++)
            {
                if (i != idx)
                    new (v->template at<T>(j++)) T(*t->template at<T>(i));
            }
            v->len = j;
            if (u == nullptr)
                return {v, nullptr};
            idx -= t->len;
            for (i = 0; i < u->len; i++)
            {
                if (i != idx)
                    new (v->template at<T>(j++)) T(*u->template at<T>(i));
            }
            v->len = j;
            return {v, nullptr};
        }
        if (LIBFPP_LIKELY(idx < t->len))
        {
            for (i = 0, j = 0; i < t->len; i++)
            {
                if (i != idx)
                    new (v->template at<T>(j++)) T(*t->template at<T>(i));
            }
            v->len = j;
            return {v, u};
        }
        idx -= t->len;
        for (i = 0, j = 0; i < u->len; i++)
        {
            if (i != idx)
                new (v->template at<T>(j++)) T(*u->template at<T>(i));
        }
        v->len = j;
        return {t, v};
    }
    template <typename T, typename Cmp, typename M = EMPTY>
    static LIBFPP_PURE result<const TREE<S, MIN> *, const TREE<S, MIN> *> tree_search_erase(const TREE<S, MIN> *t, const TREE<S, MIN> *u, const void *key)
    {
        const T &val = *reinterpret_cast<const T *>(key);
        ssize_t lo = 0, hi = t->len - 1;
        while (lo <= hi)
        {
            ssize_t mid = lo + (hi - lo) / 2;
            Cmp compare;
            order cmp = compare(val, *t->template at<T>(mid));
            if (cmp < 0)
                hi = mid - 1;
            else if (cmp > 0)
                lo = mid + 1;
            else if (cmp == 0)
                return tree_erase<T, MIN>(t, u, mid);
            else
                LIBFPP_PANIC("bad ordering");
        }
        if (hi < ssize_t(t->len) || u == nullptr) return {t, u};
        lo = 0, hi = u->len - 1;
        while (lo <= hi)
        {
            ssize_t mid = lo + (hi - lo) / 2;
            Cmp compare;
            order cmp = compare(val, *u->template at<T>(mid));
            if (cmp < 0)
                hi = mid - 1;
            else if (cmp > 0)
                lo = mid + 1;
            else if (cmp == 0)
                return tree_erase<T, MIN>(t, u, t->len + mid);
            else
                LIBFPP_PANIC("bad ordering");
        }
        return {t, u};
    }
    template <typename T, typename M = EMPTY>
    static LIBFPP_NOINLINE LIBFPP_PURE const SEQ<S, M> *seq_slow_pop_back(const SEQ<S, M> *s, const TREE<S, M> *t, const void *arg = nullptr)
    {
        // SLOW PATH:
        TREE<S, M> *u = S::template allocate<TREE<S, M>>(sizeof(T));
        if (LIBFPP_LIKELY(t->len > 1))
        {
            size_t len = t->len - 1;
            for (size_t i = 0; i < len; i++)
                new (u->template at<T>(i)) T(*t->template at<T>(i));
            u->len = len;
            return INTERNAL<S>::seq_replace_back(s, u, arg);
        }
        else
        {
            auto r = INTERNAL<S>::seq_pop_back(s, arg);
            return r.a;
        }
    }
    template <typename T, typename M = EMPTY>
    static LIBFPP_PURE const SEQ<S, M> *seq_fast_pop_back(const SEQ<S, M> *s, const void *arg = nullptr)
    {
        const DIG<S, M>  *d = s->r;
        const TREE<S, M> *t = d->t[d->tag];
        return seq_slow_pop_back<T>(s, t, arg);
    }
    template <typename T, typename M = EMPTY>
    static LIBFPP_NOINLINE LIBFPP_PURE const SEQ<S, M> *seq_slow_pop_front(const SEQ<S, M> *s, const TREE<S, M> *t, const void *arg = nullptr)
    {
        // SLOW PATH:
        TREE<S, M> *u = S::template allocate<TREE<S, M>>(sizeof(T));
        if (LIBFPP_LIKELY(t->len > 1))
        {
            size_t len = t->len - 1;
            for (size_t i = 0; i < len; i++)
                new (u->template at<T>(i)) T(*t->template at<T>(i + 1));
            u->len = len;
            return INTERNAL<S>::seq_replace_front(s, u, arg);
        }
        else
        {
            auto r = INTERNAL<S>::seq_pop_front(s, arg);
            return r.a;
        }
    }
    template <typename T, typename M = EMPTY>
    static LIBFPP_PURE const SEQ<S, M> *seq_fast_pop_front(const SEQ<S, M> *s, const void *arg = nullptr)
    {
        const DIG<S, M>  *d = s->l;
        const TREE<S, M> *t = d->t[0];
        return seq_slow_pop_front<T, M>(s, t, arg);
    }
    template <typename T, typename M = EMPTY>
    static LIBFPP_PURE const TREE<S, M> *tree_left(const TREE<S, M> *t, size_t idx)
    {
        if (idx == 0)
            return nullptr;
        if (idx >= t->len)
            return t;
        TREE<S, M> *u = S::template allocate<TREE<S, M>>(sizeof(T));
        for (size_t i = 0; i < idx; i++)
            new (u->template at<T>(i)) T(*t->template at<T>(i));
        u->len = idx;
        return u;
    }
    template <typename T, typename M = EMPTY>
    static LIBFPP_PURE const TREE<S, M> *tree_right(const TREE<S, M> *t, size_t idx)
    {
        if (idx == 0)
            return t;
        if (idx >= t->len)
            return nullptr;
        TREE<S, M> *u = S::template allocate<TREE<S, M>>(sizeof(T));
        for (size_t i = idx, j = 0; i < t->len; i++, j++)
            new (u->template at<T>(j)) T(*t->template at<T>(i));
        u->len = t->len - idx;
        return u;
    }
    template <typename T, typename M = EMPTY>
    static LIBFPP_PURE result<const ITR<S, M> *, const T &> itr_get(const ITR<S, M> *i, const void *arg = nullptr)
    {
        if (LIBFPP_UNLIKELY(i == nullptr)) goto error;
        if (LIBFPP_LIKELY(i->tag == ITR<S, M>::ELEMENT && i->pos >= i->lo && i->pos < i->hi))
        {
            const T &val = *i->t->template at<T>(i->pos - i->lo);
            return {i, val};
        }
        i = INTERNAL<S>::itr_move(i, arg);
        if (LIBFPP_LIKELY(i->pos >= i->lo && i->pos < i->hi))
        {
            const T &val = *i->t->template at<T>(i->pos - i->lo);
            return {i, val};
        }
    error:
        LIBFPP_PANIC("iterator out-of-bounds");
    }
    template <typename T, typename M = EMPTY>
    static LIBFPP_PURE const ITR<S, M> *itr_assign(const ITR<S, M> *i, const T &val, const void *arg = nullptr)
    {
        if (LIBFPP_UNLIKELY(i == nullptr)) goto error;
        if (LIBFPP_LIKELY(i->tag == ITR<S>::ELEMENT && i->pos >= i->lo && i->pos < i->hi))
        {
    redo: {}
            const auto *t = i->t;
            TREE<S, M> *u = S::template allocate<TREE<S, M>>(sizeof(T));
            size_t idx = i->pos - i->lo;
            for (size_t i = 0; i < t->len; i++)
            {
                if (i != idx)
                    new (u->template at<T>(i)) T(*t->template at<T>(i));
                else
                    new (u->template at<T>(i)) T(val);
            }
            u->len = t->len;
            const ITR<S, M> *j = S::template allocate<ITR<S, M>>(u, i->path, i->lo, i->pos, i->next, true);
            return j;
        }
        i = INTERNAL<S>::itr_move(i, arg);
        if (LIBFPP_LIKELY(i->pos >= i->lo && i->pos < i->hi))
            goto redo;
    error:
        LIBFPP_PANIC("iterator out-of-bounds");
    }
    static LIBFPP_PURE result<const UTF8_ITR<S> *, char32_t> itr_str_get(const UTF8_ITR<S> *i)
    {
        if (LIBFPP_LIKELY(i != nullptr && i->tag == UTF8_ITR<S>::ELEMENT && i->pos >= i->lo && i->pos < i->hi && i->t->len == i->t->monoid.len))
        {
            const UTF8_TREE<S> *t = i->t;
            size_t idx = i->pos - i->lo;
            return {i, static_cast<char32_t>(t->str[idx])};
        }
        else
            return INTERNAL<S>::itr_str_slow_get(i);
    }
    static LIBFPP_PURE LIBFPP_INLINE ssize_t itr_safe_add(ssize_t a, ssize_t b)
    {
        if (LIBFPP_UNLIKELY((b > 0) && (a > INTPTR_MAX - b))) return INTPTR_MAX;
        if (LIBFPP_UNLIKELY((b < 0) && (a < INTPTR_MIN - b))) return INTPTR_MIN;
        return a + b;
    }
    static LIBFPP_PURE LIBFPP_INLINE ssize_t itr_safe_sub(ssize_t a, ssize_t b)
    {
        if (LIBFPP_UNLIKELY((b < 0) && (a > INTPTR_MAX + b))) return INTPTR_MAX;
        if (LIBFPP_UNLIKELY((b > 0) && (a < INTPTR_MIN + b))) return INTPTR_MIN;
        return a - b;
    }
    template <typename M = EMPTY>
    static LIBFPP_PURE const ITR<S, M> *itr_add(const ITR<S, M> *i, ssize_t offset)
    {
        if (LIBFPP_UNLIKELY(i == nullptr)) return i;
        ssize_t pos = itr_safe_add(i->pos, offset);
        i = S::template allocate<ITR<S, M>>(i->obj, i->tag, i->path, i->lo, pos, i->next, i->dirty);
        return i;
    }
    template <typename M = EMPTY>
    static LIBFPP_PURE const ITR<S, M> *itr_sub(const ITR<S, M> *i, ssize_t offset)
    {
        if (LIBFPP_UNLIKELY(i == nullptr)) return i;
        ssize_t pos = itr_safe_sub(i->pos, offset);
        i = S::template allocate<ITR<S, M>>(i->obj, i->tag, i->path, i->lo, pos, i->next, i->dirty);
        return i;
    }
    template <typename M = EMPTY>
    static LIBFPP_PURE LIBFPP_INLINE const ITR<S, M> *itr_fast_add(const ITR<S, M> *i, ssize_t offset)
    {
        if (LIBFPP_UNLIKELY(i == nullptr)) return i;
        ssize_t pos = itr_safe_add(i->pos, offset);
        if (LIBFPP_LIKELY(i->refcount <= 1))
        {
            auto *j = const_cast<ITR<S, M> *>(i);
            j->pos = pos;
            return j;
    
        }
        i = S::template allocate<ITR<S, M>>(i->obj, i->tag, i->path, i->lo, pos, i->next, i->dirty);
        return i;
    }
    template <typename M = EMPTY>
    static LIBFPP_PURE LIBFPP_INLINE const ITR<S, M> *itr_fast_sub(const ITR<S, M> *i, ssize_t offset)
    {
        if (LIBFPP_UNLIKELY(i == nullptr)) return i;
        ssize_t pos = itr_safe_sub(i->pos, offset);
        if (LIBFPP_LIKELY(i->refcount <= 1))
        {
            auto *j = const_cast<ITR<S, M> *>(i);
            j->pos = pos;
            return j;
    
        }
        i = S::template allocate<ITR<S, M>>(i->obj, i->tag, i->path, i->lo, pos, i->next, i->dirty);
        return i;
    }
};

/**
 * Iterator base class.
 */
template <typename T, typename S, typename M>
struct iterator
{
protected:

    void SWAP(const ITR<S, M> *j) const
    {
        if (j != nullptr) j->ref();
        if (i != nullptr) i->template DEREF<T>();
        i = j;
    }

public:

    mutable const ITR<S, M> *i;

    iterator() : i(nullptr)
    {
        ;
    }
    iterator(const SEQ<S, M> *s, ssize_t pos) : i(S::template allocate<ITR<S, M>>(s, 0, 0, pos, nullptr))
    {
        i->ref();
    }
    iterator(const ITR<S, M> *i) : i(i)
    {
        if (LIBFPP_LIKELY(i != nullptr)) i->ref();
    }
    iterator(const iterator &j) : i(j.i)
    {
        if (LIBFPP_LIKELY(i != nullptr)) i->ref();
    }
    iterator &operator=(const iterator &j)
    {
        if (LIBFPP_LIKELY(i != nullptr)) i->template DEREF<T>();
        i = j.i;
        if (LIBFPP_LIKELY(i != nullptr)) i->ref();
        return *this;
    }

    ~iterator()
    {
        if (LIBFPP_LIKELY(i != nullptr)) i->template DEREF<T>();
    }

    operator index() const
    {
        if (LIBFPP_UNLIKELY(i == nullptr)) return index(index::MAX);
        return index(i->pos);
    }

    iterator operator+(ssize_t k) const
    {
        const auto *j = INTERNAL<S>::itr_add(i, k);
        return iterator(j);
    }
    iterator operator-(ssize_t k) const
    {
        const auto *j = INTERNAL<S>::itr_sub(i, k);
        return iterator(j);
    }

    iterator operator++(int)
    {
        iterator old(i);
        const auto *j = INTERNAL<S>::itr_add(i, 1);
        SWAP(j);
        return old;
    }
    iterator operator--(int)
    {
        iterator old(i);
        const auto *j = INTERNAL<S>::itr_sub(i, 1);
        SWAP(j);
        return old;
    }
    iterator operator++()
    {
        const auto *j = INTERNAL<S>::itr_fast_add(i, 1);
        SWAP(j);
        return *this;
    }
    iterator operator--()
    {
        const auto *j = INTERNAL<S>::itr_fast_sub(i, 1);
        SWAP(j);
        return *this;
    }

    iterator operator+=(ssize_t k)
    {
        const auto *j = INTERNAL<S>::itr_fast_add(i, k);
        SWAP(j);
        return *this;
    }
    iterator operator-=(ssize_t k)
    {
        const auto *j = INTERNAL<S>::itr_fast_sub(i, k);
        SWAP(j);
        return *this;
    }

    LIBFPP_PURE ptrdiff_t operator-(const iterator &j) const
    {
        if (LIBFPP_UNLIKELY(i == nullptr || j.i == nullptr)) return PTRDIFF_MAX;
        return i->pos - j.i->pos;
    }

    LIBFPP_PURE index pos() const
    {
        if (LIBFPP_UNLIKELY(i == nullptr)) return index();
        return index(i->pos);
    }

    LIBFPP_PURE T operator*() const
    {
        const auto *j = i;
        auto r = INTERNAL<S>::template itr_get<T, M>(j, nullptr);
        SWAP(r.a);
        return r.b;
    }

    LIBFPP_PURE const T *operator->() const
    {
        const auto *j = i;
        auto r = INTERNAL<S>::template itr_get<T, M>(j, nullptr);
        SWAP(r.a);
        return &r.b;
    }

    void assign(const T &val)
    {
        const auto *j = INTERNAL<S>::template itr_assign<T, M>(i, val);
        SWAP(j);
    }

    void insert(const T &val)
    {
        const auto *j = INTERNAL<S>::itr_insert(i, &val, INTERNAL<S>::template tree_insert<M>);
        SWAP(j);
    }

    void erase()
    {
        const auto *j = INTERNAL<S>::itr_erase(i, INTERNAL<S>::template tree_erase<T, M>, nullptr);
        SWAP(j);
    }

    void left()
    {
        const auto *j = INTERNAL<S>::itr_left(i, INTERNAL<S>::template tree_left<T, M>);
        SWAP(j);
    }

    void right()
    {
        const auto *j = INTERNAL<S>::itr_right(i, INTERNAL<S>::template tree_right<T, M>);
        SWAP(j);
    }

    LIBFPP_PURE int compare(const iterator &j, const void *arg = nullptr) const
    {
        if (j.i == nullptr)
        {
            if (LIBFPP_UNLIKELY(i == nullptr)) return 0;
            if (LIBFPP_LIKELY(i->pos < i->hi))
                return -1;
            const auto *k = INTERNAL<S>::itr_move(i, arg);
            SWAP(k);
            return (i->pos < i->hi? -1: 0);
        }
        if (i == nullptr)
            return -j.compare(*this);
        if (i->pos < j.i->pos) return -1;
        if (i->pos > j.i->pos) return 1;
        return 0;
    }

    LIBFPP_PURE bool operator<(const iterator &j) const
    {
        return (compare(j) < 0);
    }
    LIBFPP_PURE bool operator<=(const iterator &j) const
    {
        return (compare(j) <= 0);
    }
    LIBFPP_PURE bool operator>(const iterator &j) const
    {
        return (compare(j) > 0);
    }
    LIBFPP_PURE bool operator>=(const iterator &j) const
    {
        return (compare(j) >= 0);
    }
    LIBFPP_PURE bool operator==(const iterator &j) const
    {
        return (compare(j) == 0);
    }
    LIBFPP_PURE bool operator!=(const iterator &j) const
    {
        return (compare(j) != 0);
    }

    void dump(FILE *stream = stderr)
    {
        if (i == nullptr)
            fputs("END\n", stream);
        else
            i->dump(stream);
    }
};

/**
 * Pair.
 */
template <typename K, typename V>
struct pair
{
    K first;
    V second;
};
template <typename K, typename V, typename Cmp>
struct pair_cmp
{
    LIBFPP_PURE order operator()(const pair<K, V> &x, const pair<K, V> &y) const
    {
        Cmp compare;
        return compare(x.first, y.first);
    }
};

/*
 * [INTERNAL]
 */
extern thread_local SEQ<LOCAL> *LOCAL_NIL;
extern SEQ<SHARE>               SHARE_NIL;
template <typename S, typename M = EMPTY> SEQ<S, M> *nil();
template <> inline SEQ<LOCAL> *nil<LOCAL>()
{
    LOCAL_NIL->ref();
    return LOCAL_NIL;
}
template <> inline SEQ<LOCAL, MIN> *nil<LOCAL, MIN>()
{
    LOCAL_NIL->ref();
    return reinterpret_cast<SEQ<LOCAL, MIN> *>(LOCAL_NIL);
}
template <> inline SEQ<LOCAL, STR> *nil<LOCAL, STR>()
{
    LOCAL_NIL->ref();
    return reinterpret_cast<SEQ<LOCAL, STR> *>(LOCAL_NIL);
}
template <> inline SEQ<SHARE> *nil<SHARE>()
{
    SHARE_NIL.ref();
    return &SHARE_NIL;
}
template <> inline SEQ<SHARE, MIN> *nil<SHARE, MIN>()
{
    SHARE_NIL.ref();
    return reinterpret_cast<SEQ<SHARE, MIN> *>(&SHARE_NIL);
}
template <> inline SEQ<SHARE, STR> *nil<SHARE, STR>()
{
    SHARE_NIL.ref();
    return reinterpret_cast<SEQ<SHARE, STR> *>(&SHARE_NIL);
}

/*
 * [INTERNAL]
 */
template <typename T, typename S = LOCAL>
struct RC : OBJECT<S>
{
private:
    static constexpr size_t DATA_SIZE = ALLOC_SIZE - sizeof(OBJECT<S>);
    const T obj;

public:

    template <typename... Args>
    RC(Args... args) : obj(args...)
    {
        ;
    }

    void deref() const
    {
        if (LIBFPP_UNLIKELY(OBJECT<S>::deref()))
        {
            auto *self = const_cast<RC<T, S> *>(this);
            self->~RC();
            deallocate(self);
        }
    }

    const T *ptr() const
    {
        return &obj;
    }

    template <typename... Args>
    static LIBFPP_INLINE RC *allocate(Args... args)
    {
        if (sizeof(T) <= DATA_SIZE)
            return S::template allocate<RC>(args...);
        else
            return new (S::malloc(sizeof(RC))) RC(args...);
    }

    static LIBFPP_INLINE void deallocate(RC *self)
    {
        if (sizeof(T) <= DATA_SIZE)
            S::deallocate(self);
        else
            S::free(reinterpret_cast<void *>(self));
    }
};

/*
 * [INTERNAL]
 */
template <typename T, typename U = int, typename S = LOCAL>
struct ARRAY : OBJECT<S>
{
    const size_t size;
    const T * const data;
    void (*free)(void *, size_t);
    const U user;

    ARRAY(const T *data, size_t size, void (*free)(void *, size_t) = reinterpret_cast<void (*)(void *, size_t)>(S::free), U user = U()) : size(size), data(data), free(free), user(user)
    {
        ;
    }

    ~ARRAY()
    {
        for (size_t i = 0; i < size; i++)
            data[i].~T();
        user.~U();
    }

    void deref() const
    {
        if (LIBFPP_UNLIKELY(OBJECT<S>::deref()))
        {
            auto *self = const_cast<ARRAY<T, U, S> *>(this);
            self->~ARRAY();
            if (self->free != nullptr)
                self->free(const_cast<void *>(reinterpret_cast<const void *>(data)), size * sizeof(T));
            S::deallocate(self);
        }
    }
};

/**
 * Smart pointer
 */
template <typename T, typename Cmp = cmp<T>, typename S = LOCAL>
struct rc
{
    const struct RC<T, S> *s;

protected:
    rc(const struct RC<T, S> *s) : s(s)
    {
        if (s != nullptr) s->ref();
    }
    rc(const void *ptr) : s(reinterpret_cast<const struct RC<T, S> *>(ptr))
    {
        if (s != nullptr) s->ref();
    }
    static rc make(const void *ptr)
    {
        return rc{ptr};
    }

    LIBFPP_PURE const T *get() const
    {
        if (s == nullptr)
            LIBFPP_PANIC("nullptr deref");
        return s->ptr();
    }

    LIBFPP_PURE int compare(const rc &x) const
    {
        if (s == x.s) return 0;
        if (s == nullptr) return -1;
        if (x.s == nullptr) return 1;
        Cmp compare;
        const T &a = *s->ptr(), &b = *x.s->ptr();
        auto r = compare(a, b);
        if (r < 0) return -1;
        if (r > 0) return 1;
        if (r == 0) return 0;
        panic("bad ordering");
    }

public:

    rc() : s(nullptr)
    {
        ;
    }
    rc(std::nullptr_t) : s(nullptr)
    {
        ;
    }
    rc(const rc &x) : s(x.s)
    {
        if (s != nullptr) s->ref();
    }
    rc(rc &&x) : s(x.s)
    {
        x.s = nullptr;
    }
    template <typename... Args>
    rc(Args... args) : s(RC<T, S>::template allocate(args...))
    {
        s->ref();
    }
    rc &operator=(const rc &x)
    {
        if (x.s != nullptr) x.s->ref();
        if (s != nullptr) s->deref();
        s = x.s;
        return *this;
    }
    rc &operator=(std::nullptr_t)
    {
        if (s != nullptr) s->deref();
        s = nullptr;
        return *this;
    }
    ~rc()
    {
        if (s != nullptr) s->deref();
    }

    LIBFPP_PURE const T operator*() const
    {
        return *get();
    }
    LIBFPP_PURE const T *operator->() const
    {
        return get();
    }

    LIBFPP_PURE size_t refcount() const
    {
        return (s == nullptr? 0: s->refcount);
    }

    LIBFPP_PURE bool operator==(std::nullptr_t) const
    {
        return (s == nullptr);
    }
    LIBFPP_PURE bool operator!=(std::nullptr_t) const
    {
        return (s != nullptr);
    }
    LIBFPP_PURE operator bool() const
    {
        return (s != nullptr);
    }
    LIBFPP_PURE bool operator==(const rc &x) const
    {
        return (compare(x) == 0);
    }
    LIBFPP_PURE bool operator!=(const rc &x) const
    {
        return (compare(x) != 0);
    }
    LIBFPP_PURE bool operator<(const rc &x) const
    {
        return (compare(x) < 0);
    }
    LIBFPP_PURE bool operator<=(const rc &x) const
    {
        return (compare(x) <= 0);
    }
    LIBFPP_PURE bool operator>(const rc &x) const
    {
        return (compare(x) > 0);
    }
    LIBFPP_PURE bool operator>=(const rc &x) const
    {
        return (compare(x) >= 0);
    }

    LIBFPP_PURE bool shared(const rc &x) const
    {
        return (s == x.s);
    }
};

/**
 * Optional
 */
template <typename T, typename Cmp = cmp<T>, typename S = LOCAL>
using optional = rc<T, Cmp, S>;

/**
 * Array
 */
template <typename T, typename U = int, typename S = LOCAL>
struct array
{
private:
    struct ARRAY<T, U, S> *s;

    static void default_free(void *ptr, size_t)
    {
        S::free(ptr);
    }

public:

    array() : s(nullptr)
    {
        ;
    }
    array(const T *data, size_t size, void (*free)(void *, size_t) = default_free, U user = U()) : s(S::template allocate<ARRAY<T, U, S>>(data, size, free, user))
    {
        s->ref();
    }
    array(const array &x) : s(x.s)
    {
        if (s != nullptr) s->ref();
    }
    array(array &&x) : s(x.s)
    {
        x.s = nullptr;
    }
    array &operator=(const array &x)
    {
        if (x.s != nullptr) x.s->ref();
        if (s != nullptr) s->deref();
        s = x.s;
        return *this;
    }
    ~array()
    {
        if (s != nullptr) s->deref();
    }

    LIBFPP_PURE T operator[](const index idx) const
    {
        if (s == nullptr || size_t(idx) >= s->size) LIBFPP_PANIC("bounds error");
        return s->data[idx];
    }

    LIBFPP_PURE size_t size() const
    {
        return (s == nullptr? 0: s->size);
    }
    LIBFPP_PURE const T *data() const
    {
        return (s == nullptr? nullptr: s->data);
    }
    template <typename X = T, typename = std::enable_if_t<std::is_same_v<X, char>>>
    LIBFPP_PURE const char *c_str() const
    {
        return (s == nullptr? nullptr: s->data);
    }
    LIBFPP_PURE U user() const
    {
        return (s == nullptr? U(): s->user);
    }
};

/**
 * Vector
 */
template <typename T, typename S = LOCAL, typename M = EMPTY>
struct vector
{
protected:
    const SEQ<S, M> *s;
    
    void SWAP(const SEQ<S, M> *t)
    {
        t->ref();
        s->template DEREF<T>();
        s = t;
    }

public:

    vector() : s(nil<S, M>())
    {
        s->ref();
    }
    vector(const vector &v) : s(v.s)
    {
        s->ref();
    }
    vector(vector &&v) : s(v.s)
    {
        v.s = nil<S, M>();
        v.s->ref();
    }
    vector(const SEQ<S, M> *s) : s(s)
    {
        s->ref();
    }
    vector &operator=(const vector &v)
    {
        v.s->ref();
        s->template DEREF<T>();
        s = v.s;
        return *this;
    }
    ~vector()
    {
        s->template DEREF<T>();
    }

    /**
     * Return the number of elements in the vector.
     * O(1)
     */
    LIBFPP_PURE size_t size() const
    {
        return s->len;
    }

    /**
     * Returns `true' if the vector is empty, or `false' otherwise: xs == []
     * O(1)
     */
    LIBFPP_PURE bool empty() const
    {
        return (s->len == 0);
    }

    /**
     * Return `true' if the underlying memory is shared, or `false' otherwise.
     */
    LIBFPP_PURE bool shared(const vector &v) const
    {
        return (s == v.s);
    }

    /**
     * Clears the contents: xs := []
     * O(1)
     */
    void clear()
    {
        SWAP(nil<S, M>());
    }

    /**
     * Return the vector contents as an array.
     * O(N)
     */
    LIBFPP_PURE array<T, int, S> data() const
    {
        void *ptr = S::malloc(sizeof(T) * size());
        INTERNAL<S>::seq_write(s, ptr, INTERNAL<S>::template tree_write<T>);
        return array<T, int, S>(reinterpret_cast<T *>(ptr), size());
    }

    /**
     * Return the nth element of the vector: xs[idx]
     * O(log N)
     */
    LIBFPP_PURE T at(const index idx) const
    {
        auto r = INTERNAL<S>::seq_get(s, idx);
        return *r.a->template at<T>(r.b);
    }

    /**
     * Return the nth element of the vector: xs[idx]
     * O(log N)
     */
    LIBFPP_PURE T operator[](const index idx) const
    {
        auto r = INTERNAL<S>::seq_get(s, idx);
        return *r.a->template at<T>(r.b);
    }

    /**
     * Set the nth element of the vector: ys = xs[0:idx-1] ++ {val} ++ xs[idx+1:xs.size()-1]
     * O(log N)
     */
    void assign(const index idx, const T &val)
    {
        const auto *r = INTERNAL<S>::seq_assign(s, idx, INTERNAL<S>::template tree_assign<T, M>, reinterpret_cast<const void *>(&val));
        SWAP(r);
    }

    /**
     * Return the back of the vector: xs[xs.size()-1]
     * O(1)
     */
    LIBFPP_PURE T back() const
    {
        if (LIBFPP_UNLIKELY(s->len == 0)) LIBFPP_PANIC("index out-of-bounds");
        const auto *t = INTERNAL<S>::seq_back(s);
        return *t->template at<T>(t->len-1);
    }

    /**
     * Return the front of the vector: xs[0]
     * O(1)
     */
    LIBFPP_PURE T front() const
    {
        if (LIBFPP_UNLIKELY(s->len == 0)) LIBFPP_PANIC("index out-of-bounds");
        const auto *t = INTERNAL<S>::seq_front(s);
        return *t->template at<T>(0);
    }

    /**
     * Push an element to the back of a vector: xs := xs + {val}
     * O(1)
     */
    void push_back(const T &val)
    {
        if (LIBFPP_UNLIKELY(s->len >= LEN_MAX)) LIBFPP_PANIC("vector is full");
        const auto *r = INTERNAL<S>::seq_fast_push_back(s, val);
        SWAP(r);
    }

    /**
     * Push an element to the back of a vector: xs := xs + {val}
     * O(1)
     */
    vector operator+=(const T &val)
    {
        push_back(val);
        return *this;
    }

    /**
     * Push an element to the front of a vector: xs := {val} + xs
     * O(1)
     */
    void push_front(const T &val)
    {
        if (LIBFPP_UNLIKELY(s->len >= LEN_MAX)) LIBFPP_PANIC("vector is full");
        const auto *r = INTERNAL<S>::seq_fast_push_front(s, val);
        SWAP(r);
    }

    /**
     * Insert an element at the given position: xs := xs[0:idx-1] ++ {val} ++ xs[idx:xs.size()-1]
     * O(log(N))
     */
    void insert(const index idx, const T &val)
    {
        if (LIBFPP_UNLIKELY(s->len >= LEN_MAX)) LIBFPP_PANIC("vector is full");
        const auto *r = INTERNAL<S>::seq_insert(s, idx, reinterpret_cast<const void *>(&val), INTERNAL<S>::template tree_insert<T>);
        SWAP(r);
    }

    /**
     * Erase an element from the given position: xs := xs[0:idx-1] ++ xs[idx+1:zs.size()-1]
     * O(log(N))
     */
    bool erase(const index idx)
    {
        const auto *r = INTERNAL<S>::seq_erase(s, idx, INTERNAL<S>::template tree_erase<T>);
        if (r) SWAP(r);
        return (r != nullptr);
    }

    /**
     * Pop an element from the back of a vector: xs := xs[0:xs.size()-2]
     * O(1)
     */
    void pop_back()
    {
        if (LIBFPP_UNLIKELY(s->len == 0)) LIBFPP_PANIC("vector is empty");
        const auto *r = INTERNAL<S>::template seq_fast_pop_back<T>(s);
        SWAP(r);
    }

    /**
     * Pop an element from the front of a vector: xs := xs[1:xs.size()-1]
     * O(1)
     */
    void pop_front()
    {
        if (LIBFPP_UNLIKELY(s->len == 0)) LIBFPP_PANIC("vector is empty");
        const auto *r = INTERNAL<S>::template seq_fast_pop_front<T>(s);
        SWAP(r);
    }

    /**
     * Append (concat) two vectors: xs := xs + ys
     * O(min(log(N), log(M)))
     */
    void append(const vector &ys)
    {
        const auto *r = INTERNAL<S>::seq_append(s, ys.s);
        SWAP(r);
    }

    /**
     * Append (concat) two vectors: zs := xs + ys
     * O(min(log(N), log(M)))
     */
    vector operator+(const vector &ys) const
    {
        const auto *r = INTERNAL<S>::seq_append(s, ys.s);
        return vector(r);
    }

    /**
     * Append (concat) two vectors: xs := xs + ys
     * O(min(log(N), log(M)))
     */
    vector operator+=(const vector &ys)
    {
        append(ys);
        return *this;
    }

    /**
     * Split a vector: (xs, ys) := (xs[0:idx-1],xs[idx:xs.size()-1])
     * O(log(n))
     */
    vector split(const index idx)
    {
        auto r = INTERNAL<S>::seq_split(s, idx, INTERNAL<S>::template tree_left<T>, INTERNAL<S>::template tree_right<T>);
        SWAP(r.b);
        return vector(r.a);
    }

    /**
     * Truncate a vector to the left: xs := xs[0:idx-1]
     * O(log(n))
     */
    void left(const index idx)
    {
        const auto *r = INTERNAL<S>::seq_left(s, idx, INTERNAL<S>::template tree_left<T>);
        SWAP(r);
    }

    /**
     * Truncate a vector to the right: xs := xs[idx:xs.size()-1]
     * O(log(n))
     */
    void right(const index idx)
    {
        const auto *r = INTERNAL<S>::seq_right(s, idx, INTERNAL<S>::template tree_right<T>);
        SWAP(r);
    }

    struct iterator : F::iterator<T, S, M>
    {
        using base = F::iterator<T, S, M>;
        using base::operator-;
        using base::iterator;
        using base::i;
        using base::SWAP;

        LIBFPP_PURE iterator operator+(ssize_t k) const
        {
            const auto *j = INTERNAL<S>::itr_add(i, k);
            return iterator(j);
        }
        LIBFPP_PURE iterator operator-(ssize_t k) const
        {
            const auto *j = INTERNAL<S>::itr_sub(i, k);
            return iterator(j);
        }
        iterator operator++(int)
        {
            iterator old(i);
            const auto *j = INTERNAL<S>::itr_add(i, 1);
            SWAP(j);
            return old;
        }
        iterator operator--(int)
        {
            iterator old(i);
            const auto *j = INTERNAL<S>::itr_sub(i, 1);
            SWAP(j);
            return old;
        }
        iterator operator++()
        {
            const auto *j = INTERNAL<S>::itr_fast_add(i, 1);
            SWAP(j);
            return *this;
        }
        iterator operator--()
        {
            const auto *j = INTERNAL<S>::itr_fast_sub(i, 1);
            SWAP(j);
            return *this;
        }
        iterator operator+=(ssize_t k)
        {
            const auto *j = INTERNAL<S>::itr_fast_add(i, k);
            SWAP(j);
            return *this;
        }
        iterator operator-=(ssize_t k)
        {
            const auto *j = INTERNAL<S>::itr_fast_sub(i, k);
            SWAP(j);
            return *this;
        }

        /**
         * Extract the vector used by the iterator: i.xs;
         * O(log N)
         */
        LIBFPP_PURE vector value() const
        {
            return vector(INTERNAL<S>::itr_revert(i));
        }
    };

    /**
     * Create an iterator to the start of a vector: i := &xs[0];
     * O(1)
     */
    iterator begin() const
    {
        return iterator(s, 0);
    }

    /**
     * Create an iterator to the last element of a vector: i := &xs[xs.size()-1];
     * O(1)
     */
    iterator rbegin() const
    {
        return iterator(s, s->len-1);
    }

    /**
     * Create an iterator to the end of a vector: i := &xs[xs.size()];
     * O(1)
     */
    static iterator end()
    {
        return iterator();
    }

    /**
     * Create an iterator to the nth element: i := &xs[i];
     * O(1)
     */
    iterator find(index n) const
    {
        return iterator(s, n);
    }

    /**
     * Comparison
     * O(N)
     */
    LIBFPP_PURE int compare(const vector &xs) const
    {
        iterator i = begin(), iend = end();
        iterator j = xs.begin(), jend = xs.end();
        for (; i != iend && j != jend; ++i, ++j)
        {
            if (*i < *j) return -1;
            if (*i > *j) return 1;
        }
        if (j != jend) return 1;
        if (i != iend) return -1;
        return 0;
    }

    LIBFPP_PURE bool operator<(const vector &xs) const
    {
        return compare(xs) < 0;
    }
    LIBFPP_PURE bool operator<=(const vector &xs) const
    {
        return compare(xs) <= 0;
    }
    LIBFPP_PURE bool operator>(const vector &xs) const
    {
        return compare(xs) > 0;
    }
    LIBFPP_PURE bool operator>=(const vector &xs) const
    {
        return compare(xs) >= 0;
    }
    LIBFPP_PURE bool operator==(const vector &xs) const
    {
        return compare(xs) == 0;
    }
    LIBFPP_PURE bool operator!=(const vector &xs) const
    {
        return compare(xs) != 0;
    }

    /**
     * Foldl
     * O(N)
     */
    template <typename A, typename F>
    LIBFPP_PURE A foldl(const A &a, F f)
    {
        A b = a;
        for (auto i = begin(), iend = end(); i != iend; ++i)
            b = f(b, *i);
        return b;
    }

    /**
     * Foldr
     * O(N)
     */
    template <typename A, typename F>
    LIBFPP_PURE A foldr(F f, const A &a)
    {
        A b = a;
        for (auto i = rbegin(), iend = end(); i != iend; ++i)
            b = f(*i, b);
        return b;
    }

    /*
     * Logical NOP, but shrink memory usage.
     * O(N)
     */
    void shrink_to_fit()
    {
        vector xs;
        for (auto i = begin(), iend = end(); i != iend; ++i)
            xs.push_back(*i);
        *this = xs;
    }

    void dump(FILE *stream = stderr) const
    {
        s->dump(INTERNAL<S>::template tree_dump<T, M>, 0, stream);
        fputc('\n', stream);
    }

    const char *verify() const
    {
        return INTERNAL<S>::seq_verify(s, sizeof(T));
    }
};

/**
 * Multi-set
 */
template <typename T, typename Cmp = cmp<T>, typename S = LOCAL>
struct multiset : public vector<T, S, MIN>
{
    using base = vector<T, S, MIN>;

protected:

    using base::s;
    using base::SWAP;

    static order COMPARE(const void *a, const void *b)
    {
        const T &x = *reinterpret_cast<const T *>(a);
        const T &y = *reinterpret_cast<const T *>(b);
        Cmp compare;
        order r = compare(x, y);
        if (r.unordered()) LIBFPP_PANIC("bad ordering");
        return r;
    }

public:

    using base::vector;
    using base::size;
    using base::empty;
    using base::clear;
    using base::at;
    void assign(const index, const T &) = delete;
    using base::operator[];
    using base::back;
    using base::front;
    void push_back(const T &) = delete;
    void push_front(const T &) = delete;
    void insert(index, const T &) = delete;
    using base::pop_back;
    using base::pop_front;
    void append(const base &) = delete;
    using base::erase;
    using base::left;
    using base::right;
    using base::compare;
    using base::dump;
    using base::verify;

    /**
     * Return (the set's copy of) the element: xs[val]
     * O(log N)
     */
    LIBFPP_PURE optional<T, Cmp, S> search(const T &val) const
    {
        const auto *r = INTERNAL<S>::seq_search(s, reinterpret_cast<const void *>(&val), COMPARE);
        if (r == nullptr) return optional<T, Cmp, S>();
        return optional<T, Cmp, S>(*reinterpret_cast<const T *>(r));
    }

    /**
     * Checks if the set contains the element: val in xs
     * O(log N)
     */
    LIBFPP_PURE bool contains(const T &val) const
    {
        return (INTERNAL<S>::seq_search(s, reinterpret_cast<const void *>(&val), COMPARE) != nullptr);
    }

    /**
     * Return (the set's copy of) the element: xs[val]
     * O(log N)
     */
    LIBFPP_PURE T at(const T &val) const
    {
        const auto *r = INTERNAL<S>::seq_search(s, reinterpret_cast<const void *>(&val), COMPARE);
        if (r == nullptr) LIBFPP_PANIC("set element not found");
        return *reinterpret_cast<const T *>(r);
    }

    /**
     * Return the index of the element.
     * O(log N)
     */
    LIBFPP_PURE index pos(const T &val) const
    {
        return index{INTERNAL<S>::seq_multi_find(s, reinterpret_cast<const void *>(&val), COMPARE)};
    }

    /**
     * Return (the set's copy of) the element: xs[val]
     * O(log N)
     */
    LIBFPP_PURE T operator[](const T &val) const
    {
        return at(val);
    }

    /**
     * Pop an element from the back: xs := xs[0:xs.size()-2]
     * O(1)
     */
    void pop_back()
    {
        if (LIBFPP_UNLIKELY(s->len == 0)) LIBFPP_PANIC("vector is empty");
        const auto *r = INTERNAL<S>::template seq_fast_pop_back<T, MIN>(s);
        SWAP(r);
    }

    /**
     * Pop an element from the front: xs := xs[1:xs.size()-1]
     * O(1)
     */
    void pop_front()
    {
        if (LIBFPP_UNLIKELY(s->len == 0)) LIBFPP_PANIC("vector is empty");
        const auto *r = INTERNAL<S>::template seq_fast_pop_front<T, MIN>(s);
        SWAP(r);
    }

    /**
     * Insert an element: xs := xs union {val}
     * O(log(N))
     */
    bool insert(const T &val)
    {
        if (LIBFPP_UNLIKELY(s->len >= LEN_MAX)) LIBFPP_PANIC("set is full");
        auto r = INTERNAL<S>::seq_search_insert(s, reinterpret_cast<const void *>(&val), COMPARE, INTERNAL<S>::template tree_search_multi_insert<T, Cmp>, nullptr);
        SWAP(r.a);
        return r.b;
    }

    /**
     * Set the value the set element: xs := (xs - {val}) union {val}
     * O(log N)
     */
    void assign(const T &val)
    {
        auto r = INTERNAL<S>::seq_search_insert(s, reinterpret_cast<const void *>(&val), COMPARE, INTERNAL<S>::template tree_search_assign<T, Cmp>, nullptr);
        SWAP(r.a);
    }

    /**
     * Erase an element: xs := xs - {val}
     * O(log(N))
     */
    bool erase(const T &val)
    {
        auto r = INTERNAL<S>::seq_search_erase(s, reinterpret_cast<const void *>(&val), COMPARE, INTERNAL<S>::template tree_search_erase<T, Cmp>, nullptr);
        SWAP(r.a);
        return r.b;
    }

    /**
     * Split a set: (xs, ys) := (xs[0:idx-1],xs[idx:xs.size()-1])
     * O(log(n))
     */
    const multiset split(const index idx)
    {
        auto r = INTERNAL<S>::seq_split(s, idx, INTERNAL<S>::template tree_left<T, MIN>, INTERNAL<S>::template tree_right<T, MIN>);
        SWAP(r.b);
        return multiset(r.a);
    }

    /*
     * Convert a set into a vector.
     * O(1)
     */
    operator vector<T>() const
    {
        vector<T> xs(reinterpret_cast<const SEQ<S> *>(s));
        return xs;
    }

    struct iterator : F::iterator<T, S, MIN>
    {
        using base = F::iterator<T, S, MIN>;
        using base::operator-;
        using base::iterator;
        using base::i;
        using base::SWAP;

        LIBFPP_PURE iterator operator+(ssize_t k) const
        {
            const auto *j = INTERNAL<S>::itr_add(i, k);
            return iterator(j);
        }
        LIBFPP_PURE iterator operator-(ssize_t k) const
        {
            const auto *j = INTERNAL<S>::itr_sub(i, k);
            return iterator(j);
        }
        iterator operator++(int)
        {
            iterator old(i);
            const auto *j = INTERNAL<S>::itr_add(i, 1);
            SWAP(j);
            return old;
        }
        iterator operator--(int)
        {
            iterator old(i);
            const auto *j = INTERNAL<S>::itr_sub(i, 1);
            SWAP(j);
            return old;
        }
        iterator operator++()
        {
            const auto *j = INTERNAL<S>::itr_fast_add(i, 1);
            SWAP(j);
            return *this;
        }
        iterator operator--()
        {
            const auto *j = INTERNAL<S>::itr_fast_sub(i, 1);
            SWAP(j);
            return *this;
        }
        iterator operator+=(ssize_t k)
        {
            const auto *j = INTERNAL<S>::itr_fast_add(i, k);
            SWAP(j);
            return *this;
        }
        iterator operator-=(ssize_t k)
        {
            const auto *j = INTERNAL<S>::itr_fast_sub(i, k);
            SWAP(j);
            return *this;
        }

        /**
         * Extract the set used by the iterator: i.xs;
         * O(log N)
         */
        LIBFPP_PURE multiset value() const
        {
            return multiset(INTERNAL<S>::itr_revert(i));
        }
    };

    /**
     * Create an iterator to the start of a set: i := &xs[0];
     * O(1)
     */
    iterator begin() const
    {
        return iterator(s, 0);
    }

    /**
     * Create an iterator to the last element of a set: i := &xs[xs.size()-1];
     * O(1)
     */
    iterator rbegin() const
    {
        return iterator(s, s->len-1);
    }

    /**
     * Create an iterator to the end of a vector: i := &xs[xs.size()];
     * O(1)
     */
    static iterator end()
    {
        return iterator();
    }

    /**
     * Create an iterator to the element `val' if it exists, or else return end()
     * O(log N)
     */
    iterator find(const T &val) const
    {
        ssize_t r = INTERNAL<S>::seq_multi_find(s, reinterpret_cast<const void *>(&val), COMPARE);
        return (r < 0? iterator(): iterator(s, r));
    }

    /**
     * Set union.
     */
    multiset operator+=(const multiset &xs)
    {
        for (const auto x: xs)
            insert(x);
        return *this;
    }
    multiset operator+(const multiset &xs) const
    {
        multiset ys = *this;
        for (const auto x: xs)
            ys.insert(x);
        return ys;
    }

    /**
     * Set difference.
     */
    multiset operator-=(const multiset &xs)
    {
        for (const auto x: xs)
            erase(x);
        return *this;
    }
    multiset operator-(const multiset &xs) const
    {
        multiset ys = *this;
        for (const auto x: xs)
            ys.erase(x);
        return ys;
    }

    /**
     * Set insertion.
     */
    multiset operator+=(const T &x)
    {
        insert(x);
        return *this;
    }
    LIBFPP_PURE multiset operator+(const T &x) const
    {
        multiset xs = *this;
        xs.insert(x);
        return xs;
    }

    /**
     * Set erasure.
     */
    multiset operator-=(const T &x) const
    {
        erase(x);
        return *this;
    }
    LIBFPP_PURE multiset operator-(const T &x) const
    {
        multiset xs = *this;
        xs.erase(x);
        return xs;
    }

    /**
     * Comparison
     * O(N)
     */
    LIBFPP_PURE int compare(const multiset &xs) const
    {
        return base::compare(xs);
    }
    LIBFPP_PURE bool operator<(const multiset &xs) const
    {
        return compare(xs) < 0;
    }
    LIBFPP_PURE bool operator<=(const multiset &xs) const
    {
        return compare(xs) <= 0;
    }
    LIBFPP_PURE bool operator>(const multiset &xs) const
    {
        return compare(xs) > 0;
    }
    LIBFPP_PURE bool operator>=(const multiset &xs) const
    {
        return compare(xs) >= 0;
    }
    LIBFPP_PURE bool operator==(const multiset &xs) const
    {
        return compare(xs) == 0;
    }
    LIBFPP_PURE bool operator!=(const multiset &xs) const
    {
        return compare(xs) != 0;
    }
};

/**
 * Set
 */
template <typename T, typename Cmp = cmp<T>, typename S = LOCAL>
struct set : public multiset<T, Cmp, S>
{
    using base = multiset<T, Cmp, S>;

protected:

    using base::s;
    using base::COMPARE;
    using base::SWAP;

public:

    using base::multiset;
    using base::size;
    using base::empty;
    using base::clear;
    using base::at;
    using base::operator[];
    using base::back;
    using base::front;
    void insert(index, const T &) = delete;
    using base::pop_back;
    using base::pop_front;
    using base::erase;
    using base::left;
    using base::right;
    using base::compare;
    using base::dump;
    using base::verify;

    /**
     * Return the index of the element.
     * O(log N)
     */
    LIBFPP_PURE index pos(const T &val) const
    {
        return index{INTERNAL<S>::seq_find(s, reinterpret_cast<const void *>(&val), COMPARE)};
    }

    /**
     * Insert an element: xs := xs union {val}
     * O(log(N))
     */
    bool insert(const T &val)
    {
        if (LIBFPP_UNLIKELY(s->len >= LEN_MAX)) LIBFPP_PANIC("set is full");
        auto r = INTERNAL<S>::seq_search_insert(s, reinterpret_cast<const void *>(&val), COMPARE, INTERNAL<S>::template tree_search_insert<T, Cmp>, nullptr);
        SWAP(r.a);
        return r.b;
    }

    struct iterator : F::iterator<T, S, MIN>
    {
        using base = F::iterator<T, S, MIN>;
        using base::operator-;
        using base::iterator;
        using base::i;
        using base::SWAP;

        LIBFPP_PURE iterator operator+(ssize_t k) const
        {
            const auto *j = INTERNAL<S>::itr_add(i, k);
            return iterator(j);
        }
        LIBFPP_PURE iterator operator-(ssize_t k) const
        {
            const auto *j = INTERNAL<S>::itr_sub(i, k);
            return iterator(j);
        }
        iterator operator++(int)
        {
            iterator old(i);
            const auto *j = INTERNAL<S>::itr_add(i, 1);
            SWAP(j);
            return old;
        }
        iterator operator--(int)
        {
            iterator old(i);
            const auto *j = INTERNAL<S>::itr_sub(i, 1);
            SWAP(j);
            return old;
        }
        iterator operator++()
        {
            const auto *j = INTERNAL<S>::itr_fast_add(i, 1);
            SWAP(j);
            return *this;
        }
        iterator operator--()
        {
            const auto *j = INTERNAL<S>::itr_fast_sub(i, 1);
            SWAP(j);
            return *this;
        }
        iterator operator+=(ssize_t k)
        {
            const auto *j = INTERNAL<S>::itr_fast_add(i, k);
            SWAP(j);
            return *this;
        }
        iterator operator-=(ssize_t k)
        {
            const auto *j = INTERNAL<S>::itr_fast_sub(i, k);
            SWAP(j);
            return *this;
        }

        /**
         * Extract the set used by the iterator: i.xs;
         * O(log N)
         */
        LIBFPP_PURE set value() const
        {
            return set(INTERNAL<S>::itr_revert(i));
        }
    };

    /**
     * Create an iterator to the start of a set: i := &xs[0];
     * O(1)
     */
    iterator begin() const
    {
        return iterator(s, 0);
    }

    /**
     * Create an iterator to the last element of a set: i := &xs[xs.size()-1];
     * O(1)
     */
    iterator rbegin() const
    {
        return iterator(s, s->len-1);
    }

    /**
     * Create an iterator to the end of a vector: i := &xs[xs.size()];
     * O(1)
     */
    static iterator end()
    {
        return iterator();
    }

    /**
     * Create an iterator to the element `val' if it exists, or else return end()
     * O(log N)
     */
    iterator find(const T &val) const
    {
        ssize_t r = INTERNAL<S>::seq_find(s, reinterpret_cast<const void *>(&val), COMPARE);
        return (r < 0? iterator(): iterator(s, r));
    }

    /**
     * Set union.
     */
    set operator+=(const set &xs)
    {
        for (const auto x: xs)
            insert(x);
        return *this;
    }
    LIBFPP_PURE set operator+(const set &xs) const
    {
        set ys = *this;
        for (const auto x: xs)
            ys.insert(x);
        return ys;
    }

    /**
     * Set difference.
     */
    set operator-=(const set &xs)
    {
        for (const auto x: xs)
            erase(x);
        return *this;
    }
    LIBFPP_PURE set operator-(const set &xs) const
    {
        auto i = begin(), iend = end();
        for (; i != iend; )
        {
            if (xs.contains(*i)) i.erase();
            else                 ++i;
        }
        return i.value();
    }

    /**
     * Set insertion.
     */
    set operator+=(const T &x)
    {
        insert(x);
        return *this;
    }
    LIBFPP_PURE set operator+(const T &x) const
    {
        set xs = *this;
        xs.insert(x);
        return xs;
    }

    /**
     * Set erasure.
     */
    set operator-=(const T &x) const
    {
        erase(x);
        return *this;
    }
    LIBFPP_PURE set operator-(const T &x) const
    {
        set xs = *this;
        xs.erase(x);
        return xs;
    }

    /**
     * Set intersection.
     */
    void intersect(const set &xs)
    {
        auto i = begin(), iend = end();
        for (; i != iend; )
        {
            if (!xs.contains(*i)) i.erase();
            else                  ++i;
        }
        *this = i.value();
    }

    /**
     * Comparison
     * O(N)
     */
    LIBFPP_PURE int compare(const set &xs) const
    {
        return base::compare(xs);
    }
    LIBFPP_PURE bool operator<(const set &xs) const
    {
        return compare(xs) < 0;
    }
    LIBFPP_PURE bool operator<=(const set &xs) const
    {
        return compare(xs) <= 0;
    }
    LIBFPP_PURE bool operator>(const set &xs) const
    {
        return compare(xs) > 0;
    }
    LIBFPP_PURE bool operator>=(const set &xs) const
    {
        return compare(xs) >= 0;
    }
    LIBFPP_PURE bool operator==(const set &xs) const
    {
        return compare(xs) == 0;
    }
    LIBFPP_PURE bool operator!=(const set &xs) const
    {
        return compare(xs) != 0;
    }
};

/**
 * Multi-Map
 */
template <typename K, typename V, typename Cmp = cmp<K>, typename S = LOCAL>
struct multimap : public multiset<pair<K, V>, pair_cmp<K, V, Cmp>, S>
{
    using base = multiset<pair<K, V>, pair_cmp<K, V, Cmp>, S>;

protected:

    using base::s;
    using base::COMPARE;
    using base::SWAP;

public:

    using base::multiset;
    using base::size;
    using base::empty;
    using base::clear;
    using base::at;
    using base::operator[];
    using base::insert;
    using base::assign;
    using base::back;
    using base::front;
    using base::pop_back;
    using base::pop_front;
    using base::erase;
    using base::left;
    using base::right;
    using base::dump;
    using base::verify;
    using base::compare;

    /**
     * Return the index of the element.
     * O(log N)
     */
    LIBFPP_PURE index pos(const K &k) const
    {
        return index{INTERNAL<S>::seq_multi_find(s, reinterpret_cast<const void *>(&k), COMPARE)};
    }

    /**
     * Return element: xs[key]
     * O(log N)
     */
    LIBFPP_PURE optional<V, cmp<V>, S> search(const K &k) const
    {
        const auto *r = INTERNAL<S>::seq_search(s, reinterpret_cast<const void *>(&k), COMPARE);
        if (r == nullptr) return optional<V, cmp<V>, S>();
        return optional<V, cmp<V>, S>(reinterpret_cast<const pair<K, V> *>(r)->second);
    }

    /**
     * Checks if the map contains the key: {key, _} in xs
     * O(log N)
     */
    LIBFPP_PURE bool contains(const K &k) const
    {
        return (INTERNAL<S>::seq_search(s, reinterpret_cast<const void *>(&k), COMPARE) != nullptr);
    }

    /**
     * Return element: xs[key]
     * O(log N)
     */
    LIBFPP_PURE V at(const K &k) const
    {
        const auto *r = INTERNAL<S>::seq_search(s, reinterpret_cast<const void *>(&k), COMPARE);
        if (r == nullptr) LIBFPP_PANIC("key not found");
        return reinterpret_cast<const pair<K, V> *>(r)->second;
    }

    /**
     * Return (the map's copy of) the element: xs[key]
     * O(log N)
     */
    LIBFPP_PURE V operator[](const K &k) const
    {
        const auto *r = INTERNAL<S>::seq_search(s, reinterpret_cast<const void *>(&k), COMPARE);
        if (r == nullptr) LIBFPP_PANIC("key not found");
        return reinterpret_cast<const pair<K, V> *>(r)->second;
    }

    /**
     * Erase an element: xs := xs - {(key,val)}
     * O(log(N))
     */
    bool erase(const K &k)
    {
        auto r = INTERNAL<S>::seq_search_erase(s, reinterpret_cast<const void *>(&k), COMPARE, INTERNAL<S>::template tree_search_erase<pair<K, V>, pair_cmp<K, V, Cmp>>, nullptr);
        SWAP(r.a);
        return r.b;
    }

    /**
     * Split a map: (xs, ys) := (xs[0:idx-1],xs[idx:xs.size()-1])
     * O(log(n))
     */
    const multimap split(const index idx)
    {
        auto r = INTERNAL<S>::seq_split(s, idx, INTERNAL<S>::template tree_left<pair<K, V>, MIN>, INTERNAL<S>::template tree_right<pair<K, V>, MIN>);
        SWAP(r.b);
        return multimap(r.a);
    }

    struct iterator : F::iterator<pair<K, V>, S, MIN>
    {
        using base = F::iterator<pair<K, V>, S, MIN>;
        using base::operator-;
        using base::iterator;
        using base::i;
        using base::SWAP;

        LIBFPP_PURE iterator operator+(ssize_t k) const
        {
            const auto *j = INTERNAL<S>::itr_add(i, k);
            return iterator(j);
        }
        LIBFPP_PURE iterator operator-(ssize_t k) const
        {
            const auto *j = INTERNAL<S>::itr_sub(i, k);
            return iterator(j);
        }
        iterator operator++(int)
        {
            iterator old(i);
            const auto *j = INTERNAL<S>::itr_add(i, 1);
            SWAP(j);
            return old;
        }
        iterator operator--(int)
        {
            iterator old(i);
            const auto *j = INTERNAL<S>::itr_sub(i, 1);
            SWAP(j);
            return old;
        }
        iterator operator++()
        {
            const auto *j = INTERNAL<S>::itr_fast_add(i, 1);
            SWAP(j);
            return *this;
        }
        iterator operator--()
        {
            const auto *j = INTERNAL<S>::itr_fast_sub(i, 1);
            SWAP(j);
            return *this;
        }
        iterator operator+=(ssize_t k)
        {
            const auto *j = INTERNAL<S>::itr_fast_add(i, k);
            SWAP(j);
            return *this;
        }
        iterator operator-=(ssize_t k)
        {
            const auto *j = INTERNAL<S>::itr_fast_sub(i, k);
            SWAP(j);
            return *this;
        }

        /**
         * Extract the set used by the iterator: i.xs;
         * O(log N)
         */
        LIBFPP_PURE multimap value() const
        {
            return multimap(INTERNAL<S>::itr_revert(i));
        }
    };

    /**
     * Create an iterator to the start of a map: i := &xs[0];
     * O(1)
     */
    iterator begin() const
    {
        return iterator(s, 0);
    }

    /**
     * Create an iterator to the last element of a map: i := &xs[xs.size()-1];
     * O(1)
     */
    iterator rbegin() const
    {
        return iterator(s, s->len-1);
    }

    /**
     * Create an iterator to the end of a map: i := &xs[xs.size()];
     * O(1)
     */
    static iterator end()
    {
        return iterator();
    }

    /**
     * Create an iterator to the element indexed by `key' if it exists, or else return end()
     * O(log N)
     */
    iterator find(const K &k) const
    {
        ssize_t r = INTERNAL<S>::seq_multi_find(s, reinterpret_cast<const void *>(&k), COMPARE);
        return (r < 0? iterator(): iterator(s, r));
    }

    /**
     * Comparison
     * O(N)
     */
    LIBFPP_PURE int compare(const multimap &xs) const
    {
        return base::compare(xs);
    }
    LIBFPP_PURE bool operator<(const multimap &xs) const
    {
        return compare(xs) < 0;
    }
    LIBFPP_PURE bool operator<=(const multimap &xs) const
    {
        return compare(xs) <= 0;
    }
    LIBFPP_PURE bool operator>(const multimap &xs) const
    {
        return compare(xs) > 0;
    }
    LIBFPP_PURE bool operator>=(const multimap &xs) const
    {
        return compare(xs) >= 0;
    }
    LIBFPP_PURE bool operator==(const multimap &xs) const
    {
        return compare(xs) == 0;
    }
    LIBFPP_PURE bool operator!=(const multimap &xs) const
    {
        return compare(xs) != 0;
    }
};

/**
 * Map
 */
template <typename K, typename V, typename Cmp = cmp<K>, typename S = LOCAL>
struct map : public set<pair<K, V>, pair_cmp<K, V, Cmp>, S>
{
    using base = set<pair<K, V>, pair_cmp<K, V, Cmp>, S>;

protected:

    using base::s;
    using base::COMPARE;
    using base::SWAP;

public:
    using base::set;
    using base::size;
    using base::empty;
    using base::clear;
    using base::at;
    using base::operator[];
    using base::insert;
    using base::assign;
    using base::back;
    using base::front;
    using base::pop_back;
    using base::pop_front;
    using base::erase;
    using base::left;
    using base::right;
    using base::dump;
    using base::verify;
    using base::compare;

    /**
     * Return the index of the element.
     * O(log N)
     */
    LIBFPP_PURE index pos(const K &k) const
    {
        return index{INTERNAL<S>::seq_find(s, reinterpret_cast<const void *>(&k), COMPARE)};
    }

    /**
     * Return (the map's copy of) the element: xs[key]
     * O(log N)
     */
    LIBFPP_PURE optional<V, cmp<V>, S> search(const K &k) const
    {
        const auto *r = INTERNAL<S>::seq_search(s, reinterpret_cast<const void *>(&k), COMPARE);
        if (r == nullptr) return optional<V, cmp<V>, S>();
        return optional<V, cmp<V>, S>(reinterpret_cast<const pair<K, V> *>(r)->second);
    }

    /**
     * Checks if the map contains a key: (key,_) in xs
     * O(log N)
     */
    LIBFPP_PURE bool contains(const K &k) const
    {
        return (INTERNAL<S>::seq_search(s, reinterpret_cast<const void *>(&k), COMPARE) != nullptr);
    }

    /**
     * Return (the map's copy of) the element: xs[key]
     * O(log N)
     */
    LIBFPP_PURE V at(const K &k) const
    {
        const auto *r = INTERNAL<S>::seq_search(s, reinterpret_cast<const void *>(&k), COMPARE);
        if (r == nullptr) LIBFPP_PANIC("key not found");
        return reinterpret_cast<const pair<K, V> *>(r)->second;
    }

    /**
     * Return (the map's copy of) the element: xs[key]
     * O(log N)
     */
    LIBFPP_PURE V operator[](const K &k) const
    {
        const auto *r = INTERNAL<S>::seq_search(s, reinterpret_cast<const void *>(&k), COMPARE);
        if (r == nullptr) LIBFPP_PANIC("key not found");
        return reinterpret_cast<const pair<K, V> *>(r)->second;
    }

    /**
     * Erase an element: xs := xs - {(key,xs[key])}
     * O(log(N))
     */
    bool erase(const K &k)
    {
        auto r = INTERNAL<S>::seq_search_erase(s, reinterpret_cast<const void *>(&k), COMPARE, INTERNAL<S>::template tree_search_erase<pair<K, V>, pair_cmp<K, V, Cmp>>, nullptr);
        SWAP(r.a);
        return r.b;
    }

    /**
     * Split a map: (xs, ys) := (xs[0:idx-1],xs[idx:xs.size()-1])
     * O(log(n))
     */
    const map split(const index idx)
    {
        auto r = INTERNAL<S>::seq_split(s, idx, INTERNAL<S>::template tree_left<pair<K, V>, MIN>, INTERNAL<S>::template tree_right<pair<K, V>, MIN>);
        SWAP(r.b);
        return map(r.a);
    }

    struct iterator : F::iterator<pair<K, V>, S, MIN>
    {
        using base = F::iterator<pair<K, V>, S, MIN>;
        using base::operator-;
        using base::iterator;
        using base::i;
        using base::SWAP;

        LIBFPP_PURE iterator operator+(ssize_t k) const
        {
            const auto *j = INTERNAL<S>::itr_add(i, k);
            return iterator(j);
        }
        LIBFPP_PURE iterator operator-(ssize_t k) const
        {
            const auto *j = INTERNAL<S>::itr_sub(i, k);
            return iterator(j);
        }
        iterator operator++(int)
        {
            iterator old(i);
            const auto *j = INTERNAL<S>::itr_add(i, 1);
            SWAP(j);
            return old;
        }
        iterator operator--(int)
        {
            iterator old(i);
            const auto *j = INTERNAL<S>::itr_sub(i, 1);
            SWAP(j);
            return old;
        }
        iterator operator++()
        {
            const auto *j = INTERNAL<S>::itr_fast_add(i, 1);
            SWAP(j);
            return *this;
        }
        iterator operator--()
        {
            const auto *j = INTERNAL<S>::itr_fast_sub(i, 1);
            SWAP(j);
            return *this;
        }
        iterator operator+=(ssize_t k)
        {
            const auto *j = INTERNAL<S>::itr_fast_add(i, k);
            SWAP(j);
            return *this;
        }
        iterator operator-=(ssize_t k)
        {
            const auto *j = INTERNAL<S>::itr_fast_sub(i, k);
            SWAP(j);
            return *this;
        }

        /**
         * Extract the set used by the iterator: i.xs;
         * O(log N)
         */
        LIBFPP_PURE map value() const
        {
            return map(INTERNAL<S>::itr_revert(i));
        }
    };

    /**
     * Create an iterator to the start of a map: i := &xs[0];
     * O(1)
     */
    iterator begin() const
    {
        return iterator(s, 0);
    }

    /**
     * Create an iterator to the last element of a map: i := &xs[xs.size()-1];
     * O(1)
     */
    iterator rbegin() const
    {
        return iterator(s, s->len-1);
    }

    /**
     * Create an iterator to the end of a map: i := &xs[xs.size()];
     * O(1)
     */
    static iterator end()
    {
        return iterator();
    }

    /**
     * Create an iterator to the element indexed by `key' if it exists, or else return end()
     * O(log N)
     */
    iterator find(const K &k) const
    {
        ssize_t r = INTERNAL<S>::seq_find(s, reinterpret_cast<const void *>(&k), COMPARE);
        return (r < 0? iterator(): iterator(s, r));
    }

    /**
     * Comparison
     * O(N)
     */
    LIBFPP_PURE int compare(const map &xs) const
    {
        return base::compare(xs);
    }
    LIBFPP_PURE bool operator<(const map &xs) const
    {
        return compare(xs) < 0;
    }
    LIBFPP_PURE bool operator<=(const map &xs) const
    {
        return compare(xs) <= 0;
    }
    LIBFPP_PURE bool operator>(const map &xs) const
    {
        return compare(xs) > 0;
    }
    LIBFPP_PURE bool operator>=(const map &xs) const
    {
        return compare(xs) >= 0;
    }
    LIBFPP_PURE bool operator==(const map &xs) const
    {
        return compare(xs) == 0;
    }
    LIBFPP_PURE bool operator!=(const map &xs) const
    {
        return compare(xs) != 0;
    }
};

/**
 * String
 */
template <typename S = LOCAL>
struct string : public vector<unsigned char, S, STR>
{
    using base = vector<unsigned char, S, STR>;

protected:

    using base::s;
    using base::SWAP;

public:

    using base::vector;
    using base::size;
    using base::empty;
    using base::clear;
    using base::pop_back;
    using base::pop_front;
    using base::append;
    using base::split;
    using base::left;
    using base::right;
    using base::compare;
    using base::dump;
    using base::verify;

    string() : base() { }
 
    /**
     * Create a string from a UTF8-encoded C string.
     * O(strlen(cstr))
     */
    string(const char *cstr, const char *cmax = nullptr) : base(INTERNAL<S>::str_push_back(nil<S, STR>(), cstr, cmax))
    {
        ;
    }
    
    /**
     * Return the number of charaters in the string: |s|
     * O(1)
     */
    LIBFPP_PURE size_t size() const
    {
        return s->monoid.len;
    }

    /**
     * Return the string as a C string represented as an array.
     * O(N)
     */
    LIBFPP_PURE array<char, int, S> str() const
    {
        size_t len = s->len + 1;
        void *ptr = S::malloc(len);
        INTERNAL<S>::str_write(s, ptr, INTERNAL<S>::template tree_write<unsigned char, STR>);
        *(reinterpret_cast<char *>(ptr) + len - 1) = '\0';
        return array<char, int, S>(reinterpret_cast<char *>(ptr), len);
    }

    /**
     * Return the nth element of the string: s[idx]
     * O(log N)
     */
    LIBFPP_PURE char32_t at(const index idx) const
    {
        return INTERNAL<S>::str_get(s, idx);
    }

    /**
     * Return the nth character of the string: s[idx]
     * O(log N)
     */
    LIBFPP_PURE char32_t operator[](const index idx) const
    {
        return INTERNAL<S>::str_get(s, idx);
    }

    /**
     * Set the nth character of the string: s = s[0:idx-1] ++ [c] ++ * s[idx+1:s.size()-1]
     * O(log N)
     */
    void assign(const index idx, char32_t c)
    {
        const auto *r = INTERNAL<S>::str_assign(s, idx, c);
        SWAP(r);
    }

    /**
     * Return the last character of the string: s[s.size()-1]
     * O(1)
     */
    LIBFPP_PURE char32_t back() const
    {
        return INTERNAL<S>::str_get(s, s->len-1);
    }

    /**
     * Return the front of the vector: xs[0]
     * O(1)
     */
    LIBFPP_PURE char32_t front() const
    {
        return INTERNAL<S>::str_get(s, 0);
    }

    /**
     * Push a character to the back of a string: s := s ++ [c] 
     * O(1)
     */
    void push_back(char32_t c)
    {
        if (LIBFPP_UNLIKELY(s->len >= STR_MAX)) LIBFPP_PANIC("string is full");
        const auto *r = INTERNAL<S>::str_fast_push_back(s, c);
        SWAP(r);
    }

    /**
     * Push a character to the back of a string: s := [c] ++ s 
     * O(1)
     */
    void push_front(char32_t c)
    {
        if (LIBFPP_UNLIKELY(s->len >= STR_MAX)) LIBFPP_PANIC("string is full");
        const auto *r = INTERNAL<S>::str_fast_push_front(s, c);
        SWAP(r);
    }

    /**
     * Push a character to the back of a string: s := s ++ [c] 
     * O(1)
     */
    string operator+=(char32_t c)
    {
        push_back(c);
        return *this;
    }

    /**
     * Push a UTF8-encoded C string to the back of a string: s := s ++ cstr
     * O(strlen(cstr))
     */
    void push_back(const char *cstr)
    {
        const auto *r = INTERNAL<S>::str_push_back(s, cstr, nullptr);
        SWAP(r);
    }

    /**
     * Push a UTF8-encoded C string to the front of a string: s := cstr ++ s
     * O(strlen(cstr));
     */
    void push_front(const char *cstr)
    {
        const auto *r = INTERNAL<S>::str_push_front(s, cstr, nullptr);
        SWAP(r);
    }

    /**
     * Insert a character at the given position: xs := xs[0:idx-1] ++ [c] ++ xs[idx:xs.size()-1]
     * O(log(N))
     */
    void insert(const index idx, char32_t c)
    {
        if (LIBFPP_UNLIKELY(s->len >= STR_MAX)) LIBFPP_PANIC("string is full");
        const auto *r = INTERNAL<S>::str_insert(s, idx, c);
        SWAP(r);
    }

    /**
     * Erase a character from the given position: xs := xs[0:idx-1] ++ xs[idx+1:zs.size()-1]
     * O(log(N))
     */
    void erase(const index idx)
    {
        const auto *r = INTERNAL<S>::str_erase(s, idx);
        SWAP(r);
    }

    /**
     * Erase a character range: xs := s[0:lo-1] ++ s[hi+1:s.size()-1]
     * O(log(N))
     */
    void erase(const index lo, const index hi)
    {
        const auto *r = INTERNAL<S>::str_erase(s, lo, hi);
        SWAP(r);
    }

    /**
     * Pop a character from the back of a string: s := s[0:s.size()-2]
     * O(1)
     */
    void pop_back()
    {
        const auto *r = INTERNAL<S>::str_pop_back(s);
        SWAP(r);
    }

    /**
     * Pop a character from the front of a string: s := s[1:s.size()-1]
     * O(1)
     */
    void pop_front()
    {
        const auto *r = INTERNAL<S>::str_pop_front(s);
        SWAP(r);
    }

    /**
     * Push a UTF8-encoded C string to the back of a string: s := s ++ cstr
     * O(strlen(cstr))
     */
    string operator+=(const char *cstr)
    {
        push_back(cstr);
        return *this;
    }

    /**
     * Push a UTF8-encoded array<char> to the back of a string: s := s ++ arr
     * O(arr.size())
     */
    string operator+=(const array<char> &arr)
    {
        push_back(arr.c_str());
        return *this;
    }

    /**
     * Append (concat) two strings: s := s + t
     * O(min(log(N), log(M)))
     */
    void append(const string &ys)
    {
        const auto *r = INTERNAL<S>::str_append(s, ys.s);
        SWAP(r);
    }

    /**
     * Append (concat) two strings: r := s + t
     * O(min(log(N), log(M)))
     */
    string operator+(const string &ys) const
    {
        const auto *r = INTERNAL<S>::str_append(s, ys.s);
        return string(r);
    }

    /**
     * Append (concat) two vectors: s := s + t
     * O(min(log(N), log(M)))
     */
    const string operator+=(const string &ys)
    {
        append(ys);
        return *this;
    }

    /**
     * Split a string: (xs, ys) := (xs[0:idx-1],xs[idx:xs.size()-1])
     * O(log(n))
     */
    const string split(const index idx)
    {
        auto r = INTERNAL<S>::str_split(s, idx);
        SWAP(r.b);
        return string(r.a);
    }

    /**
     * Truncate a string to the left: xs := xs[0:idx-1]
     * O(log(n))
     */
    void left(const index idx)
    {
        const auto *r = INTERNAL<S>::str_left(s, idx);
        SWAP(r);
    }

    /**
     * Truncate a string to the right: xs := xs[idx:xs.size()-1]
     * O(log(n))
     */
    void right(const index idx)
    {
        const auto *r = INTERNAL<S>::str_right(s, idx);
        SWAP(r);
    }

    /**
     * Insert a string at the given index: s := s[0:idx-1] ++ cstr ++ s[idx:s.size()-1]
     * O(log(n) + strlen(cstr))
     */
    void insert(const index idx, const char *cstr)
    {
        const auto *r = INTERNAL<S>::str_insert(s, idx, cstr);
        SWAP(r);
    }

    /**
     * Insert a string at the given index: s := s[0:idx-1] ++ t ++ s[idx:s.size()-1]
     * O(min(log(n), log(m)))
     */
    void insert(const index idx, const string &t)
    {
        const auto *r = INTERNAL<S>::str_insert(s, idx, t.s);
        SWAP(r);
    }

    /*
     * Convert a string into a vector<unsigned char>.
     * O(1)
     */
    operator vector<unsigned char>() const
    {
        vector<unsigned char> xs(reinterpret_cast<const SEQ<S> *>(s));
        return xs;
    }

    struct iterator : F::iterator<char, S, STR>
    {
        using base = F::iterator<char, S, STR>;
        using base::operator-;
        using base::iterator;
        using base::i;
        using base::SWAP;

        LIBFPP_PURE iterator operator+(ssize_t k) const
        {
            const auto *j = INTERNAL<S>::itr_add(i, k);
            return iterator(j);
        }
        LIBFPP_PURE iterator operator-(ssize_t k) const
        {
            const auto *j = INTERNAL<S>::itr_sub(i, k);
            return iterator(j);
        }
        iterator operator++(int)
        {
            iterator old(i);
            const auto *j = INTERNAL<S>::itr_add(i, 1);
            SWAP(j);
            return old;
        }
        iterator operator--(int)
        {
            iterator old(i);
            const auto *j = INTERNAL<S>::itr_sub(i, 1);
            SWAP(j);
            return old;
        }
        iterator operator++()
        {
            const auto *j = INTERNAL<S>::itr_fast_add(i, 1);
            SWAP(j);
            return *this;
        }
        iterator operator--()
        {
            const auto *j = INTERNAL<S>::itr_fast_sub(i, 1);
            SWAP(j);
            return *this;
        }
        iterator operator+=(ssize_t k)
        {
            const auto *j = INTERNAL<S>::itr_fast_add(i, k);
            SWAP(j);
            return *this;
        }
        iterator operator-=(ssize_t k)
        {
            const auto *j = INTERNAL<S>::itr_fast_sub(i, k);
            SWAP(j);
            return *this;
        }

        LIBFPP_PURE char32_t operator*()
        {
            auto r = INTERNAL<S>::itr_str_get(i);
            SWAP(r.a);
            return r.b;
        }

        void assign(char32_t c)
        {
            const auto *j = INTERNAL<S>::itr_str_assign(i, c);
            SWAP(j);
        }

        void insert(char32_t c)
        {
            const auto *j = INTERNAL<S>::itr_str_insert(i, c);
            SWAP(j);
        }

        void insert(const string &s)
        {
            const auto *j = INTERNAL<S>::itr_str_insert(i, s.s);
            SWAP(j);
        }

        void insert(const char *cstr)
        {
            const auto *j = INTERNAL<S>::itr_str_insert(i, cstr);
            SWAP(j);
        }

        void erase()
        {
            const auto *j = INTERNAL<S>::itr_str_erase(i);
            SWAP(j);
        }

        void erase(ssize_t n)
        {
            const auto *j = INTERNAL<S>::itr_str_erase(i, n);
            SWAP(j);
        }

        void erase(const iterator &j)
        {
            const auto *k = INTERNAL<S>::itr_str_erase(i, j.i);
            SWAP(k);
        }

        void slice(ssize_t n)
        {
            const auto *j = INTERNAL<S>::itr_str_slice(i, n);
            SWAP(j);
        }

        void slice(const iterator &j)
        {
            const auto *k = INTERNAL<S>::itr_str_slice(i, j.i);
            SWAP(k);
        }

        void replace(const iterator &j, const string &s)
        {
            const auto *k = INTERNAL<S>::itr_str_replace(i, j.i, s);
            SWAP(k);
        }

        void replace(const iterator &j, const char *cstr)
        {
            const auto *k = INTERNAL<S>::itr_str_replace(i, j.i, cstr);
            SWAP(k);
        }

        void left()
        {
            const auto *j = INTERNAL<S>::itr_str_left(i);
            SWAP(j);
        }

        void right()
        {
            const auto *j = INTERNAL<S>::itr_str_right(i);
            SWAP(j);
        }

        /**
         * Find the given character.
         * O(N)
         */
        void find(char32_t c);
 
        /**
         * Find the given substring.
         * O(N * M)
         */
        void find(const string &s);
        void find(const char *cstr);

        /**
         * Extract the string used by the iterator: i.xs;
         * O(log N)
         */
        LIBFPP_PURE string value()
        {
            return string(INTERNAL<S>::itr_revert(i));
        }
    };

    /**
     * Create an iterator to the start of a string: i := &xs[0];
     * O(1)
     */
    iterator begin() const
    {
        return iterator(s, 0);
    }

    /**
     * Create an iterator to the last element of a string: i := &xs[xs.size()-1];
     * O(1)
     */
    iterator rbegin() const
    {
        return iterator(s, s->len-1);
    }

    /**
     * Create an iterator to the end of a string: i := &xs[xs.size()];
     * O(1)
     */
    static iterator end()
    {
        return iterator();
    }

    /**
     * Comparison
     * O(N)
     */
    LIBFPP_PURE int compare(const string &xs) const
    {
        return base::compare(xs);
    }
    LIBFPP_PURE bool operator<(const string &xs) const
    {
        return compare(xs) < 0;
    }
    LIBFPP_PURE bool operator<=(const string &xs) const
    {
        return compare(xs) <= 0;
    }
    LIBFPP_PURE bool operator>(const string &xs) const
    {
        return compare(xs) > 0;
    }
    LIBFPP_PURE bool operator>=(const string &xs) const
    {
        return compare(xs) >= 0;
    }
    LIBFPP_PURE bool operator==(const string &xs) const
    {
        return compare(xs) == 0;
    }
    LIBFPP_PURE bool operator!=(const string &xs) const
    {
        return compare(xs) != 0;
    }
    LIBFPP_PURE bool operator<(const char *cstr) const
    {
        return INTERNAL<S>::str_compare(s, cstr) < 0;
    }
    LIBFPP_PURE bool operator<=(const char *cstr) const
    {
        return INTERNAL<S>::str_compare(s, cstr) <= 0;
    }
    LIBFPP_PURE bool operator>(const char *cstr) const
    {
        return INTERNAL<S>::str_compare(s, cstr) > 0;
    }
    LIBFPP_PURE bool operator>=(const char *cstr) const
    {
        return INTERNAL<S>::str_compare(s, cstr) >= 0;
    }
    LIBFPP_PURE bool operator==(const char *cstr) const
    {
        return INTERNAL<S>::str_compare(s, cstr) == 0;
    }
    LIBFPP_PURE bool operator!=(const char *cstr) const
    {
        return INTERNAL<S>::str_compare(s, cstr) != 0;
    }

    /*
     * Logical NOP, but shrink memory usage.
     * O(N)
     */
    void shrink_to_fit()
    {
        string s;
        for (auto i = begin(), iend = end(); i != iend; ++i)
            s.push_back(*i);
        *this = s;
    }

    /**
     * Read a file as a string
     * O(N)
     */
    void read(const char *filename);

    /**
     * Print a string
     */
    void print(FILE *stream = stdout) const
    {
        string str;
        str << *this;
        vector<unsigned char> cs(str);
        for (auto c: cs)
            fputc(c, stream);
        fputc('\n', stream);
    }
};

/*
 * [INTERNAL]
 */
template <typename S = LOCAL>
string<S> &operator<<(string<S> &s, int8_t i)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%d_i8", i);
    s.push_back(buf);
    return s;
}
template <typename S = LOCAL>
string<S> &operator<<(string<S> &s, int16_t i)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%d_i16", i);
    s.push_back(buf);
    return s;
}
template <typename S = LOCAL>
string<S> &operator<<(string<S> &s, int32_t i)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%d_i32", i);
    s.push_back(buf);
    return s;
}
template <typename S = LOCAL>
string<S> &operator<<(string<S> &s, int64_t i)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%ld_i64", i);
    s.push_back(buf);
    return s;
}
template <typename S = LOCAL>
string<S> &operator<<(string<S> &s, uint16_t i)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%d_u16", i);
    s.push_back(buf);
    return s;
}
template <typename S = LOCAL>
string<S> &operator<<(string<S> &s, uint32_t i)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%d_u32", i);
    s.push_back(buf);
    return s;
}
template <typename S = LOCAL>
string<S> &operator<<(string<S> &s, uint64_t i)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%ld_u64", i);
    s.push_back(buf);
    return s;
}
template <typename S = LOCAL>
string<S> &operator<<(string<S> &s, char32_t c)
{
    s.push_back('\'');
    switch (c)
    {
        case '\'': s.push_back("\\'"); break;
        case '\\': s.push_back("\\\\"); break;
        case '\n': s.push_back("\\n"); break;
        case '\t': s.push_back("\\t"); break;
        case '\r': s.push_back("\\r"); break;
        case '\b': s.push_back("\\b"); break;
        case '\f': s.push_back("\\f"); break;
        default:
            if (c < 0x20)
            {
                char buf[32];
                snprintf(buf, sizeof(buf), "\\u%.4X", static_cast<uint16_t>(c));
                s.push_back(buf);
                break;
            }
            s.push_back(c);
            break;
    }
    s.push_back('\'');
    return s;
}
template <typename S = LOCAL>
string<S> &operator<<(string<S> &s, char c)
{
    if (c < 0)
    {
        char buf[32];
        snprintf(buf, sizeof(buf), "'\\u%.4X'", 0xFF & static_cast<uint16_t>(c));
        s.push_back(buf);
        return s;
    }
    s << static_cast<char32_t>(c);
    s << "u";
    return s;
}
template <typename S = LOCAL>
string<S> &operator<<(string<S> &s, unsigned char c)
{
    if (c > 0x7F)
    {
        char buf[32];
        snprintf(buf, sizeof(buf), "'\\u%.4X'", 0xFF & static_cast<uint16_t>(c));
        s.push_back(buf);
        return s;
    }
    s << static_cast<char32_t>(c);
    return s;
}
template <typename S = LOCAL>
string<S> &operator<<(string<S> &s, const char *cstr)
{
    s.push_back(cstr);
    return s;
}
template <typename T, typename S = LOCAL>
string<S> &operator<<(string<S> &s, const vector<T> &xs)
{
    s.push_back('[');
    bool prev = false;
    for (auto x: xs)
    {
        if (prev)
            s.push_back(',');
        s << x;
        prev = true;
    }
    s.push_back(']');
    return s;
}
template <typename T, typename S = LOCAL>
string<S> &operator<<(string<S> &s, const set<T> &xs)
{
    s.push_back('{');
    bool prev = false;
    for (auto x: xs)
    {
        if (prev)
            s.push_back(',');
        s << x;
        prev = true;
    }
    s.push_back('}');
    return s;
}
template <typename K, typename V, typename S = LOCAL>
string<S> &operator<<(string<S> &s, const map<K, V> &xs)
{
    s.push_back('<');
    bool prev = false;
    for (auto x: xs)
    {
        if (prev)
            s.push_back(',');
        s << x.first;
        s.push_back(':');
        s << x.second;
        prev = true;
    }
    s.push_back('>');
    return s;
}
template <typename K, typename V, typename S = LOCAL>
string<S> &operator<<(string<S> &s, const pair<K, V> &x)
{
    s.push_back('<');
    s << x.first;
    s.push_back(':');
    s << x.second;
    s.push_back('>');
    return s;
}
template <typename S = LOCAL>
string<S> &operator<<(string<S> &s, const string<S> &str)
{
    s.push_back('"');
    for (char32_t c: str)
    {
        switch (c)
        {
            case '\"': s.push_back("\\\""); break;
            case '\\': s.push_back("\\\\"); break;
            case '\n': s.push_back("\\n"); break;
            case '\t': s.push_back("\\t"); break;
            case '\r': s.push_back("\\r"); break;
            case '\b': s.push_back("\\b"); break;
            case '\f': s.push_back("\\f"); break;
            default:
                if (c < 0x20)
                {
                    char buf[32];
                    snprintf(buf, sizeof(buf), "\\u%.4X", static_cast<uint16_t>(c));
                    s.push_back(buf);
                    break;
                }
                s.push_back(c);
                break;
        }
    }
    s.push_back('"');
    return s;
}

template <typename S>
void print(const string<S> &str, FILE *stream = stdout)
{
    vector<unsigned char> cs(str);
    for (auto c: cs)
        fputc(c, stream);
}

template <typename S>
template <typename T, typename M>
void INTERNAL<S>::tree_dump(const TREE<S, M> *t, FILE *stream)
{
    string<S> s;
    for (size_t i = 0; i < t->len; i++)
    {
        if (i > 0) fputc(',', stream);
        s.clear();
        s << *t->template at<T>(i);
        print(s, stream);
    }
}

}           /* namespace F */

#endif      /* define __LIBFPP_H */
