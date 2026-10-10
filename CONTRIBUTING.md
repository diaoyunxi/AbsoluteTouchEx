# Contributing

## Development Setup

### Requirements
- C++17 compatible compiler (GCC 7+, Clang 5+, MSVC 2017+)
- CMake 3.14+ (if applicable)

## How to Contribute

1. Fork and create a branch
2. Follow the existing C++ coding style
3. Ensure code compiles without warnings (`-Wall -Wextra`)
4. Add tests for new functionality
5. Submit a PR

### Code Style
- Use modern C++ (C++17+)
- Prefer RAII and smart pointers
- Use `std::string` over C strings
- Avoid raw `new`/`delete`
- Prefer `constexpr` over `#define`

### Commit Format
```
type(module): description
```
