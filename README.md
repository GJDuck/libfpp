<h1 align="center">
  <img src=".github/libf++.png" width="150"/>
  &nbsp; Persistent Containers and Iterators for C++
</h1>

LibF++ is a library that implements persistent containers **and** iterators for C++.
Specifically, LibF++ aims to:

* Retain the full expressiveness of STL-style containers and iteration;
* Achieve the safety of persistent (immutable) data structures; and
* Use **value semantics** everywhere!

Basically everything in LibF++ is a value, including containers, objects stored in containers, and (importantly) **iterators are values**.
This means that iterators do not store references to the originating container.

LibF++ implements persistent versions of the following standard containers:

* `vector`
* (multi)`set`
* (multi)`map`
* UTF8-`string`

Why Value Semantics?
--------------------

LibF++ embraces **value semantics**: containers are treated as values, much like `int` in C++.
A value, once created, is never modified indirectly through aliases or references.
Instead of mutating an existing value, operations conceptually produce a new value, while existing values remain valid and unchanged.

Value semantics offer several practical advantages:

* **Local reasoning.** A value can only be changed through the variable that holds it. Operations cannot silently modify data through aliases, making code easier to understand and maintain.
* **No aliasing bugs.** Multiple variables can refer to the same logical value without risking accidental interference. Updating one value never changes another.
* **Natural snapshots and undo.** Previous versions remain valid after updates, making it easy to implement undo/redo, speculative execution, checkpoints, and versioned state.
* **Safer concurrency.** Immutable values can be freely shared between threads without synchronization, since reads cannot race with updates to existing values.

Under value semantics, variables may still be rebound to different values (just as an `int` variable can be assigned a new integer, but the values themselves exhibit *persistence*).
This makes containers easier to reason about, safe to share, and inexpensive to copy, while remaining consistent with familiar C++ value-oriented programming.

```cpp
    vector xs{1, 2, 3};
    vector ys = xs;         // save a snapshot
    xs.push_back(4);        // does NOT affect ys

    // xs == {1, 2, 3, 4}
    // ys == {1, 2, 3}
```

A distinctive feature of LibF++ is that **iterators also have value semantics**.
In the STL, iterators typically act as references into a container, allowing the container to be modified in place.
In LibF++, iterator operations instead modify an *iterator-local copy of the container*, leaving the original container unchanged.
The modified container can then be extracted as a new value:

```cpp
    auto it = xs.begin();
    it.erase();             // does NOT affect xs
    vector zs = it.value(); // get the iterator's version

    // xs == {1, 2, 3, 4}
    // zs == {2, 3, 4}
```

This allows familiar iterator-based algorithms and editing operations while preserving the value-semantic model throughout the library. and without safety risks such as iterator invalidation or unpredictable behavior.
For example, the following code is awkward or unsafe under conventional mutable iterators, but represents a well-defined single container pass under value semantics:

```cpp
  set<int> s = {-2, -1, 0, 1, 2, 3};
  for (auto it = s.begin(); it != s.end(); ++it)
      if (*it % 2 == 0)
          s.insert(2 * *it);

  // Safe: iterators are values, and updates produce new container values.
  // No iterator invalidation.
```

This works because the iterator `it` and container `s` are independent values, so updating one does not affect the other.

Safety
======

Traditional iterators in C++ are powerful, but notoriously fragile:

* Iterator invalidation
* Aliasing bugs
* Subtle side effects

LibF++ **eliminates these issues by design**.

Use Cases
=========

LibF++ is ideal for the following applications:

* Safety-critical code (no aliasing bugs)
* Undo/redo systems
* Snapshot-based state management
* Exploratory / speculative computation

Performance
===========

LibF++ is designed to match STL asymptotic complexity when possible:

| Operation          | STL             | LibF++   |
|--------------------|-----------------|----------|
| `push_back`        | O(1)            | O(1)     |
| erase via iterator | O(N) (`vector`) | O(1)     |
| iteration (`++it`) | O(1)            | O(1)     |
| random access      | O(1) (`vector`) | O(log N) |
| copying            | O(N)            | O(1)     |

