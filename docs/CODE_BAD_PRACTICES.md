# Anti-Patterns: Bad Practices to Avoid in Modern C++

## 1. Legacy Memory & Resource Management

**Avoid naked new and delete:** Manual memory management easily leads to memory leaks and dangling pointers. Instead, embrace Resource Acquisition Is Initialization (RAII) and use smart pointers like `std::make_unique` or `std::make_shared`.

**Avoid C-style arrays:** Standard raw arrays (`int arr[10]`) decay into raw pointers when passed to functions, losing their size information. Use `std::array` for fixed-size containers and `std::vector` for dynamic ones.

**Avoid malloc and free:** These functions do not invoke object constructors or destructors. They have no place in modern C++ unless you are writing custom low-level allocators.

## 2. "C with Classes" & Java Dialect

**Avoid treating C++ like C:** Relying heavily on macros for constants, using null-terminated char* arrays instead of `std::string` or `std::string_view`, and omitting namespaces are habits that bypass C++'s type safety.

**Avoid treating C++ like Java:** C++ features efficient value semantics. You do not need to create every single object on the heap using pointers just because that is how garbage-collected languages operate.

**Avoid using namespace std; in header files:** Doing this pollutes the global namespace of any file that includes your header, frequently causing hard-to-debug name collisions.

## 3. Fragile & Unsafe Language Features

**Avoid raw pointer arithmetic:** Modifying pointers directly bypasses bounds checking and is a primary driver of buffer overflows and lifetime safety errors.

**Avoid old-style C casts:** Writing `(int)my_variable` forces a conversion without compile-time checks. Use explicit, safer C++ casts like `static_cast`, `const_cast`, or `reinterpret_cast`.

**Avoid NULL or 0 for null pointers:** Always use `nullptr`. It is strongly typed and prevents accidental function overload mismatches where an integer is expected instead of a pointer.

**Avoid std::vector<bool> if you need standard element references:** This container is heavily optimized as a bit-field. Because individual bits cannot be directly pointed to via standard references, it behaves differently than every other standard vector and can break templates.
