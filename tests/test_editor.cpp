#include "editor.h"
#include "test_framework.h"

using namespace chat;

TEST_CASE(editor_insert_ascii) {
    Editor e;
    e.insert("hello");
    CHECK_EQ(e.text(), std::string("hello"));
    CHECK_EQ(e.cursor(), 5u);
    e.move_home();
    e.insert("> ");
    CHECK_EQ(e.text(), std::string("> hello"));
    CHECK_EQ(e.cursor(), 2u);
}

TEST_CASE(editor_utf8_backspace) {
    Editor e;
    e.insert("你好");
    CHECK_EQ(e.text(), std::string("你好"));
    CHECK(e.backspace());
    CHECK_EQ(e.text(), std::string("你"));
    CHECK(e.backspace());
    CHECK_EQ(e.text(), std::string(""));
    CHECK(!e.backspace());
}

TEST_CASE(editor_mid_insert_wide_char) {
    // "a中b": a(1B) 中(3B) b(1B)，游标是字节偏移
    Editor e;
    e.insert("ab");
    e.move_left();
    e.insert("中");
    CHECK_EQ(e.text(), std::string("a中b"));
    CHECK_EQ(e.cursor(), 4u);
    e.move_right();
    CHECK_EQ(e.cursor(), 5u);  // 末尾
    e.move_right();
    CHECK_EQ(e.cursor(), 5u);  // 不越界
    e.move_left();
    CHECK_EQ(e.cursor(), 4u);  // 'b' 之前
    e.move_left();
    CHECK_EQ(e.cursor(), 1u);  // '中' 之前
}

TEST_CASE(editor_delete_forward) {
    Editor e;
    e.insert("中文abc");
    e.move_home();
    CHECK(e.delete_forward());
    CHECK_EQ(e.text(), std::string("文abc"));
    CHECK(e.delete_forward());
    CHECK_EQ(e.text(), std::string("abc"));
}

TEST_CASE(editor_kill_operations) {
    Editor e;
    e.insert("hello world");
    e.move_home();
    for (int i = 0; i < 6; ++i) e.move_right();
    e.kill_to_end();
    CHECK_EQ(e.text(), std::string("hello "));
    e.kill_to_start();
    CHECK_EQ(e.text(), std::string(""));

    Editor f;
    f.insert("one two three");
    f.kill_word();
    CHECK_EQ(f.text(), std::string("one two "));
    f.kill_word();
    CHECK_EQ(f.text(), std::string("one "));
}

TEST_CASE(editor_history) {
    Editor e;
    e.history_push("first");
    e.history_push("second");
    e.insert("draft");
    CHECK(e.history_prev());
    CHECK_EQ(e.text(), std::string("second"));
    CHECK(e.history_prev());
    CHECK_EQ(e.text(), std::string("first"));
    CHECK(!e.history_prev());
    CHECK(e.history_next());
    CHECK_EQ(e.text(), std::string("second"));
    CHECK(e.history_next());
    CHECK_EQ(e.text(), std::string("draft"));  // 回到编辑中的草稿
    // 连续相同项不重复入栈
    e.history_push("second");
    e.history_push("second");
    CHECK_EQ(e.history_size(), 2u);
}
