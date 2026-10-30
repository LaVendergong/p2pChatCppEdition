#include "test_framework.h"

namespace chat::test {

std::vector<Case>& registry() {
    static std::vector<Case> r;
    return r;
}

int failures = 0;

}  // namespace chat::test

int main() {
    int passed = 0;
    int failed_cases = 0;
    for (const auto& c : chat::test::registry()) {
        const int before = chat::test::failures;
        c.fn();
        if (chat::test::failures == before) {
            std::printf("PASS %s\n", c.name);
            ++passed;
        } else {
            std::printf("FAIL %s\n", c.name);
            ++failed_cases;
        }
    }
    std::printf("\n%d passed, %d failed, %d assertion failures\n", passed, failed_cases, chat::test::failures);
    return chat::test::failures == 0 ? 0 : 1;
}
