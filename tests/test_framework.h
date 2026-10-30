// 极简测试框架：注册用例 + 断言，不引入第三方库。
#pragma once

#include <cstdio>
#include <sstream>
#include <string>
#include <type_traits>
#include <vector>

namespace chat::test {

struct Case {
    const char* name;
    void (*fn)();
};

std::vector<Case>& registry();
extern int failures;

inline void fail(const char* file, int line, const std::string& msg) {
    ++failures;
    std::fprintf(stderr, "  FAIL %s:%d: %s\n", file, line, msg.c_str());
}

struct Reg {
    Reg(const char* name, void (*fn)()) { registry().push_back(Case{name, fn}); }
};

// 整数比较统一到公共类型，避免 -Wsign-compare
template <typename A, typename B>
bool eq(const A& a, const B& b) {
    if constexpr (std::is_integral_v<A> && std::is_integral_v<B>) {
        using C = std::common_type_t<A, B>;
        return static_cast<C>(a) == static_cast<C>(b);
    } else {
        return a == b;
    }
}

}  // namespace chat::test

#define TEST_CASE(name)                                     \
    static void name();                                     \
    static ::chat::test::Reg reg_##name(#name, name);       \
    static void name()

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) ::chat::test::fail(__FILE__, __LINE__, #cond);           \
    } while (0)

#define CHECK_EQ(a, b)                                                              \
    do {                                                                            \
        const auto& _a = (a);                                                       \
        const auto& _b = (b);                                                       \
        if (!::chat::test::eq(_a, _b)) {                                            \
            std::ostringstream _os;                                                 \
            _os << #a << " == " << #b << " (got " << _a << " vs " << _b << ")";      \
            ::chat::test::fail(__FILE__, __LINE__, _os.str());                      \
        }                                                                           \
    } while (0)