In addition, LibF++ implements several optimizations:

* LibF++ uses structural sharing (no deep copies);
* Efficient iterator updates via lazy reconstruction;
* Optimized memory allocation for performance

That said, tree-based persistent data structures generally incur a higher constant-factor overheads compared to mutable STL equivalents.
This is especially true for array-based STL containers such as `std::vector`.

Implementation
==============

All LibF++ containers are implemented using *finger trees* as a universal abstraction, enabling:

* Amortized O(1) operations at ends;
* O(log N) access/modification; and
* Structural sharing across versions (no deep copying).

Persistent iterators are implemented as *zippers*:

* A zipper represents a cursor into a persistent structure;
* Allows efficient navigation and updates; and
* Enables structural sharing between iterator states.

Example
=======

To illustrate the differences between STL and LibF++, we consider example functions that filter non-ascii characters from a string.

This function can be implemented using STL as follows:

```cpp
    void filterAscii(std::string &s)
    {
        std::string t;
        for (auto c: s)
            if (c < 0x7F)
                t += c;
        s.swap(t);
    }
```

Alternatively, this function could be implemented using STL iterators, as follows:

```cpp
    void filterAscii(std::string &s)
    {
        for (auto i = s.begin(); i != s.end(); )
            if (*i >= 0x7F)  i = s.erase(i);
            else            ++i;
    }
```

This version is space efficient (does not create a temporary string), but is time inefficient since iterator `erase` is an O(N) operation.

In contrast, the idiomatic LibF++ version is as follows:

```cpp
    string filterAscii(string s)
    {
        string::iterator i;
        for (i = s.begin(); i != s.end(); )
            if (*i >= 0x7F) i.erase();
            else            ++i;
        return i.value();
    }
```

The libF++ version:

* Uses value semantics (no mutation), since the iterator `i` operates on its own independent copy of `s`;
* Is time efficient, since `erase` via an iterator is an O(1) operation; and
* Is space efficient, since unchanged elements are structurally shared with the original value;
* Is abstract: same algorithm for strings, vectors, sets, maps, etc.

The special call `i.value()` is necessary.
Since LibF++ uses value semantics, the `i.erase()` operation only affects the iterator-local copy of the container.
The modified container must therefore be explicitly extracted from the resulting iterator value.

Building
========

To build `libf++.so` simply run `make`.

To build the test cases, run `make test`.

To build the fuzzer, run `make fuzz`.

Limitations
===========

Libf++ should currently be considered **experimental software**.
While the library is usable and many code paths are well tested, test coverage is uneven and less frequently used functionality may still contain undiscovered bugs.
Users should expect occasional correctness issues and are encouraged to report them.

The API is not yet considered stable and may change between releases as the library evolves.
Feedback and API suggestions are welcome.

The implementation is complex and remains a work in progress.
Some areas of the code-base are not as clean or maintainable as they should be, and there are known instances of undefined behavior related to low-level type-punning techniques that have not yet been fully addressed.
Currently LibF++ only compiles using `g++`.

Although the library provides support for multi-threaded use, development and testing have been performed primarily in single-threaded environments.
Concurrent workloads are therefore less exercised and may expose bugs or performance issues that have not yet been discovered.

Performance is generally good, but the implementation is not fully optimized.
In particular, some data structures currently require more internal nodes and allocations than strictly necessary.
Additional space and time optimizations are planned for future releases.

LibF++ is **not** intended to be a general STL replacement for performance-critical paths, as persistent structures are inherently slower than mutable ones.
Rather, LibF++ aims for correctness, safety, expressiveness, and ergonomics.

License
=======

LibF++ is released under the MIT license.

Publication
===========

* Yihe Li, Gregory J. Duck, [Persistent Iterators with Value Semantics](https://www.comp.nus.edu.sg/~gregory/papers/pldi2026.pdf), Programming Language Design and Implementation (PLDI), 2026

