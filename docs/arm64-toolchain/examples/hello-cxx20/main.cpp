#include <concepts>
#include <iostream>
#include <ranges>
#include <span>

template <std::integral T>
constexpr T sum(std::span<const T> values) {
    T result{};
    for (T value : values) result += value;
    return result;
}

int main() {
    static_assert(__cplusplus >= 202002L);
#ifndef __aarch64__
#error This test must be cross compiled for AArch64
#endif
    constexpr int values[] = {1, 2, 3, 4};
    static_assert(sum<int>(values) == 10);
    int even_sum = 0;
    for (int n : values | std::views::filter([](int v) { return v % 2 == 0; }))
        even_sum += n;
    std::cout << "Hello World from NanoPC-T4 ARM64!\n"
              << "C++ standard: " << __cplusplus << '\n'
              << "Compiler: " << __VERSION__ << '\n'
              << "std::span + concepts sum: " << sum<int>(values) << '\n'
              << "std::ranges even sum: " << even_sum << '\n';
    return sum<int>(values) == 10 && even_sum == 6 ? 0 : 1;
}
