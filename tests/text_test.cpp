// Unit test for sf::utf8 / wide / ansi / win_error.

#include "common/text.hpp"

#include "test_util.hpp"

using test::check;

int main() {
    test::exit_on_crash();
    check(sf::utf8(L"caf\u00e9") == "caf\xc3\xa9", "utf8 encodes non-ASCII");
    check(sf::wide("caf\xc3\xa9") == L"caf\u00e9", "wide decodes UTF-8");
    check(sf::utf8(sf::wide("\xd0\x9f\xd1\x80\xd0\xb8")) == "\xd0\x9f\xd1\x80\xd0\xb8", "utf8/wide round trip (Cyrillic)");

    auto plain = sf::ansi(L"C:\\Games\\star conflict");
    check(plain && *plain == "C:\\Games\\star conflict", "ansi keeps plain ASCII");
    // A Chinese character exists in no Western code page. Skip if the system code page is
    // UTF-8 (65001), where everything is representable.
    if (GetACP() != CP_UTF8) {
        check(!sf::ansi(L"C:\\Users\\\u4e2d"), "ansi rejects a character the code page can't hold");
    } else {
        std::printf("SKIP: system code page is UTF-8\n");
    }

    std::string e = sf::win_error(ERROR_FILE_NOT_FOUND);
    check(test::contains(e, "(Windows error 2)") && e.size() > 20, "win_error gives text and the code");

    return test::failures ? 1 : 0;
}
